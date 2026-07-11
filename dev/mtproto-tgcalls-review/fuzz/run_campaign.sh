#!/usr/bin/env bash
cd /home/x/c/tdesktop/dev/mtproto-tgcalls-review/fuzz
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:symbolize=1
# fork=4: 4 parallel procs, robust to ooms/timeouts, continues; stops only on a real crash.
exec ./build/fuzz_tl -fork=4 -ignore_ooms=1 -ignore_timeouts=1 -ignore_crashes=1 \
  -rss_limit_mb=2048 -malloc_limit_mb=512 -timeout=10 -max_total_time=2400 \
  -artifact_prefix=crashes/ -print_final_stats=1 corpus
