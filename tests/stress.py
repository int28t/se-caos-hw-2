#!/usr/bin/env python3
"""Повторные нагруженные запуски с проверкой каждого журнала."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time
import traceback

from check_log import read_events, verify


SCENARIOS = {
    "period": (
        "ticks 100\ntarget 0\ntick_ms 2\nmin_green 2\nmax_green 5\nclearance 1\n"
        "emergency_rate 250\n"
        "stream N car north straight 2 2 1000\n"
        "stream S car south straight 2 2 1000\n"
        "stream E car east straight 2 3 1000\n"
        "stream W car west straight 2 3 1000\n"
        "stream P pedestrian north walk 1 1 700\n"
        "conflict N S 0\nconflict E W 0\n"
        "phase NS N S\nphase EW E W\nphase pedestrians P\n"
    ),
    "target": (
        "ticks 0\ntarget 80\ntick_ms 2\nmin_green 2\nmax_green 5\nclearance 1\n"
        "emergency_rate 250\n"
        "stream N car north straight 3 2 850\n"
        "stream S car south straight 3 2 850\n"
        "stream E car east straight 3 2 850\n"
        "stream W car west straight 3 2 850\n"
        "conflict N S 0\nconflict E W 0\nphase NS N S\nphase EW E W\n"
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--seeds", nargs="+", type=int, default=[0, 1, 39, 4294967295])
    parser.add_argument("--repeat", type=int, default=os.environ.get("STRESS_REPEATS", "3"))
    parser.add_argument("--timeout", type=int, default=os.environ.get("STRESS_TIMEOUT", "10"))
    args = parser.parse_args()
    if args.repeat < 1 or args.timeout < 1 or any(not 0 <= seed <= 4294967295 for seed in args.seeds):
        parser.error("repeat/timeout must be positive; seeds must fit uint32_t")
    binary = args.binary.resolve()
    output = binary.parent / "stress"
    output.mkdir(exist_ok=True)
    report = {"platform": platform.platform(), "status": "passed", "runs": []}
    started = time.monotonic()
    case = None
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        config, journal = work / "input.conf", work / "output.log"
        console, errors = work / "console.log", work / "stderr.log"
        try:
            for strategy in ("fixed", "adaptive"):
                for name, contents in SCENARIOS.items():
                    for seed in args.seeds:
                        for repeat in range(1, args.repeat + 1):
                            case = dict(strategy=strategy, scenario=name, seed=seed, repeat=repeat)
                            config.write_text(contents + f"strategy {strategy}\nseed {seed}\n")
                            journal.unlink(missing_ok=True)
                            with console.open("wb") as out, errors.open("wb") as err:
                                result = subprocess.run([str(binary), str(config), str(journal)],
                                                        stdout=out, stderr=err, timeout=args.timeout)
                            assert result.returncode == 0, errors.read_text()
                            assert console.read_bytes() == journal.read_bytes()
                            assert not errors.read_bytes(), errors.read_text()
                            events = read_events(journal.read_text())
                            summary = verify(events)
                            reason = next(d["причина"] for _, e, d in events if e == "Завершение")
                            if name == "target":
                                assert reason == "цель" and summary["прошло"] == 80 and not summary["в_зоне"]
                            else:
                                assert reason == "период" and summary["отказов"] > 0 and summary["прошло"] > 0
                            report["runs"].append(dict(case, summary=summary))
        except Exception:
            report.update(status="failed", case=case, error=traceback.format_exc())
            failure = output / "failure"
            failure.mkdir(exist_ok=True)
            for path in work.iterdir():
                shutil.copyfile(path, failure / path.name)
            print(report["error"], end="")
            print(f"Failed case: {case}; files: {failure}")
    report["duration_seconds"] = round(time.monotonic() - started, 3)
    (output / "summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(f"Stress: {len(report['runs'])} runs passed; status={report['status']}; report={output / 'summary.json'}")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
