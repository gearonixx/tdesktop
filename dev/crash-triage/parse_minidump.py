#!/usr/bin/env python3
"""Dependency-free Breakpad minidump triage.

Reads the *binary* structures (not strings) to recover, per dump:
  - the signal that killed the process (ExceptionCode)  -> SIGABRT vs SIGSEGV
  - the faulting address (ExceptionAddress)
  - the module that the crashing thread's instruction pointer (rip) is in
  - any libstdc++ hardening assertion text present

This is the ground truth for "controlled abort (DoS)" vs "wild fault (maybe worse)".
Read-only. Usage: parse_minidump.py <dump.dmp> [more...]
"""
import struct, sys, re

# stream types
EXCEPTION_STREAM = 6
MODULE_LIST      = 4
SIGNALS = {4:"SIGILL",5:"SIGTRAP",6:"SIGABRT",7:"SIGBUS",8:"SIGFPE",11:"SIGSEGV",
           2:"SIGINT",9:"SIGKILL",15:"SIGTERM"}

def u32(b,o): return struct.unpack_from("<I",b,o)[0]
def u64(b,o): return struct.unpack_from("<Q",b,o)[0]

def read_mdstring(b, rva):
    n = u32(b, rva)                      # length in BYTES of UTF-16
    raw = b[rva+4:rva+4+n]
    try: return raw.decode("utf-16-le", "replace")
    except Exception: return "<?>"

def parse(path):
    b = open(path,"rb").read()
    if b[:4] != b"MDMP":
        return {"file":path,"error":"not a minidump (bad magic)"}
    stream_count = u32(b,8)
    dir_rva      = u32(b,12)
    streams = {}
    for i in range(stream_count):
        off = dir_rva + i*12
        st  = u32(b,off); size = u32(b,off+4); rva = u32(b,off+8)
        streams.setdefault(st,(size,rva))
    out = {"file":path,"signal":None,"signal_name":"?","fault_addr":None,
           "si_code":None,"rip":None,"rip_module":"?","assert":None,"modules":0}

    # ---- module list (base,size,name) ----
    mods = []
    if MODULE_LIST in streams:
        _,rva = streams[MODULE_LIST]
        nmod = u32(b,rva); p = rva+4
        for _ in range(nmod):
            base = u64(b,p); size = u32(b,p+8); name_rva = u32(b,p+20)
            try: name = read_mdstring(b, name_rva)
            except Exception: name = "?"
            mods.append((base,size,name)); p += 108
    out["modules"] = len(mods)
    def mod_of(addr):
        if addr is None: return "?"
        for base,size,name in mods:
            if base <= addr < base+size:
                return name.split("/")[-1]
        return "<not-in-any-module>"

    # ---- exception stream ----
    if EXCEPTION_STREAM in streams:
        _,rva = streams[EXCEPTION_STREAM]
        code       = u32(b, rva+8)      # signal
        fault_addr = u64(b, rva+24)
        si_code    = u64(b, rva+40)     # ExceptionInformation[0]
        ctx_rva    = u32(b, rva+8+4+4+8+8+4+4+15*8 + 4)  # LOCATION_DESCRIPTOR.rva
        out["signal"] = code
        out["signal_name"] = SIGNALS.get(code, f"sig{code}")
        out["fault_addr"] = fault_addr
        out["si_code"] = si_code
        # AMD64 context (breakpad has only 6 debug regs) -> rip at offset 248
        try:
            rip = u64(b, ctx_rva + 248)
            out["rip"] = rip
            out["rip_module"] = mod_of(rip)
        except Exception:
            pass

    # ---- assertion text (from raw bytes; decisive corroboration) ----
    m = re.search(rb"Assertion '([^']{1,80})' failed", b)
    if m: out["assert"] = m.group(1).decode("latin1")
    e = re.search(rb"reference = ([A-Za-z0-9_:<> ]{1,60})&;", b)
    if e: out["vec_elem"] = e.group(1).decode("latin1")
    return out

def classify(r):
    if r.get("error"): return "ERROR"
    sig = r["signal"]
    if sig == 6:  # SIGABRT
        if r.get("assert"): return "ABORT/hardening-assert (DoS)"
        return "ABORT (DoS)"
    if sig == 11:
        fa = r["fault_addr"] or 0
        kind = "near-null" if fa < 0x10000 else ("canonical-wild" if fa < 0x800000000000 else "non-canonical")
        return f"SIGSEGV @0x{fa:x} ({kind})"
    if sig is None: return "no-exception-stream"
    return f"{r['signal_name']}"

if __name__ == "__main__":
    for p in sys.argv[1:]:
        try: r = parse(p)
        except Exception as e: r = {"file":p,"error":str(e)}
        cls = classify(r)
        elem = r.get("vec_elem","")
        print("{:42s} | {:22s} | rip_in={:20s} | assert={} {}".format(
            p.split("/")[-1],
            f"{r.get('signal_name','?')} {cls}",
            str(r.get("rip_module","?"))[:20],
            (r.get("assert") or "-"),
            (f"[vec:{elem}]" if elem else "")))
