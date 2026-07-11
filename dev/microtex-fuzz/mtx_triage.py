#!/usr/bin/env python3
"""
mtx_triage — crash-corpus triage & memory-corruption hunter for the MicroTeX
LaTeX/TeX fuzzing campaigns in this folder.

Goal: take a pile of libFuzzer/AFL crash inputs (thousands, mostly duplicates of
a few root causes) and boil it down to a small, ranked table of *distinct* bugs,
with each bug tagged by exploit-relevant primitive so RCE-grade issues
(out-of-bounds WRITE, use-after-free, double-free, type confusion) stand out from
the DoS-grade majority (OOB read / SEGV / timeout).

It does NOT modify the vendored MicroTeX library. It only *runs* an
ASan-instrumented `fuzz_tex` (or the local ASan build here) against inputs and
parses the reports.

Thermal note: this machine has throttled at ~93C under heavy parallel fuzzing.
Keep -j modest (default 4) and prefer running when no big campaign is active.

Usage:
    ./mtx_triage.py run   <crash_dir> [--bin PATH] [-j N] [--sample N]
                          [--timeout S] [--out DIR]
    ./mtx_triage.py report <out_dir>            # re-print tables from saved run.json

Typical:
    ./mtx_triage.py run \
        ../../Telegram/ThirdParty/MicroTeX/fuzz/crashes5h \
        --bin ../../Telegram/ThirdParty/MicroTeX/fuzz/build/fuzz_tex \
        --sample 1000 -j 4 --out triage_run
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import random
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, asdict, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Classification of ASan/libFuzzer error classes by exploit relevance.
# RCE-grade = a write, a lifetime bug, or a type confusion that can, in
# principle, be turned into control-flow / arbitrary-memory effects.
# DoS-grade = read faults / resource exhaustion: crash only.
# ---------------------------------------------------------------------------
RCE_GRADE = {
    "heap-buffer-overflow",       # only WRITE variant is RCE-grade; see is_write
    "stack-buffer-overflow",
    "global-buffer-overflow",
    "heap-use-after-free",
    "stack-use-after-return",
    "use-after-poison",
    "double-free",
    "bad-free",
    "alloc-dealloc-mismatch",
    "dynamic-stack-buffer-overflow",
}
DOS_GRADE = {
    "SEGV", "FPE", "ABRT", "stack-overflow", "TIMEOUT", "OOM",
    "negative-size-param", "requested-allocation-size-exceeds-maximum",
}

ERR_RE = re.compile(r"SUMMARY: AddressSanitizer: ([A-Za-z0-9_-]+)")
ERR_RE2 = re.compile(r"ERROR: AddressSanitizer: ([A-Za-z0-9_-]+)")
RW_RE = re.compile(r"\b(READ|WRITE) of size (\d+)")
ADDR_RE = re.compile(r"on (?:unknown )?address (0x[0-9a-f]+)")
# a stack frame that lands inside the MicroTeX source tree
FRAME_RE = re.compile(r"#\d+ 0x[0-9a-f]+ in (\S+).*?MicroTeX/src/(\S+?:\d+)")
# top tex:: frame even if location column differs
TEXFRAME_RE = re.compile(r"#\d+ 0x[0-9a-f]+ in (tex::\S+)")
LOC_RE = re.compile(r"MicroTeX/src/(\S+?:\d+)")
UBSAN_RE = re.compile(r"runtime error: (.+)")


@dataclass
class CrashRecord:
    input: str
    err_class: str = "UNKNOWN"
    access: str = ""          # READ / WRITE / ""
    access_size: int = 0
    fault_addr: str = ""
    top_frame: str = ""       # first tex:: frame
    loc: str = ""             # file:line inside MicroTeX/src
    ubsan: str = ""           # UBSan message if that's what fired
    grade: str = "UNKNOWN"    # RCE / DoS / UNKNOWN
    size: int = 0             # input length in bytes

    @property
    def bucket_key(self) -> str:
        return f"{self.err_class}|{self.access}|{self.top_frame}|{self.loc}"


def classify(rec: CrashRecord, log: str) -> None:
    m = ERR_RE.search(log) or ERR_RE2.search(log)
    if m:
        rec.err_class = m.group(1)
    elif "libFuzzer: timeout" in log or "ERROR: libFuzzer: timeout" in log:
        rec.err_class = "TIMEOUT"
    elif "out-of-memory" in log:
        rec.err_class = "OOM"
    elif (u := UBSAN_RE.search(log)):
        rec.err_class = "ubsan"
        rec.ubsan = u.group(1)[:120]

    if (rw := RW_RE.search(log)):
        rec.access = rw.group(1)
        rec.access_size = int(rw.group(2))
    if (a := ADDR_RE.search(log)):
        rec.fault_addr = a.group(1)

    # top in-tree frame + location
    tf = TEXFRAME_RE.search(log)
    if tf:
        rec.top_frame = tf.group(1)
    loc = LOC_RE.search(log)
    if loc:
        rec.loc = loc.group(1)

    rec.grade = grade_of(rec)


def grade_of(rec: CrashRecord) -> str:
    c = rec.err_class
    if c in RCE_GRADE:
        # *-buffer-overflow is only RCE-grade on WRITE; a READ overflow is DoS
        if c.endswith("buffer-overflow") and rec.access == "READ":
            return "DoS"
        return "RCE"
    # dynamic_cast SEGV reading a bad vtable = candidate type confusion
    if c == "SEGV" and (
        "dynamic_cast" in rec.top_frame or "dynamic_pointer_cast" in rec.top_frame
    ):
        return "RCE?"  # type-confusion candidate — needs manual review
    if c in DOS_GRADE or c == "ubsan":
        return "DoS"
    return "UNKNOWN"


def run_one(bin_path: str, inp: Path, timeout: int, asan_env: dict) -> CrashRecord:
    rec = CrashRecord(input=inp.name, size=inp.stat().st_size)
    try:
        p = subprocess.run(
            [bin_path, str(inp)],
            capture_output=True, text=True, timeout=timeout, env=asan_env,
        )
        log = p.stdout + p.stderr
    except subprocess.TimeoutExpired:
        rec.err_class = "TIMEOUT"
        rec.grade = "DoS"
        return rec
    classify(rec, log)
    return rec


def cmd_run(args: argparse.Namespace) -> int:
    crash_dir = Path(args.crash_dir)
    bin_path = args.bin
    if not Path(bin_path).exists():
        print(f"error: fuzz binary not found: {bin_path}", file=sys.stderr)
        return 2
    files = sorted(p for p in crash_dir.iterdir() if p.is_file())
    if args.sample and args.sample < len(files):
        random.seed(args.seed)
        files = random.sample(files, args.sample)
    print(f"[mtx_triage] {len(files)} inputs  bin={bin_path}  -j{args.jobs}  "
          f"timeout={args.timeout}s", file=sys.stderr)

    out = Path(args.out)
    (out / "logs").mkdir(parents=True, exist_ok=True)
    (out / "repro").mkdir(parents=True, exist_ok=True)

    asan_env = dict(os.environ)
    asan_env["ASAN_OPTIONS"] = ("detect_leaks=0:abort_on_error=0:symbolize=1:"
                                "handle_abort=1:handle_segv=1:print_summary=1")

    records: list[CrashRecord] = []
    done = 0
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_one, bin_path, f, args.timeout, asan_env): f
                for f in files}
        for fut in cf.as_completed(futs):
            rec = fut.result()
            records.append(rec)
            done += 1
            if done % 100 == 0:
                print(f"  ...{done}/{len(files)}", file=sys.stderr)

    # bucket & keep the SMALLEST input per bucket as the representative repro
    buckets: dict[str, dict] = {}
    for r in records:
        b = buckets.setdefault(r.bucket_key, {"count": 0, "rep": None})
        b["count"] += 1
        if b["rep"] is None or r.size < b["rep"].size:
            b["rep"] = r

    ranked = sorted(
        buckets.values(),
        key=lambda b: ({"RCE": 0, "RCE?": 1, "UNKNOWN": 2, "DoS": 3}
                       .get(b["rep"].grade, 4), -b["count"]),
    )

    # copy representative inputs out for reporting/upstream
    for i, b in enumerate(ranked):
        rep = b["rep"]
        src = crash_dir / rep.input
        dst = out / "repro" / f"{i:02d}_{rep.grade}_{rep.err_class}_{rep.input[:16]}"
        try:
            shutil.copy(src, dst)
        except OSError:
            pass

    result = {
        "crash_dir": str(crash_dir),
        "bin": bin_path,
        "total_scanned": len(records),
        "distinct_buckets": len(buckets),
        "buckets": [
            {"count": b["count"], **asdict(b["rep"])} for b in ranked
        ],
    }
    (out / "run.json").write_text(json.dumps(result, indent=2))
    print_report(result)
    print(f"\n[mtx_triage] wrote {out/'run.json'} and {len(ranked)} repro inputs "
          f"to {out/'repro'}", file=sys.stderr)
    return 0


def _repro_text(crash_dir: str, name: str) -> str:
    try:
        data = (Path(crash_dir) / name).read_bytes()[:48]
        return data.decode("latin-1").replace("\n", "\\n").replace("\t", "\\t")
    except OSError:
        return ""


def print_report(result: dict) -> None:
    buckets = result["buckets"]
    n_rce = sum(1 for b in buckets if b["grade"] in ("RCE", "RCE?"))
    print("\n" + "=" * 78)
    print(f"MicroTeX crash triage — {result['total_scanned']} inputs scanned, "
          f"{result['distinct_buckets']} distinct buckets, "
          f"{n_rce} RCE-grade/candidate")
    print("=" * 78)
    hdr = f"{'#':>2}  {'grade':<5} {'count':>6}  {'class':<22} {'acc':<5} {'top frame @ loc'}"
    print(hdr)
    print("-" * 78)
    for i, b in enumerate(buckets):
        frame = b["top_frame"] or "(non-tex)"
        loc = b["loc"] or ""
        acc = b["access"] or "-"
        print(f"{i:>2}  {b['grade']:<5} {b['count']:>6}  {b['err_class']:<22} "
              f"{acc:<5} {frame} @ {loc}")
        rep = _repro_text(result["crash_dir"], b["input"])
        if rep:
            print(f"      repro[{b['size']}B]: {rep!r}")
    print("-" * 78)
    print("grade: RCE=write/UAF/double-free/type-confusion  RCE?=dynamic_cast "
          "type-confusion candidate  DoS=read/segv/timeout")


def cmd_report(args: argparse.Namespace) -> int:
    result = json.loads((Path(args.out_dir) / "run.json").read_text())
    print_report(result)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="triage a crash directory")
    r.add_argument("crash_dir")
    r.add_argument("--bin", default="../../Telegram/ThirdParty/MicroTeX/fuzz/build/fuzz_tex")
    r.add_argument("-j", "--jobs", type=int, default=4)
    r.add_argument("--sample", type=int, default=0, help="0 = all")
    r.add_argument("--timeout", type=int, default=10)
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--out", default="triage_run")
    r.set_defaults(func=cmd_run)

    rp = sub.add_parser("report", help="re-print tables from a run dir")
    rp.add_argument("out_dir")
    rp.set_defaults(func=cmd_report)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
