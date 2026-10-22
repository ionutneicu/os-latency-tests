#!/usr/bin/env python3
"""Render an os-latency-tests results.json as an HTML report plus SVG charts.

    tools/olt-report.py olt-results/results.json [-o report.html]

Uses only the Python standard library, so it also runs on the target board.
Writes one self-contained HTML file (inline SVG, works offline, light and dark
mode, hover readouts) and every chart as a standalone .svg file in charts/
next to it, ready for slides and documents.

Charts:
  * memory hierarchy: random load latency and sequential bandwidth against
    working-set size, with the machine's cache sizes drawn in, so steps in the
    curves can be matched to L1/L2/LLC capacity;
  * latency distributions (percentile curves, p0 to p99.99+) for every group
    of latency benchmarks (wake-ups, thread lifecycle, ...), from raw samples;
  * malloc/free cost against block size.
Plus a per-level memory summary, design hints and a full percentile table.
"""

import argparse
import csv
import html
import json
import math
import statistics
import sys
from pathlib import Path

# ---------------------------------------------------------------- formatting


def fmt_bytes(n, compact=False):
    for unit, size in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if n >= size:
            s = f"{n / size:.3g}"
            return f"{s}{unit[0]}" if compact else f"{s} {unit}"
    return f"{int(n)}" if compact else f"{int(n)} B"


def fmt_ns(v):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "n/a"
    for unit, div in (("s", 1e9), ("ms", 1e6), ("us", 1e3)):
        if abs(v) >= div:
            v /= div
            break
    else:
        unit = "ns"
    return f"{v:.3g} {unit}"


def fmt_num(v):
    return f"{v:.3g}"


def parse_size_label(s):
    """'16', '4K', '1M' -> bytes."""
    mult = {"K": 1 << 10, "M": 1 << 20, "G": 1 << 30}
    s = s.strip()
    if s and s[-1].upper() in mult:
        return int(float(s[:-1]) * mult[s[-1].upper()])
    return int(float(s))


# ---------------------------------------------------------------- data access


def data_caches(platform):
    """Data/unified caches, smallest level first, one entry per level."""
    seen = {}
    for c in platform.get("caches", []):
        if c.get("type") == "Instruction":
            continue
        seen.setdefault(c["level"], c)
    return [seen[k] for k in sorted(seen)]


def sweep_series(results, name):
    pts = []
    for r in results:
        if r["name"] != name or "bytes" not in r.get("params", {}):
            continue
        p = r["percentiles"]
        pts.append({"x": int(r["params"]["bytes"]), "p50": p.get("p50"), "lo": r.get("min"), "hi": p.get("p90")})
    return sorted(pts, key=lambda d: d["x"])


def percentile_sorted(v, p):
    if not v:
        return None
    h = (len(v) - 1) * p / 100.0
    lo = int(math.floor(h))
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (h - lo) * (v[hi] - v[lo])


def min_samples_for(p):
    return 1 if p <= 50 else math.ceil(1000.0 / (100.0 - p) - 1e-6)


PCT_GRID = [0, 25, 50, 75, 90, 95, 99, 99.5, 99.9, 99.95, 99.99, 99.995, 99.999]


def distribution(result, base_dir):
    """(percentile, value) points for a result, from raw samples when available.

    Only percentiles with at least ~10 samples beyond them are kept."""
    raw = result.get("raw_csv")
    samples = None
    if raw and (base_dir / raw).is_file():
        with open(base_dir / raw, newline="") as f:
            rd = csv.reader(f)
            next(rd, None)
            samples = sorted(float(row[1]) for row in rd if len(row) > 1)
    if samples:
        n = len(samples)
        return [(p, percentile_sorted(samples, p)) for p in PCT_GRID if n >= min_samples_for(p)], samples[-1]
    pts = [(0, result["min"])]
    unrel = set(result.get("unreliable_percentiles", []))
    for k, v in result["percentiles"].items():
        if k not in unrel:
            pts.append((float(k[1:].replace("_", ".")), v))
    return sorted(pts), result["max"]


# ---------------------------------------------------------------- SVG charts

W, H = 1000, 420
M = {"l": 72, "r": 20, "t": 30, "b": 78}

CHART_STYLE_LIGHT = """
  .bg{fill:#fcfcfb} .ax{fill:#52514e;font:12px system-ui,sans-serif} .title{fill:#0b0b0b;font:600 15px system-ui,sans-serif}
  .grid{stroke:#e4e3df} .zone-a{fill:#f3f2ef} .zone-b{fill:#fcfcfb} .marker{stroke:#8a8984}
  .zlabel{fill:#8a8984;font:600 12px system-ui,sans-serif} .hit{fill:transparent}
  .s1{stroke:#2a78d6} .s2{stroke:#eb6834} .s3{stroke:#1baf7a} .s4{stroke:#6250d6}
  .f1{fill:#2a78d6} .f2{fill:#eb6834} .f3{fill:#1baf7a} .f4{fill:#6250d6}
"""


class Chart:
    """Line chart on a log2-bytes or 'nines' (percentile) x axis."""

    def __init__(self, cid, title, x_kind, y_log, y_label, unit, series, zones=(), markers=(), note=""):
        self.cid, self.title, self.x_kind, self.y_log = cid, title, x_kind, y_log
        self.y_label, self.unit, self.series = y_label, unit, [s for s in series if s["points"]]
        self.zones, self.markers, self.note = list(zones), list(markers), note

    # x mapping
    def xv(self, x):
        if self.x_kind == "bytes":
            return math.log2(x)
        return -math.log10(max(1e-9, 1 - x / 100.0))  # nines: p50 -> 0.3, p99 -> 2, p99.9 -> 3

    def x_label(self, x):
        if self.x_kind == "bytes":
            return fmt_bytes(x)
        return "min" if x == 0 else f"p{x:g}"

    def y_fmt(self, v):
        return fmt_ns(v) if self.unit == "ns" else f"{fmt_num(v)} {self.unit}"

    def render(self, standalone=False):
        if not self.series:
            return ""
        xs = sorted({p[0] for s in self.series for p in s["points"]})
        ys = [v for s in self.series for p in s["points"] for v in p[1:] if v is not None and v > 0]
        x0, x1 = self.xv(xs[0]), self.xv(xs[-1])
        x1 = x1 if x1 > x0 else x0 + 1
        pw, ph = W - M["l"] - M["r"], H - M["t"] - M["b"]
        X = lambda x: M["l"] + (self.xv(x) - x0) / (x1 - x0) * pw
        if self.y_log:
            lo, hi = math.log10(min(ys) * 0.8), math.log10(max(ys) * 1.25)
            Y = lambda v: M["t"] + ph - (math.log10(max(v, 1e-12)) - lo) / (hi - lo) * ph
            yticks = [m * 10 ** e for e in range(math.floor(lo), math.ceil(hi) + 1) for m in (1, 2, 5)
                      if lo <= math.log10(m * 10 ** e) <= hi]
        else:
            top = max(ys) * 1.1
            Y = lambda v: M["t"] + ph - v / top * ph
            step = 10 ** math.floor(math.log10(top / 4))
            step *= next(k for k in (1, 2, 5, 10) if top / (k * step) <= 6)
            yticks = [i * step for i in range(int(top / step) + 1)]

        o = []
        o.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H + (24 if standalone else 0)}" '
                 f'role="img" aria-label="{html.escape(self.title)}">')
        if standalone:
            o.append(f"<style>{CHART_STYLE_LIGHT}</style>")
            o.append(f'<rect class="bg" width="{W}" height="{H + 24}"/>')
            o.append(f'<text class="title" x="{M["l"]}" y="18">{html.escape(self.title)}</text>')
            o.append('<g transform="translate(0,24)">')

        # cache-level zones: alternating bands labelled at the top
        for i, z in enumerate(self.zones):
            a = max(X(max(z["from"], xs[0])), M["l"]) if z["from"] else M["l"]
            b = min(X(min(z["to"], xs[-1])), M["l"] + pw)
            if b <= a:
                continue
            o.append(f'<rect class="{"zone-a" if i % 2 == 0 else "zone-b"}" x="{a:.1f}" y="{M["t"]}" '
                     f'width="{b - a:.1f}" height="{ph}"/>')
            o.append(f'<text class="zlabel" x="{(a + b) / 2:.1f}" y="{M["t"] - 10}" text-anchor="middle">'
                     f'{html.escape(z["label"])}</text>')
        for v in yticks:
            o.append(f'<line class="grid" x1="{M["l"]}" x2="{M["l"] + pw}" y1="{Y(v):.1f}" y2="{Y(v):.1f}"/>')
            label = self.y_fmt(v) if self.unit == "ns" else fmt_num(v)
            o.append(f'<text class="ax" x="{M["l"] - 8}" y="{Y(v) + 4:.1f}" text-anchor="end">{label}</text>')
        # x ticks
        if self.x_kind == "bytes":
            e0, e1 = math.ceil(math.log2(xs[0])), math.floor(math.log2(xs[-1]))
            stride = 1 if e1 - e0 <= 12 else 2
            ticks = [2 ** e for e in range(e0, e1 + 1) if (e - e0) % stride == 0]
            tick_label = lambda x: fmt_bytes(x, compact=True)
        else:
            ticks = [p for p in (0, 50, 90, 99, 99.9, 99.99, 99.999) if xs[0] <= p <= xs[-1]]
            tick_label = self.x_label
        for t in ticks:
            o.append(f'<text class="ax" x="{X(t):.1f}" y="{M["t"] + ph + 18}" text-anchor="middle">{tick_label(t)}</text>')
        for mk in self.markers:
            if xs[0] <= mk["x"] <= xs[-1]:
                o.append(f'<line class="marker" x1="{X(mk["x"]):.1f}" x2="{X(mk["x"]):.1f}" y1="{M["t"]}" '
                         f'y2="{M["t"] + ph}" stroke-dasharray="4 3"/>')
        xl = "working set (buffer size)" if self.x_kind == "bytes" else "percentile (log scale of nines)"
        if self.x_kind == "bytes" and self.cid.startswith("alloc"):
            xl = "block size"
        o.append(f'<text class="ax" x="{M["l"] + pw / 2}" y="{M["t"] + ph + 40}" text-anchor="middle">{xl}</text>')
        o.append(f'<text class="ax" x="16" y="{M["t"] + ph / 2}" transform="rotate(-90 16 {M["t"] + ph / 2})" '
                 f'text-anchor="middle">{html.escape(self.y_label)}</text>')

        # series: optional band, then a 2px line
        for s in self.series:
            pts = s["points"]
            if s.get("band") and all(len(p) == 4 and p[2] is not None and p[3] is not None for p in pts):
                poly = [f"{X(p[0]):.1f},{Y(p[3]):.1f}" for p in pts] + [f"{X(p[0]):.1f},{Y(p[2]):.1f}" for p in reversed(pts)]
                o.append(f'<polygon class="f{s["slot"]}" opacity="0.15" points="{" ".join(poly)}"/>')
            line = " ".join(f"{X(p[0]):.1f},{Y(p[1]):.1f}" for p in pts)
            dash = ' stroke-dasharray="6 4"' if s.get("dash") else ""
            o.append(f'<polyline class="s{s["slot"]}" fill="none" stroke-width="2" stroke-linejoin="round"{dash} points="{line}"/>')
            if len(pts) <= 16:  # few points: mark them
                for p in pts:
                    o.append(f'<circle class="f{s["slot"]}" cx="{X(p[0]):.1f}" cy="{Y(p[1]):.1f}" r="3"/>')

        # legend (always for >= 2 series)
        if len(self.series) > 1:
            lx, ly = M["l"], H - 12
            for s in self.series:
                dash = ' stroke-dasharray="6 4"' if s.get("dash") else ""
                o.append(f'<line class="s{s["slot"]}" stroke-width="2"{dash} x1="{lx}" x2="{lx + 22}" y1="{ly - 4}" y2="{ly - 4}"/>')
                o.append(f'<text class="ax" x="{lx + 28}" y="{ly}">{html.escape(s["name"])}</text>')
                lx += 40 + 7 * len(s["name"])

        # hover columns (HTML only): one target per measured x with its readout
        if not standalone:
            bounds = [M["l"]] + [(X(a) + X(b)) / 2 for a, b in zip(xs, xs[1:])] + [M["l"] + pw]
            for i, x in enumerate(xs):
                rows = [f"<b>{html.escape(self.x_label(x))}</b>"]
                zone = next((z for z in self.zones if z["from"] < x <= z["to"]), None)
                if zone:
                    rows[0] += f' <span class="note">fits {html.escape(zone["label"])}</span>'
                for s in self.series:
                    p = next((q for q in s["points"] if q[0] == x), None)
                    if p:
                        rows.append(f'<i class="f{s["slot"]}"></i>{html.escape(s["name"])}: <b>{self.y_fmt(p[1])}</b>')
                tip = html.escape("<br>".join(rows), quote=True)
                o.append(f'<rect class="hit" x="{bounds[i]:.1f}" y="{M["t"]}" width="{bounds[i + 1] - bounds[i]:.1f}" '
                         f'height="{ph}" data-x="{X(x):.1f}" data-tip="{tip}"/>')
            o.append(f'<line class="cross" x1="0" x2="0" y1="{M["t"]}" y2="{M["t"] + ph}" visibility="hidden"/>')
        if standalone:
            o.append("</g>")
        o.append("</svg>")
        return "\n".join(o)


# ---------------------------------------------------------------- analysis


def level_zones(caches, max_bytes):
    zones, lo = [], 0
    for c in caches:
        zones.append({"from": lo, "to": c["size_bytes"], "label": f"L{c['level']}"})
        lo = c["size_bytes"]
    zones.append({"from": lo, "to": max(max_bytes, lo * 2), "label": "DRAM"})
    return zones


def level_summary(lat, rd, wr, zones, line):
    rows = []
    for z in zones:
        lo = z["from"] * 1.5 if z["from"] else 0
        hi = z["to"] * 0.75 if z["label"] != "DRAM" else float("inf")

        def med(series, conv=lambda v: v):
            vals = [conv(p["p50"]) for p in series if lo <= p["x"] <= hi and p["p50"]]
            return statistics.median(vals) if vals else None

        rows.append({
            "level": z["label"],
            "range": (f"> {fmt_bytes(int(lo))}" if hi == float("inf")
                      else f"up to {fmt_bytes(int(hi))}" if lo == 0
                      else f"{fmt_bytes(int(lo))} – {fmt_bytes(int(hi))}"),
            "latency": med(lat),
            "read_gbs": med(rd, lambda v: line / v),
            "write_gbs": med(wr, lambda v: line / v),
        })
    return rows


def build_guidance(summary, caches, platform, results, cfg):
    tips = []
    by = {r["level"]: r for r in summary}
    l1 = next((c for c in caches if c["level"] == 1), None)
    l2 = next((c for c in caches if c["level"] == 2), None)
    if l1 and by.get("L1", {}).get("latency") and by.get("DRAM", {}).get("latency"):
        ratio = by["DRAM"]["latency"] / by["L1"]["latency"]
        tips.append(f"A random access that misses all caches costs about {ratio:.0f}x an L1 hit "
                    f"({fmt_ns(by['DRAM']['latency'])} vs {fmt_ns(by['L1']['latency'])}). Keep the data a hot loop "
                    f"touches under ~{fmt_bytes(int(l1['size_bytes'] * 0.75))} per core where possible.")
    if l2 and by.get("L2", {}).get("latency"):
        tips.append(f"Working sets up to ~{fmt_bytes(int(l2['size_bytes'] * 0.75))} still get L2 latency "
                    f"({fmt_ns(by['L2']['latency'])}): size per-thread buffers, lookup tables and queues to fit there.")
    if by.get("DRAM", {}).get("read_gbs") and by.get("DRAM", {}).get("latency"):
        line = platform.get("cache_line_size", 64)
        tips.append(f"Beyond the last cache, sequential reads cost {fmt_ns(line / by['DRAM']['read_gbs'])} per "
                    f"{line}-byte line vs {fmt_ns(by['DRAM']['latency'])} per random access: prefer contiguous "
                    f"arrays and streaming over pointer-heavy structures.")
    named = {r["name"]: r for r in results if not r.get("params")}
    wake = named.get("sync.mutex_wakeup")
    if wake:
        p50, p99 = wake["percentiles"].get("p50"), wake["percentiles"].get("p99")
        if p50 and wake["min"] and p50 > 3 * wake["min"] and not cfg.get("block_deep_idle"):
            tips.append(f"Blocking wake-up p50 ({fmt_ns(p50)}) is {p50 / wake['min']:.0f}x its minimum "
                        f"({fmt_ns(wake['min'])}): most of it is likely the sleeping core leaving a deep idle state. "
                        f"Compare with a --no-idle run; latency-critical threads should spin/poll or run with deep idle blocked.")
        elif p50:
            tips.append(f"A blocking hand-off (mutex/condvar wake-up) costs {fmt_ns(p50)} typically and "
                        f"{fmt_ns(p99)} at p99: avoid blocking on the critical path below that budget.")
    create = named.get("thread.create_to_start")
    if create and create["percentiles"].get("p99"):
        tips.append(f"Starting a thread costs {fmt_ns(create['percentiles']['p50'])} typically but "
                    f"{fmt_ns(create['percentiles']['p99'])} at p99 (max {fmt_ns(create['max'])}): reuse threads "
                    f"from a pool instead of spawning per task.")
    return tips


# ---------------------------------------------------------------- page

CSS = """
:root {
  color-scheme: light;
  --surface: #fcfcfb; --text: #0b0b0b; --text-2: #52514e; --muted: #8a8984;
  --grid: #e4e3df; --zone-a: #f3f2ef; --zone-b: #fcfcfb;
  --s1: #2a78d6; --s2: #eb6834; --s3: #1baf7a; --s4: #6250d6;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --surface: #1a1a19; --text: #f0efec; --text-2: #c3c2b7; --muted: #8f8e88;
    --grid: #33332f; --zone-a: #232322; --zone-b: #1a1a19;
    --s1: #3987e5; --s2: #d95926; --s3: #199e70; --s4: #9085e9;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --surface: #1a1a19; --text: #f0efec; --text-2: #c3c2b7; --muted: #8f8e88;
  --grid: #33332f; --zone-a: #232322; --zone-b: #1a1a19;
  --s1: #3987e5; --s2: #d95926; --s3: #199e70; --s4: #9085e9;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--surface); color: var(--text);
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 1040px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { font-size: 22px; margin: 0 0 4px; }
h2 { font-size: 17px; margin: 32px 0 4px; }
nav { display: flex; flex-wrap: wrap; gap: 4px 16px; margin: 8px 0 16px; font-size: 13px; }
nav a { color: var(--text-2); }
p.sub { color: var(--text-2); margin: 0 0 12px; }
dl.plat { display: grid; grid-template-columns: max-content 1fr; gap: 2px 16px; margin: 12px 0; }
dl.plat dt { color: var(--text-2); }
dl.plat dd { margin: 0; overflow-wrap: anywhere; }
.chart { position: relative; margin: 8px 0 4px; }
.chart svg { width: 100%; height: auto; display: block; }
.chart .ax { fill: var(--text-2); font-size: 12px; }
.chart .zlabel { fill: var(--muted); font-size: 12px; font-weight: 600; }
.chart .grid { stroke: var(--grid); }
.chart .zone-a { fill: var(--zone-a); } .chart .zone-b { fill: var(--zone-b); }
.chart .marker { stroke: var(--muted); }
.chart .cross { stroke: var(--text-2); }
.chart .hit { fill: transparent; }
.s1 { stroke: var(--s1); } .s2 { stroke: var(--s2); } .s3 { stroke: var(--s3); } .s4 { stroke: var(--s4); }
.f1 { fill: var(--s1); background: var(--s1); } .f2 { fill: var(--s2); background: var(--s2); }
.f3 { fill: var(--s3); background: var(--s3); } .f4 { fill: var(--s4); background: var(--s4); }
.chart circle { stroke: var(--surface); stroke-width: 1.5; }
.dl { font-size: 12px; color: var(--text-2); }
.dl a { color: var(--text-2); }
.tip { position: absolute; pointer-events: none; background: var(--surface); color: var(--text);
  border: 1px solid var(--grid); border-radius: 6px; padding: 6px 8px; font-size: 12px; top: 28px;
  box-shadow: 0 2px 8px rgba(0,0,0,.12); white-space: nowrap; display: none; z-index: 1; }
.tip b { font-weight: 600; }
.tip i { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin-right: 4px; }
.table-wrap { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; font-size: 13px; }
th, td { text-align: right; padding: 4px 8px; border-bottom: 1px solid var(--grid); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { color: var(--text-2); font-weight: 600; }
td.unrel { color: var(--muted); }
ul.tips li { margin: 4px 0; }
.note { color: var(--text-2); font-size: 12px; }
.missing { color: var(--text-2); border: 1px dashed var(--grid); border-radius: 6px; padding: 8px 12px; }
"""

JS = r"""
document.querySelectorAll('.chart').forEach(chart => {
  const svg = chart.querySelector('svg'); if (!svg) return;
  const cross = svg.querySelector('.cross');
  const tip = document.createElement('div'); tip.className = 'tip'; chart.appendChild(tip);
  const W = svg.viewBox.baseVal.width;
  svg.querySelectorAll('.hit').forEach(h => {
    h.addEventListener('pointerenter', () => {
      const x = +h.dataset.x;
      cross.setAttribute('x1', x); cross.setAttribute('x2', x); cross.setAttribute('visibility', 'visible');
      tip.innerHTML = h.dataset.tip; tip.style.display = 'block';
      const r = svg.getBoundingClientRect(), px = x / W * r.width;
      tip.style.left = (px > r.width * 0.6 ? px - tip.offsetWidth - 12 : px + 12) + 'px';
    });
  });
  svg.addEventListener('pointerleave', () => { tip.style.display = 'none'; cross.setAttribute('visibility', 'hidden'); });
});
"""

GROUP_TITLES = {
    "sync": ("Blocking wake-up latency", "Time from one thread releasing a mutex / signalling a condition variable to "
             "the blocked thread running again."),
    "thread": ("Thread lifecycle", "Cost of starting a new thread and of joining a finished one."),
}


def build_report(doc, src_path, charts_dir):
    plat = doc["platform"]
    cfg = doc.get("config", {})
    results = doc["results"]
    base = src_path.parent
    line = plat.get("cache_line_size") or 64
    caches = data_caches(plat)
    e = html.escape

    sections = []      # (anchor, title, html)
    charts = []        # Chart objects, also written as standalone SVG

    def chart_block(ch):
        charts.append(ch)
        return (f"<div class='chart'>{ch.render()}</div>"
                f"<div class='dl'><a href='{e(charts_dir.name)}/{e(ch.cid)}.svg'>{e(ch.cid)}.svg</a></div>")

    # ---- memory hierarchy
    lat = sweep_series(results, "mem.latency_random")
    rd = sweep_series(results, "mem.read_seq")
    wr = sweep_series(results, "mem.write_seq")
    summary, mem_html = [], []
    if lat:
        max_x = max(p["x"] for p in lat)
        zones = level_zones(caches, max_x) if caches else []
        markers = [{"x": c["size_bytes"]} for c in caches]
        mem_html.append("<p class='sub'>Dependent loads in random order through a buffer of the given size, so "
                        "hardware prefetchers cannot help. Each step up is the working set spilling out of a cache "
                        "level; dashed lines are the cache sizes reported by the OS. Band: min to p90 across samples.</p>")
        mem_html.append(chart_block(Chart(
            "mem-latency", "Random load latency vs working set", "bytes", True, "ns per load (log)", "ns",
            [{"name": "random load latency (p50)", "slot": 1, "band": True,
              "points": [(p["x"], p["p50"], p["lo"], p["hi"]) for p in lat]}], zones, markers)))
        if not cfg.get("hugepages"):
            mem_html.append("<p class='note'>Base pages were used, so latency past the TLB reach also includes page "
                            "walks. Re-run with <code>--hugepages</code> to separate TLB misses from cache misses.</p>")
        mem_html.append("<p class='sub'>Streaming read and write over the same buffer: prefetchers are effective, so "
                        "the drops are smaller than for random access and show each level's bandwidth.</p>")
        mem_html.append(chart_block(Chart(
            "mem-bandwidth", "Sequential bandwidth vs working set", "bytes", False, "GB/s", "GB/s",
            [{"name": "read", "slot": 1, "points": [(p["x"], line / p["p50"]) for p in rd]},
             {"name": "write", "slot": 2, "points": [(p["x"], line / p["p50"]) for p in wr]}], zones, markers)))
        if zones:
            summary = level_summary(lat, rd, wr, zones, line)
            gb = lambda v: f"{v:.3g} GB/s" if v else "n/a"
            mem_html.append("<h3>Per-level summary</h3><div class='table-wrap'><table><tr><th>data fits in</th>"
                            "<th>sizes used</th><th>random load</th><th>seq read</th><th>seq write</th></tr>"
                            + "".join(f"<tr><td>{e(r['level'])}</td><td>{e(r['range'])}</td><td>{fmt_ns(r['latency'])}</td>"
                                      f"<td>{gb(r['read_gbs'])}</td><td>{gb(r['write_gbs'])}</td></tr>" for r in summary)
                            + "</table></div>")
    else:
        mem_html.append("<p class='missing'>The memory sweep was not part of this run. Run "
                        "<code>os-latency-tests</code> without <code>-t</code> (or with <code>-t 'mem.*'</code>) "
                        "to get the cache graphs.</p>")
    sections.append(("memory", "Memory hierarchy", "\n".join(mem_html)))

    # ---- latency distributions, one chart per benchmark group
    groups = {}
    for r in results:
        if r["params"] or r["name"].startswith(("mem.", "alloc.")):
            continue
        groups.setdefault(r["name"].split(".")[0], []).append(r)
    for g, rs in groups.items():
        title, desc = GROUP_TITLES.get(g, (f"{g} latency", ""))
        series = []
        for i, r in enumerate(rs[:4]):
            pts, mx = distribution(r, base)
            series.append({"name": f"{r['name'].split('.', 1)[1]} (max {fmt_ns(mx)})", "slot": i + 1, "points": pts})
        body = (f"<p class='sub'>{e(desc)} The curve reads left to right from the fastest sample to the tail: "
                f"the further right it rises, the rarer but costlier the outliers. Percentiles with fewer than ~10 "
                f"samples beyond them are not drawn.</p>")
        body += chart_block(Chart(f"{g}-percentiles", f"{title}: latency by percentile", "nines", True,
                                  "latency (log)", "ns", series))
        sections.append((g, title, body))

    # ---- allocator
    alloc = [r for r in results if r["name"] in ("alloc.malloc", "alloc.free") and "size" in r["params"]]
    if alloc:
        def pts(name, key):
            return sorted((parse_size_label(r["params"]["size"]), r["percentiles"][key] if key != "min" else r["min"])
                          for r in alloc if r["name"] == name)
        body = ("<p class='sub'>Cost of one <code>malloc()</code> and one <code>free()</code> per block size, in the "
                "allocator's best case (the block is reused every iteration and never touched). Solid: p50, dashed: p99.</p>")
        body += chart_block(Chart("alloc-size", "malloc / free latency vs block size", "bytes", True, "latency (log)", "ns",
                                  [{"name": "malloc p50", "slot": 1, "points": pts("alloc.malloc", "p50")},
                                   {"name": "malloc p99", "slot": 1, "dash": True, "points": pts("alloc.malloc", "p99")},
                                   {"name": "free p50", "slot": 2, "points": pts("alloc.free", "p50")},
                                   {"name": "free p99", "slot": 2, "dash": True, "points": pts("alloc.free", "p99")}]))
        sections.append(("alloc", "Heap allocator", body))

    # ---- hints
    tips = build_guidance(summary, caches, plat, results, cfg)
    if tips:
        sections.insert(0, ("hints", "Design hints for this machine",
                            "<ul class='tips'>" + "".join(f"<li>{e(t)}</li>" for t in tips) + "</ul>"))

    # ---- full table
    other = [r for r in results if not r["name"].startswith("mem.")]
    if other:
        pcols = ["p50", "p90", "p99", "p99_9", "p99_99"]
        rows = []
        for r in other:
            label = r["name"] + (" [" + ", ".join(f"{k}={v}" for k, v in r["params"].items()) + "]" if r["params"] else "")
            unrel = set(r.get("unreliable_percentiles", []))
            cells = "".join(f"<td class='{'unrel' if c in unrel else ''}'>{fmt_ns(r['percentiles'].get(c))}</td>" for c in pcols)
            rows.append(f"<tr><td>{e(label)}</td><td>{r['count']}</td><td>{fmt_ns(r['min'])}</td>{cells}"
                        f"<td>{fmt_ns(r['max'])}</td></tr>")
        sections.append(("table", "All latency results",
                         "<p class='sub'>Grey values have fewer than ~10 samples beyond the percentile and are not "
                         "statistically reliable.</p><div class='table-wrap'><table><tr><th>benchmark</th><th>n</th>"
                         "<th>min</th>" + "".join(f"<th>{c.replace('_', '.')}</th>" for c in pcols)
                         + "<th>max</th></tr>" + "".join(rows) + "</table></div>"))

    # ---- page
    out = ["<!doctype html><html lang='en'><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width, initial-scale=1'>"
           f"<title>Latency report</title><style>{CSS}</style></head><body><main>"]
    board = f" · {e(plat['board'])}" if plat.get("board") else ""
    out.append(f"<h1>Latency report</h1><p class='sub'>{e(plat.get('cpu_model', ''))}{board} · "
               f"{e(plat.get('arch', ''))} · Linux {e(plat.get('kernel_release', ''))} "
               f"[{e(plat.get('preempt_model', ''))}]</p>")

    def sharing(c):
        cpus = c.get("shared_cpus", "")
        if not cpus:
            return ""
        return "per core" if ("-" not in cpus and cpus.count(",") <= 1 and c["level"] < 3) else f"shared by CPUs {cpus}"

    cache_str = "; ".join(f"L{c['level']} {fmt_bytes(c['size_bytes'])}"
                          + ", ".join([""] + [s for s in (f"{c['ways']}-way" if c.get("ways") else "", sharing(c)) if s])
                          for c in caches)
    perf = plat.get("perf", {})
    rows = [
        ("CPUs", f"{plat.get('cpu_count')} online" + (f", isolated {plat['isolated_cpus']}" if plat.get("isolated_cpus") else "")),
        ("Data caches", cache_str or "unknown"),
        ("Cache line", f"{line} B"),
        ("Memory", fmt_bytes(plat["physical_memory"]) if plat.get("physical_memory") else "unknown"),
        ("Huge pages", f"THP {plat.get('thp_mode') or 'n/a'}; sweep used {'huge' if cfg.get('hugepages') else 'base'} pages"),
        ("Power", f"governor {plat.get('governor') or 'n/a'}, cpuidle {plat.get('cpuidle_driver') or 'n/a'}, deep idle "
                  f"{'blocked' if cfg.get('block_deep_idle') else 'allowed'} during run"),
        ("Perf counters", f"{perf.get('backend', 'none')}: {', '.join(perf.get('events', [])) or 'none available'}"),
        ("Run", f"{cfg.get('samples')} samples, affinity {cfg.get('affinity')} (CPU {cfg.get('main_cpu')}/{cfg.get('worker_cpu')}), "
                f"RT priority {cfg.get('rt_priority') or 'default'}, mlock {'on' if cfg.get('lock_memory') else 'off'}"),
        ("Compiler", f"{plat.get('compiler', '')} ({plat.get('build_type', '')})"),
    ]
    out.append("<dl class='plat'>" + "".join(f"<dt>{e(k)}</dt><dd>{e(str(v))}</dd>" for k, v in rows) + "</dl>")
    out.append("<nav>" + "".join(f"<a href='#{a}'>{e(t)}</a>" for a, t, _ in sections) + "</nav>")
    for a, t, body in sections:
        out.append(f"<h2 id='{a}'>{e(t)}</h2>{body}")
    out.append(f"<p class='note'>Generated from {e(src_path.name)} by tools/olt-report.py. "
               f"Charts are also saved as standalone SVG files in {e(charts_dir.name)}/.</p>")
    out.append(f"<script>{JS}</script></main></body></html>")
    return "\n".join(out), charts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("results", help="results.json written by os-latency-tests")
    ap.add_argument("-o", "--output", help="output HTML file (default: report.html next to results.json)")
    args = ap.parse_args()

    src = Path(args.results)
    try:
        doc = json.loads(src.read_text())
    except (OSError, ValueError) as ex:
        sys.exit(f"error: cannot read {src}: {ex}")
    out = Path(args.output) if args.output else src.with_name("report.html")
    charts_dir = out.with_name("charts")
    page, charts = build_report(doc, src, charts_dir)
    try:
        charts_dir.mkdir(parents=True, exist_ok=True)
        for ch in charts:
            (charts_dir / f"{ch.cid}.svg").write_text(ch.render(standalone=True))
        out.write_text(page)
    except PermissionError:
        sys.exit(f"error: cannot write {out} (directory created by a sudo run?). "
                 f"Fix with: sudo chown -R $USER: {out.parent}  or pass -o ~/report.html")
    print(f"report written to {out} ({len(charts)} charts, SVGs in {charts_dir}/)")


if __name__ == "__main__":
    main()
