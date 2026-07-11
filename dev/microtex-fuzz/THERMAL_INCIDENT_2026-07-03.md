# Thermal / resource incident — 2026-07-03 (AFL out3 campaign)

Live readings captured during the session (source: `sensors`, `top -bn1`, `free -h`,
`vmstat`, `/proc/pressure/*`). NOT continuous logs — single snapshots. Times are local.

## Peak snapshot — 07:18:45 (fuzzers running, 6 AFL instances, ~2h37m in)

`sensors` → coretemp-isa-0000:
    Package id 0:  +93.0 C  (high +105.0, crit +105.0)   <-- the "93C" figure
    hottest core:  Core 20 +94.0 C
    Core 16 +90, Core 24 +86, Core 0/11 +82, others 78-80, Core 32/33 +68

`sensors` → acpitz-acpi-0:      temp1 +92.0 C
`sensors` → thinkpad-isa-0000:  CPU +92.0 C ; fan1 4490 RPM ; fan2 4484 RPM ; pwm1 128% ; temp5 +85 C
`sensors` → nvme Composite:     +59.9 C (high +79.8, crit +82.8)

top:    load avg 9.96 ; %Cpu us 32.8 sy 9.7 id 11.1 **wa 44.7**
free:   Mem 30Gi total, 22Gi used, ~0.65Gi free ; **Swap 12Gi used / 23Gi**
vmstat: r=6-10, **b=22** (blocked on I/O) ; si/so 75/280
PSI io: some=76% full=49%   (system stalled on I/O ~half the time)

## Post-stop snapshot — 07:22:18 (fuzzers killed)

    Package id 0:  +51.0 C     CPU +50.0 C     Composite +48.9 C
    load avg 0.98 ; %Cpu us 2.6 **wa 71.7** (paging swap back in)
    Swap still 13Gi used ; Mem 1.6Gi free

## Notes / honesty

- 93C = single live `sensors` reading, not a persisted log. Only other copy is the chat scrollback.
- Throttle counters seen (`core_throttle_count` sum 110,973,134 ; `package` 10,089,901) are
  CUMULATIVE SINCE BOOT (21h uptime) -- they do NOT cleanly measure this 2.6h incident.
- Fuzzers' own RAM was negligible; the 12-13Gi swap was desktop apps (CLion ~2Gi, Firefox ~1Gi
  + web content) + page cache, squeezed while AFL churned disk (32,707 queue files).
- Lid was closed / half-closed while running unattended (user asleep) -> restricted airflow;
  the 93C was held WITH fans maxed (128%) and CPU already throttling. Margin to 105C shutdown
  was ~12C but cooling was saturated. Chip self-protects (HW shutdown at limit) so no runaway,
  but sustained heat next to the battery is the real cumulative-wear risk.

## INDEPENDENT RECORD — kernel journal (journalctl)

Temps themselves are not logged, BUT the kernel logs every time the package crosses its
throttle trip point (~100C PROCHOT). This is a real, timestamped, independent record.

Dense throttle cluster DURING the run (each = package hit trip point, clock throttled):
    07:11:12 ... 07:13:05 ... 07:14:40 ... 07:15:38 ... 07:16:11 (CPU10-13) ... 07:16:35 ... 07:17:01
    07:17:01  "Package temperature/speed normal"  (last event before I stopped fuzzers ~07:19-07:22)

=> The chip was repeatedly BOUNCING OFF its throttle limit (~100C) in the minutes before the
   93C sensors snapshot -- i.e. peaks touched the throttle point, hotter than the 93C read alone.
First throttle event THIS boot: Jul 02 11:54:34 -- so intermittent throttling had been happening
all day, not only this incident (cumulative counters reflect that). The 07:11-07:17 cluster is
this incident specifically.

Check it yourself:
    journalctl -k -b | grep -iE 'temperature is above threshold|speed normal'

## How to check temperature yourself (live)
    sensors                       # look at 'Package id 0' under coretemp-isa-0000
    cat /sys/class/thermal/thermal_zone*/type /sys/class/thermal/thermal_zone*/temp
