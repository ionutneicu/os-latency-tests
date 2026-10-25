#!/usr/bin/env python3
"""Render the HTML/Markdown report from a recorded run and from variants of it.

Usage: report_fixtures.py REPORT_SCRIPT FIXTURE WORK_DIR

FIXTURE is a results.json from a real run (tests/fixtures/report-results.json,
trimmed to three results per benchmark). Each variant below is written to
WORK_DIR/<name>/results.json and rendered in a child process. A variant passes
when the report exits with status 0, writes report.html and report.md, and the
Markdown has no Python artefacts (None, nan) in it.

The variants cover what the report meets in practice: results from older
versions without newer fields, values recorded as null, and a run with no
results. A short run has no p99_999 key at all, and the recorded variant covers
that. Before #29 both the recorded and the null variant crashed the report.
"""

import copy
import json
import re
import subprocess
import sys
from pathlib import Path

DEEPEST = "p99_99"


def as_recorded(doc):
    return doc


def without_newer_fields(doc):
    # Results written before these fields existed: no CPU caches, mitigations,
    # thp mode, kernel preemption model, RT priority or hugepage setting.
    d = copy.deepcopy(doc)
    for k in ("caches", "cpu_vulnerabilities", "thp_mode", "preempt_model", "governor",
              "cpuidle_driver", "isolated_cpus", "physical_memory"):
        d["platform"].pop(k, None)
    for k in ("rt_priority", "hugepages", "lock_memory", "block_deep_idle", "idle_limit", "cpu_warmup_ms"):
        d["config"].pop(k, None)
    return d


def null_deep_percentile(doc):
    # A percentile the run could not compute is recorded as null.
    d = copy.deepcopy(doc)
    for r in d["results"]:
        if DEEPEST in r["percentiles"]:
            r["percentiles"][DEEPEST] = None
    return d


def second_wakeup_cluster(doc):
    # Wake-up p90 five times p50: the design hint for a second cluster (C1E, idle exit) fires.
    d = copy.deepcopy(doc)
    for r in d["results"]:
        if r["name"] == "sync.mutex_wakeup" and "p50" in r["percentiles"]:
            r["percentiles"]["p90"] = 5 * r["percentiles"]["p50"]
    return d


def no_results(doc):
    d = copy.deepcopy(doc)
    d["results"] = []
    return d


VARIANTS = {
    "recorded": as_recorded,
    "older-fields": without_newer_fields,
    "null-percentile": null_deep_percentile,
    "second-cluster": second_wakeup_cluster,
    "no-results": no_results,
}

FORBIDDEN = re.compile(r"\bNone\b|\bnan\b")


def render(script, work, name, doc):
    run_dir = work / name
    run_dir.mkdir(parents=True, exist_ok=True)
    results = run_dir / "results.json"
    results.write_text(json.dumps(doc, indent=1))
    proc = subprocess.run([sys.executable, str(script), str(results), "-o", str(run_dir / "report.html")],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return f"{name}: report exited {proc.returncode}: {proc.stderr.strip().splitlines()[-1:]}"
    md_path = run_dir / "report.md"
    if not (run_dir / "report.html").is_file() or not md_path.is_file():
        return f"{name}: report.html or report.md was not written"
    hit = FORBIDDEN.search(md_path.read_text())
    if hit:
        return f"{name}: report.md contains a Python value: {hit.group(0)!r}"
    print(f"ok  {name}")
    return None


def main(argv):
    if len(argv) != 4:
        sys.exit(__doc__)
    script, fixture, work = Path(argv[1]), Path(argv[2]), Path(argv[3])
    base = json.loads(fixture.read_text())
    failures = [f for f in (render(script, work, name, make(base)) for name, make in VARIANTS.items()) if f]
    for f in failures:
        print("FAIL " + f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
