#!/usr/bin/env python3
"""Render an os-latency-tests results.json as a self-contained HTML report.

    tools/olt-report.py olt-results/results.json -o report.html

Uses only the Python standard library and writes one HTML file with inline
SVG charts (no network access needed), so it also runs on the target board.

The report shows:
  * the memory-hierarchy sweep: load latency and sequential bandwidth against
    working-set size, with the machine's cache sizes drawn in, so steps in the
    curves can be matched to L1/L2/LLC capacity;
  * a per-level summary (typical latency/bandwidth when the data fits a level);
  * a table of every other benchmark with its percentiles.
"""

import argparse
import html
import json
import math
import statistics
import sys
from pathlib import Path


def fmt_bytes(n):
    for unit, size in (("GiB", 1 << 30), ("MiB", 1 << 20), ("KiB", 1 << 10)):
        if n >= size:
            v = n / size
            return f"{v:.3g} {unit}"
    return f"{n} B"


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
        pts.append({
            "x": int(r["params"]["bytes"]),
            "p50": p.get("p50"),
            "lo": r.get("min"),
            "hi": p.get("p90"),
        })
    return sorted(pts, key=lambda d: d["x"])


def level_zones(caches, max_bytes):
    """Size ranges in which the working set fits a given level."""
    zones, lo = [], 0
    for c in caches:
        zones.append({"from": lo, "to": c["size_bytes"], "label": f"L{c['level']}"})
        lo = c["size_bytes"]
    zones.append({"from": lo, "to": max(max_bytes, lo * 2), "label": "DRAM"})
    return zones


def level_summary(lat, rd, wr, zones, line):
    """Median of the sweep points well inside each zone (away from the edges)."""
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


def build_guidance(summary, caches, platform):
    tips = []
    by = {r["level"]: r for r in summary}
    l1 = next((c for c in caches if c["level"] == 1), None)
    l2 = next((c for c in caches if c["level"] == 2), None)
    if l1 and by.get("L1", {}).get("latency") and by.get("DRAM", {}).get("latency"):
        ratio = by["DRAM"]["latency"] / by["L1"]["latency"]
        tips.append(f"A random access that misses all caches costs about {ratio:.0f}x an L1 hit "
                    f"({fmt_ns(by['DRAM']['latency'])} vs {fmt_ns(by['L1']['latency'])}). Keep the data a hot loop "
                    f"touches under ~{fmt_bytes(int(l1['size_bytes'] * 0.75))} per core where possible.")
    if l2:
        tips.append(f"Working sets up to ~{fmt_bytes(int(l2['size_bytes'] * 0.75))} still get L2 latency: size "
                    f"per-thread buffers, lookup tables and queues to fit there.")
    if by.get("DRAM", {}).get("read_gbs") and by.get("DRAM", {}).get("latency"):
        line = platform.get("cache_line_size", 64)
        seq_per_line = line / by["DRAM"]["read_gbs"]
        tips.append(f"Beyond the last cache, sequential reads cost {fmt_ns(seq_per_line)} per {line}-byte line "
                    f"vs {fmt_ns(by['DRAM']['latency'])} per random access: prefer contiguous arrays and "
                    f"streaming over pointer-heavy structures.")
    return tips


CSS = """
:root {
  color-scheme: light;
  --surface: #fcfcfb; --surface-2: #f3f2ef; --text: #0b0b0b; --text-2: #52514e; --muted: #8a8984;
  --grid: #e4e3df; --zone-a: #f3f2ef; --zone-b: #fcfcfb; --marker: #8a8984;
  --s1: #2a78d6; --s2: #eb6834;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --surface: #1a1a19; --surface-2: #232322; --text: #f0efec; --text-2: #c3c2b7; --muted: #8f8e88;
    --grid: #33332f; --zone-a: #232322; --zone-b: #1a1a19; --marker: #8f8e88;
    --s1: #3987e5; --s2: #d95926;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --surface: #1a1a19; --surface-2: #232322; --text: #f0efec; --text-2: #c3c2b7; --muted: #8f8e88;
  --grid: #33332f; --zone-a: #232322; --zone-b: #1a1a19; --marker: #8f8e88;
  --s1: #3987e5; --s2: #d95926;
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--surface); color: var(--text);
  font: 14px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 1040px; margin: 0 auto; padding: 24px 16px 48px; }
h1 { font-size: 22px; margin: 0 0 4px; }
h2 { font-size: 17px; margin: 32px 0 4px; }
p.sub { color: var(--text-2); margin: 0 0 16px; }
dl.plat { display: grid; grid-template-columns: max-content 1fr; gap: 2px 16px; margin: 12px 0; }
dl.plat dt { color: var(--text-2); }
dl.plat dd { margin: 0; overflow-wrap: anywhere; }
.chart { position: relative; margin: 8px 0 4px; }
.chart svg { width: 100%; height: auto; display: block; }
.chart text { fill: var(--text-2); font-size: 11px; }
.chart .zone-label { fill: var(--muted); font-size: 11px; font-weight: 600; }
.legend { display: flex; gap: 16px; color: var(--text-2); font-size: 13px; margin: 4px 0; }
.legend span::before { content: ""; display: inline-block; width: 14px; height: 2px; margin-right: 6px;
  vertical-align: middle; background: var(--c); }
.tip { position: absolute; pointer-events: none; background: var(--surface); color: var(--text);
  border: 1px solid var(--grid); border-radius: 6px; padding: 6px 8px; font-size: 12px;
  box-shadow: 0 2px 8px rgba(0,0,0,.12); white-space: nowrap; display: none; }
.tip b { font-weight: 600; }
.tip i { display: inline-block; width: 8px; height: 8px; border-radius: 50%; background: var(--c); margin-right: 4px; }
.table-wrap { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; font-size: 13px; }
th, td { text-align: right; padding: 4px 8px; border-bottom: 1px solid var(--grid); white-space: nowrap; }
th:first-child, td:first-child { text-align: left; }
th { color: var(--text-2); font-weight: 600; }
td.unrel { color: var(--muted); }
ul.tips li { margin: 4px 0; }
.note { color: var(--text-2); font-size: 12px; }
"""

JS = r"""
const DATA = JSON.parse(document.getElementById('olt-data').textContent);

function fmtBytes(n) {
  const u = [['GiB', 2**30], ['MiB', 2**20], ['KiB', 2**10]];
  for (const [s, d] of u) if (n >= d) return +(n / d).toPrecision(3) + ' ' + s;
  return n + ' B';
}
function fmtNum(v, unit) { return (+v.toPrecision(3)) + ' ' + unit; }
function niceLogTicks(lo, hi) {
  const t = [];
  for (let e = Math.floor(Math.log10(lo)); e <= Math.ceil(Math.log10(hi)); e++)
    for (const m of [1, 2, 5]) { const v = m * 10 ** e; if (v >= lo && v <= hi) t.push(v); }
  return t;
}
function niceLinTicks(hi) {
  const step = 10 ** Math.floor(Math.log10(hi / 4)); const m = [1, 2, 5].find(k => hi / (k * step) <= 6) || 10;
  const s = m * step, t = []; for (let v = 0; v <= hi + 1e-9; v += s) t.push(v); return t;
}

function lineChart(el, cfg) {
  const W = 1000, H = 380, M = {l: 64, r: 16, t: 24, b: 40};
  const pw = W - M.l - M.r, ph = H - M.t - M.b;
  const xs = cfg.series.flatMap(s => s.points.map(p => p.x));
  const ys = cfg.series.flatMap(s => s.points.flatMap(p => [p.y, p.hi ?? p.y, p.lo ?? p.y])).filter(v => v > 0);
  if (!xs.length) { el.textContent = 'No sweep data in this run.'; return; }
  const x0 = Math.log2(Math.min(...xs)), x1 = Math.log2(Math.max(...xs));
  const X = v => M.l + (Math.log2(v) - x0) / (x1 - x0 || 1) * pw;
  let Y, yticks;
  if (cfg.ylog) {
    const lo = Math.min(...ys) * 0.8, hi = Math.max(...ys) * 1.25;
    Y = v => M.t + ph - (Math.log10(v) - Math.log10(lo)) / (Math.log10(hi) - Math.log10(lo)) * ph;
    yticks = niceLogTicks(lo, hi);
  } else {
    const hi = Math.max(...ys) * 1.1;
    Y = v => M.t + ph - v / hi * ph;
    yticks = niceLinTicks(hi);
  }
  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.setAttribute('role', 'img');
  svg.setAttribute('aria-label', cfg.title);
  const add = (tag, attrs, parent = svg) => { const e = document.createElementNS(ns, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]); parent.appendChild(e); return e; };

  // cache-level zones: alternating background so each step sits in a labelled band
  (cfg.zones || []).forEach((z, i) => {
    const a = Math.max(X(Math.max(z.from, 2 ** x0)), M.l), b = Math.min(X(Math.min(z.to, 2 ** x1)), M.l + pw);
    if (b <= a) return;
    add('rect', {x: a, y: M.t, width: b - a, height: ph, fill: i % 2 ? 'var(--zone-b)' : 'var(--zone-a)'});
    const t = add('text', {x: (a + b) / 2, y: M.t - 8, 'text-anchor': 'middle', class: 'zone-label'});
    t.textContent = z.label;
  });
  yticks.forEach(v => {
    add('line', {x1: M.l, x2: M.l + pw, y1: Y(v), y2: Y(v), stroke: 'var(--grid)', 'stroke-width': 1});
    const t = add('text', {x: M.l - 8, y: Y(v) + 4, 'text-anchor': 'end'}); t.textContent = +v.toPrecision(3);
  });
  for (let e = Math.ceil(x0); e <= Math.floor(x1); e++) {
    const v = 2 ** e; if (e % 2 && x1 - x0 > 12) continue;
    const t = add('text', {x: X(v), y: M.t + ph + 18, 'text-anchor': 'middle'}); t.textContent = fmtBytes(v).replace(' ', '');
  }
  (cfg.markers || []).forEach(m => {
    if (m.x < 2 ** x0 || m.x > 2 ** x1) return;
    add('line', {x1: X(m.x), x2: X(m.x), y1: M.t, y2: M.t + ph, stroke: 'var(--marker)', 'stroke-width': 1, 'stroke-dasharray': '4 3'});
  });
  const yl = add('text', {x: 14, y: M.t + ph / 2, transform: `rotate(-90 14 ${M.t + ph / 2})`, 'text-anchor': 'middle'});
  yl.textContent = cfg.yLabel;
  const xl = add('text', {x: M.l + pw / 2, y: H - 4, 'text-anchor': 'middle'}); xl.textContent = 'working set (buffer size)';

  cfg.series.forEach(s => {
    if (s.points.some(p => p.lo != null)) {
      const top = s.points.map(p => `${X(p.x)},${Y(p.hi)}`), bot = s.points.slice().reverse().map(p => `${X(p.x)},${Y(p.lo)}`);
      add('polygon', {points: top.concat(bot).join(' '), fill: s.color, opacity: 0.15});
    }
    add('polyline', {points: s.points.map(p => `${X(p.x)},${Y(p.y)}`).join(' '), fill: 'none', stroke: s.color,
      'stroke-width': 2, 'stroke-linejoin': 'round'});
  });

  // hover: crosshair snaps to the nearest measured size
  const cross = add('line', {y1: M.t, y2: M.t + ph, stroke: 'var(--text-2)', 'stroke-width': 1, visibility: 'hidden'});
  const dots = cfg.series.map(s => add('circle', {r: 4, fill: s.color, stroke: 'var(--surface)', 'stroke-width': 2, visibility: 'hidden'}));
  const hit = add('rect', {x: M.l, y: M.t, width: pw, height: ph, fill: 'transparent'});
  el.appendChild(svg);
  const tip = document.createElement('div'); tip.className = 'tip'; el.appendChild(tip);
  const sizes = [...new Set(xs)].sort((a, b) => a - b);
  hit.addEventListener('pointermove', ev => {
    const r = svg.getBoundingClientRect(), sx = (ev.clientX - r.left) / r.width * W;
    let best = sizes[0]; for (const v of sizes) if (Math.abs(X(v) - sx) < Math.abs(X(best) - sx)) best = v;
    cross.setAttribute('x1', X(best)); cross.setAttribute('x2', X(best)); cross.setAttribute('visibility', 'visible');
    let rows = `<b>${fmtBytes(best)}</b>`;
    const zone = (cfg.zones || []).find(z => best > z.from && best <= z.to);
    if (zone) rows += ` <span class="note">fits ${zone.label}</span>`;
    cfg.series.forEach((s, i) => {
      const p = s.points.find(q => q.x === best);
      if (!p) { dots[i].setAttribute('visibility', 'hidden'); return; }
      dots[i].setAttribute('cx', X(p.x)); dots[i].setAttribute('cy', Y(p.y)); dots[i].setAttribute('visibility', 'visible');
      rows += `<br><i style="--c:${s.color}"></i>${s.name}: <b>${fmtNum(p.y, cfg.unit)}</b>`;
      if (p.lo != null) rows += ` <span class="note">(min ${fmtNum(p.lo, cfg.unit)}, p90 ${fmtNum(p.hi, cfg.unit)})</span>`;
    });
    tip.innerHTML = rows; tip.style.display = 'block';
    const px = X(best) / W * r.width, flip = px > r.width * 0.6;
    tip.style.left = (flip ? px - tip.offsetWidth - 12 : px + 12) + 'px'; tip.style.top = '28px';
  });
  hit.addEventListener('pointerleave', () => { tip.style.display = 'none'; cross.setAttribute('visibility', 'hidden');
    dots.forEach(d => d.setAttribute('visibility', 'hidden')); });
}

const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
function render() {
  document.querySelectorAll('.chart').forEach(c => c.replaceChildren());
  const line = DATA.line;
  lineChart(document.getElementById('lat'), {
    title: 'Random load latency vs working set', ylog: true, yLabel: 'ns per load (log)', unit: 'ns',
    zones: DATA.zones, markers: DATA.markers,
    series: [{name: 'random load latency (p50)', color: css('--s1'),
      points: DATA.lat.map(p => ({x: p.x, y: p.p50, lo: p.lo, hi: p.hi}))}]});
  lineChart(document.getElementById('bw'), {
    title: 'Sequential bandwidth vs working set', ylog: false, yLabel: 'GB/s', unit: 'GB/s',
    zones: DATA.zones, markers: DATA.markers,
    series: [
      {name: 'read', color: css('--s1'), points: DATA.rd.map(p => ({x: p.x, y: line / p.p50}))},
      {name: 'write', color: css('--s2'), points: DATA.wr.map(p => ({x: p.x, y: line / p.p50}))}]});
}
render();
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', render);
"""


def build_html(doc, src_name):
    plat = doc["platform"]
    cfg = doc.get("config", {})
    results = doc["results"]
    line = plat.get("cache_line_size") or 64
    caches = data_caches(plat)

    lat = sweep_series(results, "mem.latency_random")
    rd = sweep_series(results, "mem.read_seq")
    wr = sweep_series(results, "mem.write_seq")
    max_x = max([p["x"] for p in lat] or [0])
    zones = level_zones(caches, max_x) if caches else []
    markers = [{"x": c["size_bytes"], "label": f"L{c['level']}"} for c in caches]
    summary = level_summary(lat, rd, wr, zones, line) if lat and zones else []
    tips = build_guidance(summary, caches, plat) if summary else []

    e = html.escape
    out = []
    out.append("<!doctype html><html lang='en'><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width, initial-scale=1'>"
               f"<title>Latency report</title><style>{CSS}</style></head><body><main>")
    board = f" · {e(plat['board'])}" if plat.get("board") else ""
    out.append(f"<h1>Latency report</h1><p class='sub'>{e(plat.get('cpu_model', ''))}{board} · "
               f"{e(plat.get('arch', ''))} · Linux {e(plat.get('kernel_release', ''))} "
               f"[{e(plat.get('preempt_model', ''))}]</p>")

    def sharing(c):
        cpus = c.get("shared_cpus", "")
        if not cpus:
            return ""
        return "per core" if cpus.isdigit() or "," not in cpus and "-" not in cpus else f"shared by CPUs {cpus}"

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
        ("Governor", plat.get("governor") or "n/a"),
        ("CPU idle", (plat.get("cpuidle_driver") or "n/a")
                     + ("; deep idle blocked during run" if cfg.get("block_deep_idle") else "; deep idle allowed")),
        ("Perf counters", f"{perf.get('backend', 'none')}: {', '.join(perf.get('events', [])) or 'none available'}"),
        ("Run", f"affinity {cfg.get('affinity')}, CPU {cfg.get('main_cpu')}/{cfg.get('worker_cpu')}, "
                f"RT priority {cfg.get('rt_priority') or 'default'}, mlock {'on' if cfg.get('lock_memory') else 'off'}"),
        ("Compiler", f"{plat.get('compiler', '')} ({plat.get('build_type', '')})"),
    ]
    out.append("<dl class='plat'>" + "".join(f"<dt>{e(k)}</dt><dd>{e(str(v))}</dd>" for k, v in rows) + "</dl>")

    if lat:
        out.append("<h2>Memory hierarchy: random load latency</h2>"
                   "<p class='sub'>Dependent loads in random order through a buffer of the given size, so hardware "
                   "prefetchers cannot help. Each step up is the working set spilling out of a cache level "
                   "(dashed lines are the cache sizes reported by the OS). Band: min to p90 across samples.</p>"
                   "<div class='legend'><span style='--c:var(--s1)'>random load latency (p50)</span></div>"
                   "<div class='chart' id='lat'></div>")
        if not cfg.get("hugepages"):
            out.append("<p class='note'>Base pages were used, so latency past the TLB reach also includes page walks. "
                       "Re-run with <code>--hugepages</code> to separate TLB misses from cache misses.</p>")
        out.append("<h2>Memory hierarchy: sequential bandwidth</h2>"
                   "<p class='sub'>Streaming read and write over the same buffer; prefetchers are effective here, "
                   "so the drops are smaller than for random access and show bandwidth limits of each level.</p>"
                   "<div class='legend'><span style='--c:var(--s1)'>read</span><span style='--c:var(--s2)'>write</span></div>"
                   "<div class='chart' id='bw'></div>")
        if summary:
            out.append("<h2>Per-level summary</h2><div class='table-wrap'><table><tr><th>data fits in</th>"
                       "<th>sizes used</th><th>random load</th><th>seq read</th><th>seq write</th></tr>")
            for r in summary:
                gb = lambda v: f"{v:.3g} GB/s" if v else "n/a"
                out.append(f"<tr><td>{e(r['level'])}</td><td>{e(r['range'])}</td><td>{fmt_ns(r['latency'])}</td>"
                           f"<td>{gb(r['read_gbs'])}</td><td>{gb(r['write_gbs'])}</td></tr>")
            out.append("</table></div>")
        if tips:
            out.append("<h2>Design hints for this machine</h2><ul class='tips'>"
                       + "".join(f"<li>{e(t)}</li>" for t in tips) + "</ul>")

    other = [r for r in results if not r["name"].startswith("mem.")]
    if other:
        pcols = ["p50", "p90", "p99", "p99_9", "p99_99"]
        out.append("<h2>Latency benchmarks</h2><p class='sub'>Grey values have fewer than ~10 samples beyond the "
                   "percentile and are not statistically reliable.</p><div class='table-wrap'><table><tr><th>benchmark</th>"
                   "<th>n</th><th>min</th>" + "".join(f"<th>{c.replace('_', '.')}</th>" for c in pcols) + "<th>max</th></tr>")
        for r in other:
            label = r["name"] + (" [" + ", ".join(f"{k}={v}" for k, v in r["params"].items()) + "]" if r["params"] else "")
            unrel = set(r.get("unreliable_percentiles", []))
            cells = "".join(f"<td class='{'unrel' if c in unrel else ''}'>{fmt_ns(r['percentiles'].get(c))}</td>" for c in pcols)
            out.append(f"<tr><td>{e(label)}</td><td>{r['count']}</td><td>{fmt_ns(r['min'])}</td>{cells}"
                       f"<td>{fmt_ns(r['max'])}</td></tr>")
        out.append("</table></div>")

    out.append(f"<p class='note'>Generated from {e(src_name)} by tools/olt-report.py.</p>")
    data = {"line": line, "zones": zones, "markers": markers, "lat": lat, "rd": rd, "wr": wr}
    out.append("<script type='application/json' id='olt-data'>"
               + json.dumps(data).replace("</", "<\\/") + "</script>")
    out.append(f"<script>{JS}</script></main></body></html>")
    return "\n".join(out)


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
    try:
        out.write_text(build_html(doc, src.name))
    except PermissionError:
        sys.exit(f"error: cannot write {out} (directory created by a sudo run?). "
                 f"Fix with: sudo chown -R $USER: {out.parent}  or pass -o ~/report.html")
    print(f"report written to {out}")


if __name__ == "__main__":
    main()
