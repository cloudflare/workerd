"""Measure startup, case execution and shutdown without changing test budgets."""
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time

binary = Path("bazel-bin/src/workerd/server/workerd.exe").resolve()
assert binary.is_file(), binary
config = Path("src/workerd/api/tests/persistent-stubs-test.wd-test").resolve()
results = []
for run in range(1, 6):
    with tempfile.TemporaryDirectory(prefix="persistent-timing-") as storage:
        started = time.monotonic()
        events = []
        process = subprocess.Popen(
            [str(binary), "test", str(config), "--experimental", "--predictable",
             "--compat-date=2000-01-01", "-dTEST_TMPDIR=" + storage],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, encoding="utf-8", errors="replace",
        )

        def collect():
            for line in process.stdout:
                events.append(dict(seconds=time.monotonic() - started, line=line.rstrip()))

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        timed_out = False
        try:
            process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            process.wait(timeout=10)
        elapsed = time.monotonic() - started
        reader.join(timeout=5)
        Path(f"persistent-timing-{run}.json").write_text(json.dumps(events, indent=2) + "\n")
        case_events = [event for event in events if "[ TEST ]" in event["line"]]
        passes = [event for event in events if "[ PASS ]" in event["line"]]
        failures = [event for event in events if "[ FAIL ]" in event["line"]]
        result = dict(run=run, elapsed=elapsed, timed_out=timed_out,
                      exit_code=process.returncode, cases=len(case_events), passes=len(passes),
                      first_case=case_events[0]["seconds"] if case_events else None,
                      last_pass=passes[-1]["seconds"] if passes else None)
        results.append(result)
        print(json.dumps(result), flush=True)
        assert not timed_out and process.returncode == 0 and not failures, result
        assert len(passes) == len(case_events) and len(passes) > 0, result
Path("persistent-timing-summary.json").write_text(json.dumps(results, indent=2) + "\n")
