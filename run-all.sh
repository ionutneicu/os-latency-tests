#!/bin/sh
# Build (if needed), run every benchmark and render the HTML report + SVG charts.
#
#   ./run-all.sh              full run; uses sudo for RT priority, mlock and
#                             blocked deep idle (most trustworthy tails)
#   ./run-all.sh --quick      fewer samples, coarser memory sweep (~20 s)
#   ./run-all.sh --no-sudo    unprivileged run (default scheduling, idle allowed)
#   ./run-all.sh -- ARGS...   extra os-latency-tests options, e.g. -- --hugepages
#
# Results land in results/<host>-<timestamp>/ (results.json, raw/, report.html, report.md, charts/).
set -eu

cd "$(dirname "$0")"
privileged=1
quick=0
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) quick=1 ;;
        --no-sudo) privileged=0 ;;
        --) shift; break ;;
        -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done

echo "==> building"
# Configure on every run, so a first configure that failed (for example before
# g++ was installed) is retried and CMake repairs flags it cached empty (#12).
# The build type defaults to RelWithDebInfo; an existing choice is kept.
cmake -S . -B build >/dev/null
cmake --build build -j >/dev/null

out="results/$(hostname)-$(date +%Y%m%d-%H%M%S)"
set -- -o "$out" "$@"
if [ "$quick" = 1 ]; then
    set -- -n 2000 --sweep-steps 2 --sweep-samples 7 "$@"
fi

runner=""
if [ "$privileged" = 1 ]; then
    set -- -p 80 -m --no-idle "$@"
    if [ "$(id -u)" != 0 ]; then
        if command -v sudo >/dev/null 2>&1; then
            runner="sudo"
            echo "==> sudo is used for SCHED_FIFO, mlockall and blocking deep idle (use --no-sudo to skip)"
        else
            echo "==> sudo not found, running unprivileged" >&2
            shift 4  # drop the privileged options
        fi
    fi
fi

echo "==> running benchmarks (results in $out)"
$runner ./build/os-latency-tests "$@"

echo "==> rendering report"
if command -v python3 >/dev/null 2>&1; then
    python3 tools/olt-report.py "$out/results.json"
    if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$out/report.html" >/dev/null 2>&1 &
    fi
    echo "==> open $out/report.html (also report.md; charts as SVG in $out/charts/)"
else
    echo "python3 not found: copy $out to a machine with Python 3 and run tools/olt-report.py there" >&2
fi
