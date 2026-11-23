# Working on os-latency-tests

A C++17 benchmark suite for Linux (x86-64 and aarch64). A wrong number is worse
than no number: accuracy comes before features.

## Read first

1. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): the layers, the lifecycle of one measurement, the `results.json` contract, and the decisions already made.
2. [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md): what else is affected when one part changes.
3. [docs/REQUIREMENTS.md](docs/REQUIREMENTS.md): what a benchmark must satisfy before its numbers are shown.

## Rules

- **Before changing a benchmark, a shared helper (`src/benchmarks/*_util.hpp`), the engine or the HAL**, find that part in the dependency map. Every benchmark listed against it can change too: run those again and compare with the run before your change.
- **When you add** a benchmark, a shared helper or a cross-reference between results, add it to the dependency map in the same change.
- **When you change** what a layer owns, the measurement lifecycle or the `results.json` fields, update ARCHITECTURE in the same change.
- The report script matches results by name and parameter keys. Rename both sides together, or the section disappears without an error.
- Intrinsics, assembly and compiler builtins live only in the HAL (`include/olt/hal/`, `src/hal/`).
- Do not reopen a decision recorded in ARCHITECTURE section 6 without new evidence.
- Check that numbers are physically plausible before reporting them (REQUIREMENTS section 3).

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`./run-all.sh` builds, runs every benchmark and writes the report. Timings from
a container, a VM or CI show that the code runs, not that the numbers are right.
