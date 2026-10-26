"""Read-only comparison of guest and host clock rates; does not adjust either clock."""
import argparse
import json
from pathlib import Path
import subprocess
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--distro", default="Ubuntu")
    parser.add_argument("--seconds", type=int, choices=range(2, 61), default=10, metavar="2..60")
    parser.add_argument("--output", default="tests/results/wsl-clocks")
    args = parser.parse_args()
    source = """
import json, platform, sys, time
from pathlib import Path
base = Path('/sys/devices/system/clocksource/clocksource0')
print(json.dumps({'kernel':platform.release(), 'clocksource':(base/'current_clocksource').read_text().strip()}), flush=True)
for i in range(int(sys.argv[1]) * 4 + 1):
    print(json.dumps({'linux_monotonic_ns':time.monotonic_ns(),
                      'linux_raw_ns':time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW),
                      'linux_realtime_ns':time.time_ns()}), flush=True)
    time.sleep(.25)
"""
    root = Path(args.output).resolve()
    root.mkdir(parents=True, exist_ok=True)
    prefix = root / time.strftime("%Y%m%d-%H%M%S")
    rows, metadata, errors = [], {}, []
    with prefix.with_suffix(".stderr.log").open("w", encoding="utf-8") as log:
        child = subprocess.Popen(["wsl", "-d", args.distro, "--exec", "python3", "-u", "-c", source, str(args.seconds)],
                                 stdout=subprocess.PIPE, stderr=log, text=True,
                                 creationflags=subprocess.CREATE_NO_WINDOW)

        def read():
            try:
                for line in child.stdout:
                    received, realtime = time.monotonic_ns(), time.time_ns()
                    row = json.loads(line)
                    if "kernel" in row:
                        metadata.update(row)
                    else:
                        rows.append({**row, "windows_monotonic_ns":received, "windows_realtime_ns":realtime})
            except Exception as error:
                errors.append(str(error))

        reader = threading.Thread(target=read, daemon=True)
        reader.start()
        try:
            code = child.wait(timeout=args.seconds + 30)
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            reader.join(timeout=5)
        if code or errors or reader.is_alive() or len(rows) < 2:
            raise RuntimeError(f"Clock sampling failed: exit={code}, errors={errors}; see {prefix}.stderr.log")
    elapsed = {key:(rows[-1][key]-rows[0][key])/1e9 for key in rows[0]}
    drift = 100 * (elapsed["windows_monotonic_ns"] / elapsed["linux_monotonic_ns"] - 1)
    report = {"distro":args.distro, **metadata, "elapsed_seconds":elapsed,
              "host_over_guest_monotonic_percent":drift, "samples":rows,
              "note":"Host timestamps are pipe arrival times, so scheduling/transport delay also contributes. This is diagnostic evidence, not a network-quality acceptance test."}
    path = prefix.with_suffix(".json")
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Guest elapsed {elapsed['linux_monotonic_ns']:.6f}s; host elapsed {elapsed['windows_monotonic_ns']:.6f}s; difference {drift:.3f}%; {path}")


if __name__ == "__main__":
    main()
