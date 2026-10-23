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
import re
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


def false_sharing(results):
    """{op: {"alone": p50, "points": [(distance, p50, min, p90)], "cpus": (a, b)}} from multicore.false_sharing*."""
    out = {}
    for r in results:
        prm = r.get("params", {})
        if not r["name"].startswith("multicore.false_sharing") or "op" not in prm or not r["percentiles"].get("p50"):
            continue
        d = out.setdefault(prm["op"], {"alone": None, "points": [], "cpus": (prm.get("cpu_a"), prm.get("cpu_b"))})
        if r["name"] == "multicore.false_sharing_alone":
            d["alone"] = r["percentiles"]["p50"]
        else:
            d["points"].append((int(prm["distance"]), r["percentiles"]["p50"], r.get("min"), r["percentiles"].get("p90")))
    for d in out.values():
        d["points"].sort()
    return out


def safe_padding(fs):
    """Smallest distance from which the contended cost stays within 25% of one core alone, or None."""
    if not fs.get("alone") or not fs["points"]:
        return None
    ok = None
    for dist, p50, _, _ in reversed(fs["points"]):
        if p50 > 1.25 * fs["alone"]:
            break
        ok = dist
    return ok


# mem.read_seq / mem.write_seq param "isa" (olt/hal/stream.h) -> display name
ISA_NAMES = {"avx512": "AVX-512", "avx2": "AVX2", "sse2": "SSE2", "neon": "NEON", "generic": "16-byte"}

ILP_OPS = [("int_mul", "integer multiply"), ("fp_add", "double add"),
           ("fp_mul", "double multiply"), ("load_l1", "load hitting L1"), ("load_mem", "load missing to DRAM")]


def ilp_summary(results):
    """{op: {"points": [(chains, p50)], "latency", "best", "speedup", "chains"}} from cpu.ilp results."""
    pts = {}
    for r in results:
        prm = r.get("params", {})
        if r["name"] == "cpu.ilp" and "chains" in prm and r["percentiles"].get("p50"):
            pts.setdefault(prm["op"], []).append((int(prm["chains"]), r["percentiles"]["p50"]))
    out = {}
    for op, p in pts.items():
        p.sort()
        if p[0][0] != 1:
            continue
        lat = p[0][1]
        best = min(v for _, v in p)
        need = next(n for n, v in p if lat / v >= 0.9 * lat / best)
        out[op] = {"points": p, "latency": lat, "best": best, "speedup": lat / best, "chains": need}
    return out


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
  .s1{stroke:#2a78d6} .s2{stroke:#eb6834} .s3{stroke:#1baf7a} .s4{stroke:#6250d6} .s5{stroke:#e87ba4}
  .f1{fill:#2a78d6} .f2{fill:#eb6834} .f3{fill:#1baf7a} .f4{fill:#6250d6} .f5{fill:#e87ba4}
"""


class Chart:
    """Line chart on a log2-bytes, 'nines' (percentile) or linear 'count' x axis."""

    def __init__(self, cid, title, x_kind, y_log, y_label, unit, series, zones=(), markers=(), note="", x_title=None):
        self.cid, self.title, self.x_kind, self.y_log = cid, title, x_kind, y_log
        self.x_title = x_title
        self.y_label, self.unit, self.series = y_label, unit, [s for s in series if s["points"]]
        self.zones, self.markers, self.note = list(zones), list(markers), note

    # x mapping
    def xv(self, x):
        if self.x_kind == "bytes":
            return math.log2(x)
        if self.x_kind == "count":
            return x
        return -math.log10(max(1e-9, 1 - x / 100.0))  # nines: p50 -> 0.3, p99 -> 2, p99.9 -> 3

    def x_label(self, x):
        if self.x_kind == "bytes":
            return fmt_bytes(x)
        if self.x_kind == "count":
            return f"{x:g}"
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

        # legend layout: wrap onto extra rows instead of running off the edge
        legend, lx, row = [], M["l"], 0
        if len(self.series) > 1:
            for s in self.series:
                width = 40 + 7 * len(s["name"])
                if lx + width > W - M["r"] and lx > M["l"]:
                    lx, row = M["l"], row + 1
                legend.append((s, lx, row))
                lx += width
        extra = 20 * row
        HH = H + extra

        o = []
        o.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {HH + (24 if standalone else 0)}" '
                 f'role="img" aria-label="{html.escape(self.title)}">')
        if standalone:
            o.append(f"<style>{CHART_STYLE_LIGHT}</style>")
            o.append(f'<rect class="bg" width="{W}" height="{HH + 24}"/>')
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
        elif self.x_kind == "count":
            ticks, tick_label = xs, self.x_label
        else:
            ticks = [p for p in (0, 50, 90, 99, 99.9, 99.99, 99.999) if xs[0] <= p <= xs[-1]]
            tick_label = self.x_label
        for t in ticks:
            o.append(f'<text class="ax" x="{X(t):.1f}" y="{M["t"] + ph + 18}" text-anchor="middle">{tick_label(t)}</text>')
        for mk in self.markers:
            if xs[0] <= mk["x"] <= xs[-1]:
                o.append(f'<line class="marker" x1="{X(mk["x"]):.1f}" x2="{X(mk["x"]):.1f}" y1="{M["t"]}" '
                         f'y2="{M["t"] + ph}" stroke-dasharray="4 3"/>')
        xl = self.x_title or ("working set (buffer size)" if self.x_kind == "bytes" else "percentile (log scale of nines)")
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
        for s, lx, row in legend:
            ly = H - 12 + 20 * row
            dash = ' stroke-dasharray="6 4"' if s.get("dash") else ""
            o.append(f'<line class="s{s["slot"]}" stroke-width="2"{dash} x1="{lx}" x2="{lx + 22}" y1="{ly - 4}" y2="{ly - 4}"/>')
            o.append(f'<text class="ax" x="{lx + 28}" y="{ly}">{html.escape(s["name"])}</text>')

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


class BarChart:
    """Horizontal bars, one per variant (identity by label, single colour)."""

    def __init__(self, cid, title, x_label, rows, higher_better=False, best_label="fastest", ratio_word="slower"):
        self.cid, self.title, self.x_label, self.rows = cid, title, x_label, rows
        self.higher_better, self.best_label, self.ratio_word = higher_better, best_label, ratio_word

    def render(self, standalone=False):
        if not self.rows:
            return ""
        lw, rw, bh, gap, top = 170, 330, 22, 10, 10
        h = top + len(self.rows) * (bh + gap) + 34
        pw = W - lw - rw
        vmax = max(r["value"] for r in self.rows) or 1
        o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {h + (24 if standalone else 0)}" '
             f'role="img" aria-label="{html.escape(self.title)}">']
        if standalone:
            o.append(f"<style>{CHART_STYLE_LIGHT}</style><rect class=\"bg\" width=\"{W}\" height=\"{h + 24}\"/>")
            o.append(f'<text class="title" x="{lw}" y="18">{html.escape(self.title)}</text><g transform="translate(0,24)">')
        vals = [r["value"] for r in self.rows]
        best = max(vals) if self.higher_better else min(vals)
        for i, r in enumerate(self.rows):
            y = top + i * (bh + gap)
            w = max(2.0, r["value"] / vmax * pw)
            o.append(f'<text class="ax" x="{lw - 10}" y="{y + bh / 2 + 4}" text-anchor="end">{html.escape(r["label"])}</text>')
            o.append(f'<rect class="f1" x="{lw}" y="{y}" width="{w:.1f}" height="{bh}" rx="4"/>')
            slower = best / r["value"] if self.higher_better else r["value"] / best
            ratio = f"  ({self.best_label})" if r["value"] == best else f"  ({slower:.1f}x {self.ratio_word})"
            o.append(f'<text class="ax" x="{lw + w + 8:.1f}" y="{y + bh / 2 + 4}">{html.escape(r["text"] + ratio)}</text>')
            if not standalone and r.get("tip"):
                o.append(f'<rect class="hit" x="0" y="{y - gap / 2}" width="{W}" height="{bh + gap}" '
                         f'data-x="-10" data-tip="{html.escape(r["tip"], quote=True)}"/>')
        o.append(f'<text class="ax" x="{lw}" y="{h - 8}">{html.escape(self.x_label)}</text>')
        if not standalone:
            o.append(f'<line class="cross" x1="0" x2="0" y1="0" y2="0" visibility="hidden"/>')
        else:
            o.append("</g>")
        o.append("</svg>")
        return "\n".join(o)


class Heatmap:
    """Symmetric CPU x CPU matrix, sequential single-hue colour scale (darker = slower)."""

    LIGHT, DARK = (219, 232, 248), (13, 61, 120)

    def __init__(self, cid, title, cpus, values, unit_fmt=fmt_ns):
        self.cid, self.title, self.cpus, self.values, self.unit_fmt = cid, title, cpus, values, unit_fmt

    def colour(self, v, lo, hi):
        t = 0.0 if hi <= lo else (math.log(v) - math.log(lo)) / (math.log(hi) - math.log(lo))
        rgb = [round(a + (b - a) * t) for a, b in zip(self.LIGHT, self.DARK)]
        return "#%02x%02x%02x" % tuple(rgb), t > 0.55

    def render(self, standalone=False):
        if not self.values:
            return ""
        n = len(self.cpus)
        cell = max(22, min(56, (W - 120) // n))
        left, top = 60, 34
        size = cell * n
        h = top + size + 46
        vals = list(self.values.values())
        lo, hi = min(vals), max(vals)
        o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {h + (24 if standalone else 0)}" '
             f'role="img" aria-label="{html.escape(self.title)}">']
        if standalone:
            o.append(f"<style>{CHART_STYLE_LIGHT}</style><rect class=\"bg\" width=\"{W}\" height=\"{h + 24}\"/>")
            o.append(f'<text class="title" x="{left}" y="18">{html.escape(self.title)}</text><g transform="translate(0,24)">')
        show_text = cell >= 34
        for i, a in enumerate(self.cpus):
            o.append(f'<text class="ax" x="{left - 8}" y="{top + i * cell + cell / 2 + 4}" text-anchor="end">{a}</text>')
            o.append(f'<text class="ax" x="{left + i * cell + cell / 2}" y="{top - 8}" text-anchor="middle">{a}</text>')
            for j, b in enumerate(self.cpus):
                x, y = left + j * cell, top + i * cell
                if a == b:
                    o.append(f'<rect class="grid" fill="none" x="{x + 1}" y="{y + 1}" width="{cell - 2}" height="{cell - 2}"/>')
                    continue
                v = self.values.get((min(a, b), max(a, b)))
                if v is None:
                    continue
                fill, dark = self.colour(v, lo, hi)
                o.append(f'<rect x="{x + 1}" y="{y + 1}" width="{cell - 2}" height="{cell - 2}" rx="3" fill="{fill}"/>')
                if show_text:
                    o.append(f'<text x="{x + cell / 2}" y="{y + cell / 2 + 4}" text-anchor="middle" '
                             f'style="font:11px system-ui,sans-serif" fill="{"#ffffff" if dark else "#0b0b0b"}">{v:.0f}</text>')
                if not standalone:
                    tip = html.escape(f"<b>CPU {a} ↔ CPU {b}</b><br>one-way hand-off p50: <b>{self.unit_fmt(v)}</b>", quote=True)
                    o.append(f'<rect class="hit" x="{x}" y="{y}" width="{cell}" height="{cell}" data-x="-10" data-tip="{tip}"/>')
        # colour scale legend
        lx, ly = left + size + 30, top
        if lx + 40 < W:
            steps = 6
            for k in range(steps):
                v = math.exp(math.log(lo) + (math.log(hi) - math.log(lo)) * k / (steps - 1)) if hi > lo else lo
                fill, _ = self.colour(v, lo, hi)
                o.append(f'<rect x="{lx}" y="{ly + k * 22}" width="18" height="20" fill="{fill}"/>')
                o.append(f'<text class="ax" x="{lx + 26}" y="{ly + k * 22 + 14}">{self.unit_fmt(v)}</text>')
        o.append(f'<text class="ax" x="{left}" y="{top + size + 26}">one-way latency between CPUs, p50 '
                 f'(ns in cells; darker = slower)</text>')
        if not standalone:
            o.append('<line class="cross" x1="0" x2="0" y1="0" y2="0" visibility="hidden"/>')
        else:
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


def level_summary(lat, rd, wr, zones, line, nt=(), wrr=()):
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
            "nt_gbs": med(nt, lambda v: line / v),
            "store_ns": med(wrr),
        })
    return rows


def idle_text(cfg):
    """How idle states were limited during the run, for the Power line."""
    if "idle_limit" not in cfg:  # results.json from before per-CPU idle limits
        return "deep idle " + ("blocked" if cfg.get("block_deep_idle") else "allowed") + " during run"
    lim = cfg["idle_limit"]
    if not lim:
        return "deep idle allowed during run"
    if not lim.get("applied"):
        return "idle limit refused (needs root), deep idle allowed during run"
    where = "the benchmark CPUs" if lim.get("per_cpu") else "all CPUs"
    states = f" (allowed: {lim['states']})" if lim.get("states") else ""
    return f"idle states limited to {lim.get('latency_us')} µs exit latency on {where}{states}"


def deep_idle_limited(cfg):
    """True when the run kept the benchmark CPUs out of deep idle states."""
    if "idle_limit" not in cfg:
        return bool(cfg.get("block_deep_idle"))
    return bool(cfg["idle_limit"] and cfg["idle_limit"].get("applied"))


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
    dram = by.get("DRAM", {})
    if dram.get("store_ns") and dram.get("latency") and dram["latency"] > 2 * dram["store_ns"]:
        tips.append(f"A store that misses to DRAM costs {fmt_ns(dram['store_ns'])} of throughput vs "
                    f"{fmt_ns(dram['latency'])} for a dependent load: the store buffer lets the core move on and "
                    f"overlaps the misses. Scattered writes (histograms, scatter, logging) hurt far less than "
                    f"scattered reads; what stalls is reading the data back or filling the store buffer.")
    if dram.get("nt_gbs") and dram.get("write_gbs") and dram["nt_gbs"] > 1.3 * dram["write_gbs"]:
        tips.append(f"Writing a buffer larger than the last cache: streaming (non-temporal) stores reach "
                    f"{dram['nt_gbs']:.3g} GB/s vs {dram['write_gbs']:.3g} GB/s for normal stores, which first read "
                    f"each line in. Use them for large output written once and not read again soon "
                    f"(_mm_stream_si128 / stnp, or a memcpy/memset that switches to them).")
    named = {r["name"]: r for r in results if not r.get("params")}
    wake = named.get("sync.mutex_wakeup")
    if wake:
        p50, p99 = wake["percentiles"].get("p50"), wake["percentiles"].get("p99")
        if p50 and wake["min"] and p50 > 3 * wake["min"] and not deep_idle_limited(cfg):
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
    def by_param(name):
        return {next(iter(r["params"].values())): r for r in results if r["name"] == name and len(r["params"]) == 1}

    c2c = [r for r in results if r["name"] == "multicore.c2c_latency" and r["percentiles"].get("p50")]
    if len(c2c) >= 2:
        v = sorted(r["percentiles"]["p50"] for r in c2c)
        if v[-1] > 2 * v[0]:
            tips.append(f"Where threads run matters: handing a cache line to another CPU costs {fmt_ns(v[0])} on the "
                        f"fastest pair but {fmt_ns(v[-1])} on the slowest ({v[-1] / v[0]:.0f}x). Keep communicating "
                        f"threads on nearby CPUs (see the core-to-core matrix).")

    ilp = ilp_summary(results)
    if "fp_add" in ilp and ilp["fp_add"]["speedup"] >= 1.5:
        f = ilp["fp_add"]
        tips.append(f"Loop unrolling: a floating-point sum with one accumulator runs at {fmt_ns(f['latency'])} per add "
                    f"(the add latency); with {f['chains']} independent accumulators it reaches {fmt_ns(f['best'])} "
                    f"({f['speedup']:.1f}x). Split reductions into {f['chains']} partial sums (the compiler does it "
                    f"for integers, for floating point only with -ffast-math / -fassociative-math).")
    if "load_mem" in ilp and ilp["load_mem"]["speedup"] >= 2:
        m = ilp["load_mem"]
        tips.append(f"Memory-level parallelism: one chain of dependent DRAM misses costs {fmt_ns(m['latency'])} per "
                    f"load, but {m['chains']} independent chains overlap to {fmt_ns(m['best'])} ({m['speedup']:.0f}x). "
                    f"Batch independent lookups (hash probes, tree searches, pointer chasing) {m['chains']} at a time, "
                    f"or prefetch ahead, instead of finishing one before starting the next.")

    fs = false_sharing(results).get("atomic")
    pad = safe_padding(fs) if fs else None
    if pad and fs["points"][0][1] > 1.5 * fs["alone"]:
        worst = max(p[1] for p in fs["points"])
        line = platform.get("cache_line_size", 64)
        extra = (f" That is more than the {line}-byte cache line because the adjacent-line prefetcher pulls lines "
                 f"in pairs: alignas({line}) is not enough here, whatever "
                 f"std::hardware_destructive_interference_size says.") if pad > line else ""
        tips.append(f"False sharing: two cores updating counters in the same cache line pay {fmt_ns(worst)} per atomic "
                    f"increment instead of {fmt_ns(fs['alone'])} ({worst / fs['alone']:.0f}x). Keep data written by "
                    f"different threads at least {pad} bytes apart (alignas({pad}) or padding).{extra}")

    lat = by_param("pattern.spsc_latency")
    if "mutex_condvar" in lat and ("lockfree_cached" in lat or "lockfree" in lat):
        lf = lat.get("lockfree_cached") or lat["lockfree"]
        a, b = lat["mutex_condvar"]["percentiles"], lf["percentiles"]
        head = (f"Thread-to-thread hand-off: a blocking mutex+condvar queue takes {fmt_ns(a['p50'])} (p99 "
                f"{fmt_ns(a['p99'])}), a polling lock-free SPSC ring {fmt_ns(b['p50'])} (p99 {fmt_ns(b['p99'])})")
        if b["p50"] < a["p50"]:
            tips.append(f"{head}: {a['p50'] / b['p50']:.0f}x faster, at the price of a core spinning while it waits. "
                        f"Give each polling thread its own core.")
        else:
            tips.append(f"{head}: here polling is {b['p50'] / a['p50']:.0f}x slower, because producer and consumer "
                        f"{'share a CPU' if cfg.get('affinity') == 'same' else 'compete for CPU time'} and the spinner "
                        f"burns the time slice the other side needs. Never spin-wait on a shared core; block instead.")
    thr = by_param("pattern.spsc_throughput")
    if len(thr) >= 2:
        best = min(thr.items(), key=lambda kv: kv[1]["percentiles"]["p50"])
        worst = max(thr.items(), key=lambda kv: kv[1]["percentiles"]["p50"])
        tips.append(f"Streaming between two threads: {best[0]} moves {1e3 / best[1]['percentiles']['p50']:.3g} M items/s, "
                    f"{worst[1]['percentiles']['p50'] / best[1]['percentiles']['p50']:.1f}x more than {worst[0]}.")
    dm, d1 = by_param("pattern.dispatch_mixed"), by_param("pattern.dispatch_one_type")
    if "virtual" in dm and "template" in dm:
        v, t = dm["virtual"]["percentiles"]["p50"], dm["template"]["percentiles"]["p50"]
        one = (f" With a single type at the call site the virtual call costs only {fmt_ns(d1['virtual']['percentiles']['p50'])}: "
               f"the target is predicted." if "virtual" in d1 else "")
        alt = min((k for k in ("switch", "variant") if k in dm), key=lambda k: dm[k]["percentiles"]["p50"], default=None)
        mid = (f" A {'std::variant' if alt == 'variant' else 'switch on a type tag'} sits between "
               f"({fmt_ns(dm[alt]['percentiles']['p50'])})." if alt else "")
        tips.append(f"Dispatch: calling through a virtual function on objects of mixed types costs {fmt_ns(v)} per call, "
                    f"{v / t:.0f}x the {fmt_ns(t)} of the same work with the type known at compile time (templates, "
                    f"objects grouped by type).{one}{mid} On a hot loop over mixed objects, keep one container per type "
                    f"or sort by type.")
    lst = by_param("pattern.alloc_list")
    if "new_delete" in lst and "pmr_monotonic" in lst:
        a, b = lst["new_delete"]["percentiles"]["p50"], lst["pmr_monotonic"]["percentiles"]["p50"]
        tips.append(f"Node-based containers: a std::pmr::monotonic_buffer_resource arena makes building a list "
                    f"{a / b:.1f}x cheaper than new/delete ({fmt_ns(b)} vs {fmt_ns(a)} per node). Use arenas for "
                    f"short-lived, per-request or per-frame data that is freed all at once.")
    vec = by_param("pattern.alloc_vector")
    if "new_delete" in vec and "reserve" in vec:
        a, b = vec["new_delete"]["percentiles"]["p50"], vec["reserve"]["percentiles"]["p50"]
        if a > b * 1.15:
            tips.append(f"std::vector: reserve() up front saves {100 * (1 - b / a):.0f}% of the build cost "
                        f"by avoiding regrowth copies.")
    return tips


# ---------------------------------------------------------------- page

CSS = """
:root {
  color-scheme: light;
  --surface: #fcfcfb; --text: #0b0b0b; --text-2: #52514e; --muted: #8a8984;
  --grid: #e4e3df; --zone-a: #f3f2ef; --zone-b: #fcfcfb;
  --s1: #2a78d6; --s2: #eb6834; --s3: #1baf7a; --s4: #6250d6; --s5: #e87ba4;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    color-scheme: dark;
    --surface: #1a1a19; --text: #f0efec; --text-2: #c3c2b7; --muted: #8f8e88;
    --grid: #33332f; --zone-a: #232322; --zone-b: #1a1a19;
    --s1: #3987e5; --s2: #d95926; --s3: #199e70; --s4: #9085e9; --s5: #d55181;
  }
}
:root[data-theme="dark"] {
  color-scheme: dark;
  --surface: #1a1a19; --text: #f0efec; --text-2: #c3c2b7; --muted: #8f8e88;
  --grid: #33332f; --zone-a: #232322; --zone-b: #1a1a19;
  --s1: #3987e5; --s2: #d95926; --s3: #199e70; --s4: #9085e9; --s5: #d55181;
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
.s5 { stroke: var(--s5); } .f5 { fill: var(--s5); background: var(--s5); }
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
.warn { border: 1px solid var(--grid); border-left: 4px solid #eda100; border-radius: 6px; padding: 8px 12px; margin: 12px 0; }
.warn ul { margin: 4px 0 0; padding-left: 20px; }
.caution { border: 1px solid var(--grid); border-left: 4px solid var(--s1); border-radius: 6px; padding: 8px 12px; margin: 12px 0; color: var(--text-2); }
.invalid { border: 1px solid var(--grid); border-left: 4px solid #d03b3b; border-radius: 6px; padding: 8px 12px; margin: 12px 0; }
.invalid p { margin: 4px 0 0; }
.guide { margin: 4px 0 18px; font-size: 14px; }
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
      if (x >= 0) { cross.setAttribute('x1', x); cross.setAttribute('x2', x); cross.setAttribute('visibility', 'visible'); }
      tip.innerHTML = h.dataset.tip; tip.style.display = 'block';
      const r = svg.getBoundingClientRect(), px = x / W * r.width;
      if (x < 0) { tip.style.left = r.width * 0.55 + 'px'; tip.style.top = (h.getBBox().y / svg.viewBox.baseVal.height * r.height) + 'px'; return; }
      tip.style.top = '28px';
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


UNOPTIMISED_WARNING = (
    "The {langs} code of this binary was compiled without optimisation. Its timed loops then measure the "
    "compiler's stack traffic instead of the hardware: results come out up to several times too slow, and no "
    "design hints are given. Rebuild in a fresh directory (rm -rf build && cmake -S . -B build "
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j) and run again.")


def unoptimised_languages(plat):
    """Languages built without optimisation ("C++", "C", "C++ and C"); "" if none or unknown."""
    opt = plat.get("optimized") or {}
    return " and ".join(name for key, name in (("cxx", "C++"), ("c", "C")) if opt.get(key) is False)


HOT_PATH_WARNING = (
    "Hot path only: the techniques these results justify (spinning instead of blocking, lock-free queues, "
    "pinning, cache-sized buffers, arenas, branchless code, intrinsics) trade readability, maintainability and "
    "portability for nanoseconds. Apply them only to the measured hot path of ultra-low-latency or "
    "high-performance code, after profiling shows it matters, and keep the rest simple and portable. The numbers "
    "are for this machine; re-measure on each target.")


REPO_URL = "https://github.com/ionutneicu/os-latency-tests"
GUIDE_URL = REPO_URL + "/blob/main/docs/benchmarks/"
GLOSSARY_URL = "https://github.com/ionutneicu/os-latency-tests/blob/main/docs/GLOSSARY.md"
GUIDES = {"thread": ("threads.md", "threads: spawn or pool"), "sync": ("sync.md", "blocking wake-up")}


def guide_link(page, what, rs=(), plat=None):
    """Link to the benchmark's page (goal, what is measured, how to apply it in C++), and to the code
    that produced the results rs."""
    return (f"<p class='guide'>Benchmark guide: <a href='{GUIDE_URL}{page}'>{html.escape(what)}</a>, what is "
            f"measured and how to apply the result in C++.</p>" + source_links(rs, plat or {}))


def source_url(src, plat):
    """GitHub link to a result's source line, at the commit the binary was built from."""
    commit = plat.get("git_commit") or ""
    return f"{REPO_URL}/blob/{commit or 'main'}/{src['file']}" + (f"#L{src['line']}" if commit else "")


def source_links(rs, plat):
    """'Code measured: ...' line linking each variant (or the whole benchmark) to its source."""
    links = {}
    for r in rs:
        src = r.get("source")
        if not src or not src.get("file"):
            continue
        label = ", ".join(r["params"].values()) if len(r["params"]) == 1 else r["name"].split(".", 1)[-1]
        links.setdefault(label, src)
    if not links:
        return ""
    e = html.escape
    srcs = {(v["file"], v["line"]) for v in links.values()}
    if len(srcs) == 1:
        src = next(iter(links.values()))
        items = f"<a href='{e(source_url(src, plat))}'><code>{e(src['file'])}</code></a>"
    else:
        items = " · ".join(f"<a href='{e(source_url(src, plat))}'>{e(label)}</a>" for label, src in links.items())
    commit = plat.get("git_commit") or ""
    if not commit:
        where = " (no git commit was recorded for this build: the links go to the current main branch)"
    elif plat.get("git_dirty"):
        where = f" at commit {e(commit[:7])}, built with local changes, so lines may differ"
    else:
        where = f" at commit {e(commit[:7])}"
    return f"<p class='guide'>Code measured{where}: {items}</p>"


def md_cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


def html_to_markdown(body):
    """Converts the small HTML subset used in report sections to GitHub Markdown."""
    body = re.sub(r"<!--md:(.*?)-->.*?<!--/md-->", lambda m: "\n\n" + m.group(1) + "\n\n",
                  body, flags=re.S)

    def table(m):
        rows = []
        for tr in re.findall(r"<tr>(.*?)</tr>", m.group(0), flags=re.S):
            cells = [md_cell(inline(c)) for c in re.findall(r"<t[hd][^>]*>(.*?)</t[hd]>", tr, flags=re.S)]
            rows.append("| " + " | ".join(cells) + " |")
        if rows:
            rows.insert(1, "|" + "---|" * rows[0].count(" | ") + "---|")
        return "\n\n" + "\n".join(rows) + "\n\n"

    def inline(t):
        t = re.sub(r"</?code>", "`", t)
        t = re.sub(r"</?b>", "**", t)
        t = re.sub(r"<a href='([^']*)'>(.*?)</a>", r"[\2](\1)", t)
        t = re.sub(r"<br\s*/?>", " ", t)
        return html.unescape(re.sub(r"<[^>]+>", "", t)).strip()

    body = re.sub(r"<table>.*?</table>", table, body, flags=re.S)
    body = re.sub(r"<h3>(.*?)</h3>", lambda m: f"\n\n### {inline(m.group(1))}\n\n", body, flags=re.S)
    body = re.sub(r"<li>(.*?)</li>", lambda m: f"\n- {inline(m.group(1))}", body, flags=re.S)
    body = re.sub(r"<p class='(note|missing)'>(.*?)</p>",
                  lambda m: f"\n\n> {inline(m.group(2))}\n\n", body, flags=re.S)
    body = re.sub(r"<p[^>]*>(.*?)</p>", lambda m: f"\n\n{inline(m.group(1))}\n\n", body, flags=re.S)
    body = re.sub(r"</?(div|ul)[^>]*>", "\n", body)
    return re.sub(r"\n{3,}", "\n\n", body).strip() + "\n"


def build_report(doc, src_path, charts_dir):
    plat = doc["platform"]
    cfg = doc.get("config", {})
    results = doc["results"]
    base = src_path.parent
    line = plat.get("cache_line_size") or 64
    caches = data_caches(plat)
    e = html.escape

    def named(prefix):
        return [r for r in results if r["name"].startswith(prefix)]

    sections = []      # (anchor, title, html)
    charts = []        # Chart objects, also written as standalone SVG

    def chart_block(ch):
        charts.append(ch)
        # the <!--md:...--> marker tells html_to_markdown() to use the SVG file instead
        return (f"<!--md:![{e(ch.title)}]({e(charts_dir.name)}/{e(ch.cid)}.svg)-->"
                f"<div class='chart'>{ch.render()}</div>"
                f"<div class='dl'><a href='{e(charts_dir.name)}/{e(ch.cid)}.svg'>{e(ch.cid)}.svg</a></div><!--/md-->")

    # ---- memory hierarchy
    lat = sweep_series(results, "mem.latency_random")
    rd = sweep_series(results, "mem.read_seq")
    wr = sweep_series(results, "mem.write_seq")
    nt = sweep_series(results, "mem.write_nt")
    wrr = sweep_series(results, "mem.write_random")
    summary, mem_html = [], []
    if lat:
        max_x = max(p["x"] for p in lat)
        zones = level_zones(caches, max_x) if caches else []
        markers = [{"x": c["size_bytes"]} for c in caches]
        mem_html.append("<p class='sub'>Dependent loads in random order through a buffer of the given size, so "
                        "hardware prefetchers cannot help. Each step up is the working set spilling out of a cache "
                        "level; dashed lines are the cache sizes reported by the OS. Band: min to p90 across samples."
                        + (" Second line: independent stores to random lines of the same buffer. A store miss "
                           "does not stall the core (the store buffer holds it while the line is fetched), so misses "
                           "overlap and a store costs far less than a dependent load at every level." if wrr else "")
                        + "</p>")
        lat_series = [{"name": "random load latency (p50)", "slot": 1, "band": True,
                       "points": [(p["x"], p["p50"], p["lo"], p["hi"]) for p in lat]}]
        if wrr:
            lat_series.append({"name": "random store, independent (p50)", "slot": 2,
                               "points": [(p["x"], p["p50"]) for p in wrr]})
        mem_html.append(chart_block(Chart(
            "mem-latency", "Random access cost vs working set", "bytes", True, "ns per access (log)", "ns",
            lat_series, zones, markers)))
        if not cfg.get("hugepages"):
            mem_html.append("<p class='note'>Base pages were used, so latency past the TLB reach also includes page "
                            "walks. Re-run with <code>--hugepages</code> to separate TLB misses from cache misses.</p>")
        isa = next((r["params"].get("isa") for r in results
                    if r["name"] == "mem.read_seq" and r.get("params", {}).get("isa")), None)
        mem_html.append("<p class='sub'>Sequential read and write over the same buffer"
                        + (f" with {e(ISA_NAMES.get(isa, isa))} vector loads and stores" if isa else "")
                        + ": prefetchers are effective, so the drops are smaller than for random "
                        "access and show each level's bandwidth (log scale: equal steps are equal ratios). A normal store must first read its line in, so "
                        "each written line crosses the bus twice: read for ownership, then write-back."
                        + (" Dashed curve: streaming (non-temporal) stores, which skip that read and the caches: slow "
                           "while the data would fit in cache, faster beyond it." if nt else "")
                        + (" Narrower vectors (scalar or SSE code) cannot reach the L1 and L2 figures: those levels "
                           "deliver as many bytes per cycle as the core can request." if isa in ("avx512", "avx2") else "")
                        + "</p>")
        bw_series = [{"name": "read", "slot": 1, "points": [(p["x"], line / p["p50"]) for p in rd]},
                     {"name": "write", "slot": 2, "points": [(p["x"], line / p["p50"]) for p in wr]}]
        if nt:
            bw_series.append({"name": "streaming write", "slot": 3, "dash": True,
                              "points": [(p["x"], line / p["p50"]) for p in nt]})
        mem_html.append(chart_block(Chart(
            "mem-bandwidth", "Sequential bandwidth vs working set", "bytes", True, "GB/s (log)", "GB/s",
            bw_series, zones, markers)))
        if zones:
            summary = level_summary(lat, rd, wr, zones, line, nt, wrr)
            gb = lambda v: f"{v:.3g} GB/s" if v else "n/a"
            ns = lambda v: fmt_ns(v) if v else "n/a"
            mem_html.append("<h3>Per-level summary</h3><div class='table-wrap'><table><tr><th>data fits in</th>"
                            "<th>sizes used</th><th>random load</th><th>random store</th><th>seq read</th>"
                            "<th>seq write</th><th>streaming write</th></tr>"
                            + "".join(f"<tr><td>{e(r['level'])}</td><td>{e(r['range'])}</td><td>{ns(r['latency'])}</td>"
                                      f"<td>{ns(r['store_ns'])}</td><td>{gb(r['read_gbs'])}</td>"
                                      f"<td>{gb(r['write_gbs'])}</td><td>{gb(r['nt_gbs'])}</td></tr>" for r in summary)
                            + "</table></div>")
    else:
        mem_html.append("<p class='missing'>The memory sweep was not part of this run. Run "
                        "<code>os-latency-tests</code> without <code>-t</code> (or with <code>-t 'mem.*'</code>) "
                        "to get the cache graphs.</p>")
    if lat:
        mem_html.append(guide_link("memory.md", "memory hierarchy", named("mem."), plat))
    sections.append(("memory", "Memory hierarchy", "\n".join(mem_html)))

    # ---- instruction-level parallelism
    ilp = ilp_summary(results)
    if ilp:
        names = dict(ILP_OPS)
        chains = sorted({n for d in ilp.values() for n, _ in d["points"]})

        def speedup_chart(cid, title, ops):
            series = [{"name": names[op], "slot": i + 1,
                       "points": [(n, ilp[op]["latency"] / v) for n, v in ilp[op]["points"]]}
                      for i, op in enumerate(o for o in ops if o in ilp)]
            if not series:
                return ""
            top = max(p[1] for s in series for p in s["points"])
            series.append({"name": "perfect overlap", "slot": 5, "dash": True,
                           "points": [(n, n) for n in chains if n <= top * 1.2]})
            return chart_block(Chart(cid, title, "count", False, "speed-up vs one chain", "x", series,
                                     x_title="independent dependency chains in the loop"))

        body = ("<p class='sub'>A loop runs N independent dependency chains of the same operation (N accumulators, "
                "N pointer chases). With one chain every step waits for the previous result, so it runs at the "
                "operation's latency; with more chains the out-of-order core overlaps them until an execution "
                "port, the load units or the miss buffers are saturated. Where a curve flattens is how far to "
                "unroll such a loop on this core. Dashed: perfect overlap.</p>")
        body += speedup_chart("cpu-ilp-alu", "Instruction-level parallelism: arithmetic",
                              ["int_mul", "fp_add", "fp_mul"])
        body += speedup_chart("cpu-ilp-mem", "Instruction-level parallelism: loads",
                              ["load_l1", "load_mem"])
        body += ("<div class='table-wrap'><table><tr><th>operation</th><th>one chain (latency)</th>"
                 "<th>best per op</th><th>speed-up</th><th>chains to get 90% of it</th></tr>"
                 + "".join(f"<tr><td>{e(names[op])}</td><td>{fmt_ns(d['latency'])}</td><td>{fmt_ns(d['best'])}</td>"
                           f"<td>{d['speedup']:.1f}x</td><td>{d['chains']}</td></tr>"
                           for op, d in ((o, ilp[o]) for o, _ in ILP_OPS if o in ilp))
                 + "</table></div>")
        body += ("<p class='note'>Curves can drop again at high chain counts: once accumulators outnumber the registers "
                 "(16 general-purpose and 16 vector registers on x86-64, 32 of each on AArch64) the compiler spills "
                 "them to the stack. More chains is not always better.</p>")
        body += guide_link("ilp.md", "instruction-level parallelism", named("cpu.ilp"), plat)
        sections.append(("cpu", "Instruction-level parallelism", body))

    # ---- latency distributions, one chart per benchmark group
    groups = {}
    for r in results:
        if r["params"] or r["name"].startswith(("mem.", "alloc.", "pattern.", "multicore.")):
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
        if g in GUIDES:
            body += guide_link(*GUIDES[g], rs, plat)
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
        body += guide_link("alloc.md", "heap allocator", alloc, plat)
        sections.append(("alloc", "Heap allocator", body))

    # ---- design patterns: variants of one task side by side
    pat_html = []

    def variants(name):
        return [r for r in results if r["name"] == name and len(r["params"]) == 1]

    def bar_rows(rs, conv=None, unit_text=None):
        rows = []
        for r in rs:
            v = r["percentiles"]["p50"]
            label = next(iter(r["params"].values()))
            text = (unit_text(v) if unit_text else f"p50 {fmt_ns(v)}, p99 {fmt_ns(r['percentiles'].get('p99'))}")
            rows.append({"label": label, "value": conv(v) if conv else v, "text": text,
                         "tip": f"<b>{html.escape(label)}</b><br>p50 {fmt_ns(v)} · p90 {fmt_ns(r['percentiles'].get('p90'))}"
                                f" · p99 {fmt_ns(r['percentiles'].get('p99'))} · max {fmt_ns(r['max'])}"})
        return rows

    av, al = variants("pattern.alloc_vector"), variants("pattern.alloc_list")
    if av or al:
        pat_html.append("<h3>Container memory: heap vs reserve vs pmr arenas</h3>"
                        "<p class='sub'>Build and destroy a 256-element container; cost per element (p50, shorter is "
                        "better, compared with the fastest in brackets). Node containers allocate once per element, so the "
                        "allocator matters far more there.</p>")
        if av:
            pat_html.append(chart_block(BarChart("pattern-alloc-vector", "std::vector<int>, 256 push_back: ns per element",
                                                 "ns per element (p50)", bar_rows(av))))
        if al:
            pat_html.append(chart_block(BarChart("pattern-alloc-list", "std::list<int>, 256 push_back: ns per element",
                                                 "ns per element (p50)", bar_rows(al))))
        pat_html.append(guide_link("pattern-alloc.md", "container memory", av + al, plat))
    sl, st = variants("pattern.spsc_latency"), variants("pattern.spsc_throughput")
    if sl or st:
        pat_html.append("<h3>Single producer / single consumer: mutex vs spinlock vs lock-free</h3>"
                        "<p class='sub'>The same bounded ring hand-off synchronised five ways. Latency is half a "
                        "ping-pong round trip; throughput streams 256 Ki items through one queue.</p>")
        if sl:
            series = []
            for i, r in enumerate(sl[:5]):
                pts, mx = distribution(r, base)
                series.append({"name": f"{r['params']['queue']} (max {fmt_ns(mx)})", "slot": i + 1, "points": pts})
            pat_html.append(chart_block(Chart("pattern-spsc-latency", "SPSC hand-off latency by percentile", "nines",
                                              True, "one-way latency (log)", "ns", series)))
        if st:
            pat_html.append(chart_block(BarChart(
                "pattern-spsc-throughput", "SPSC streaming throughput", "million items per second (p50 run)",
                bar_rows(st, conv=lambda v: 1e3 / v, unit_text=lambda v: f"{1e3 / v:.3g} M items/s ({fmt_ns(v)}/item)"),
                higher_better=True)))
        cpu_rows = []
        for r in (sl or st):
            m = r.get("metrics", {}).get("cpu_cores")
            if m is not None:
                cpu_rows.append({"label": r["params"]["queue"], "value": m, "text": f"{m:.2f} cores busy",
                                 "tip": f"<b>{html.escape(r['params']['queue'])}</b><br>CPU time of producer + consumer / "
                                        f"wall time during the latency test: {m:.2f}"})
        if cpu_rows:
            pat_html.append("<p class='sub'>What it costs: CPU time burnt by producer plus consumer during the "
                            "latency test, divided by wall time. 2.0 means both threads kept a core fully busy "
                            "(polling); low values mean they slept while waiting.</p>")
            pat_html.append(chart_block(BarChart("pattern-spsc-cpu", "SPSC CPU usage while handing off",
                                                 "CPU cores busy (producer + consumer)", cpu_rows,
                                                 best_label="least CPU", ratio_word="more CPU")))
        pat_html.append(guide_link("pattern-spsc.md", "SPSC queues", sl + st, plat))
    d1, dm = variants("pattern.dispatch_one_type"), variants("pattern.dispatch_mixed")
    if d1 or dm:
        pat_html.append("<h3>Dispatch: virtual vs std::function vs std::variant vs templates</h3>"
                        "<p class='sub'>1024 small objects each compute a one-cycle operation; ns per call (p50, shorter "
                        "is better). Run-time dispatch picks the code per object (vtable, function pointer, switch, "
                        "variant); <code>template</code> fixes the type at compile time, so the call inlines away. "
                        "With one type the branch predictor always guesses the target; with four types in random "
                        "order it misses most of the time.</p>")
        if d1:
            pat_html.append(chart_block(BarChart("pattern-dispatch-one", "One type at the call site: ns per call",
                                                 "ns per call (p50)", bar_rows(d1))))
        if dm:
            pat_html.append(chart_block(BarChart("pattern-dispatch-mixed", "Four types in random order: ns per call",
                                                 "ns per call (p50)", bar_rows(dm))))
        pat_html.append(guide_link("pattern-dispatch.md", "dispatch", d1 + dm, plat))
    c2c = {}
    for r in results:
        if r["name"] == "multicore.c2c_latency" and "cpu_a" in r["params"] and r["percentiles"].get("p50"):
            a, b = int(r["params"]["cpu_a"]), int(r["params"]["cpu_b"])
            c2c[(min(a, b), max(a, b))] = r["percentiles"]["p50"]
    fsd = false_sharing(results)
    fs_html = ""
    if fsd:
        names = {"atomic": "atomic fetch_add", "plain": "plain ++ (non-atomic)"}
        series = []
        for slot, op in enumerate(("atomic", "plain"), start=1):
            d = fsd.get(op)
            if not d or not d["points"]:
                continue
            series.append({"name": f"{names[op]}, two cores", "slot": slot, "band": True, "points": d["points"]})
            if d["alone"]:
                series.append({"name": f"{names[op]}, one core alone", "slot": slot, "dash": True,
                               "points": [(p[0], d["alone"]) for p in d["points"]]})
        a, b = next(iter(fsd.values()))["cpus"]
        line = plat.get("cache_line_size") or 64
        fs_html = ("<h3>False sharing: how far apart must per-thread data be?</h3>"
                   f"<p class='sub'>Threads on CPU {e(str(a))} and CPU {e(str(b))} (different physical cores) each "
                   "increment their own counter; the counters sit the given distance apart. While both share a "
                   "cache line, every write pulls the line away from the other core. Atomic read-modify-writes pay "
                   "each transfer; plain stores hide most of it in the store buffer, so the penalty there is "
                   "smaller but not zero. Dashed: the same loop on one core alone. Band: min to p90.</p>")
        fs_html += chart_block(Chart("multicore-false-sharing", "False sharing: cost per increment vs distance "
                                     "between counters", "bytes", True, "ns per increment (log)", "ns", series,
                                     markers=[{"x": line}], x_title="distance between the two counters"))
        pad = safe_padding(fsd.get("atomic", {})) if fsd.get("atomic") else None
        if pad:
            fs_html += (f"<p class='note'>On this CPU, counters written by different threads need at least "
                        f"{pad} bytes between them (vertical line: the {line}-byte cache line).</p>")
        fs_html += guide_link("false-sharing.md", "false sharing", named("multicore.false_sharing"), plat)
    if c2c:
        cpus = sorted({c for k in c2c for c in k})
        vals = sorted(c2c.values())
        fast = min(c2c, key=c2c.get)
        slow = max(c2c, key=c2c.get)
        body = ("<p class='sub'>Two threads pinned to a pair of CPUs ping-pong one value through a "
                "<code>std::atomic</code> on its own cache line; each cell is the one-way hand-off latency (half a "
                "round trip, p50). This is the floor for any inter-thread communication between those CPUs: "
                "SMT siblings share L1/L2 and are fastest, other cores on the same die or cluster come next, "
                "other clusters, CCXs or sockets are slowest.</p>")
        body += chart_block(Heatmap("multicore-c2c", "Core-to-core latency (one-way, p50)", cpus, c2c))
        body += (f"<p class='note'>Fastest pair: CPU {fast[0]} ↔ {fast[1]} ({fmt_ns(c2c[fast])}); slowest: CPU "
                 f"{slow[0]} ↔ {slow[1]} ({fmt_ns(c2c[slow])}); median {fmt_ns(statistics.median(vals))}. Pin "
                 f"threads that talk a lot (producer/consumer pairs) to a fast pair with <code>--cpus A,B</code> "
                 f"or in your application.</p>")
        body += guide_link("c2c.md", "core-to-core latency", named("multicore.c2c"), plat)
        sections.append(("multicore", "Multicore communication", body + fs_html))
    elif fs_html:
        sections.append(("multicore", "Multicore communication", fs_html))
    else:
        sections.append(("multicore", "Multicore communication",
                         "<p class='missing'>The core-to-core benchmark was not part of this run (run without "
                         "<code>-t</code> or with <code>-t 'multicore.*'</code>).</p>"))

    if pat_html:
        sections.append(("patterns", "Design patterns", "\n".join(pat_html)))
    else:
        sections.append(("patterns", "Design patterns",
                         "<p class='missing'>No design-pattern benchmarks in this run (run without <code>-t</code> "
                         "or with <code>-t 'pattern.*'</code>).</p>"))

    # ---- hints (none from an unoptimised build: they would describe the compiler)
    unopt = unoptimised_languages(plat)
    tips = [] if unopt else build_guidance(summary, caches, plat, results, cfg)
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
        ("Power", f"governor {plat.get('governor') or 'n/a'}, cpuidle {plat.get('cpuidle_driver') or 'n/a'}, "
                  f"{idle_text(cfg)}"),
        ("Perf counters", f"{perf.get('backend', 'none')}: {', '.join(perf.get('events', [])) or 'none available'}"),
        ("Run", f"{cfg.get('samples')} samples, affinity {cfg.get('affinity')} (CPU {cfg.get('main_cpu')}/{cfg.get('worker_cpu')}), "
                f"RT priority {cfg.get('rt_priority') or 'default'}, mlock {'on' if cfg.get('lock_memory') else 'off'}"),
        ("Compiler", f"{plat.get('compiler', '')} ({plat.get('build_type', '')})"
                     + (f", {unopt} NOT optimised" if unopt else
                        ", optimised" if all((plat.get("optimized") or {}).get(k) for k in ("c", "cxx")) else "")),
    ]
    if plat.get("git_commit"):
        rows.append(("Source", f"commit {plat['git_commit'][:12]}" + (" + local changes" if plat.get("git_dirty") else "")))
    out.append("<dl class='plat'>" + "".join(f"<dt>{e(k)}</dt><dd>{e(str(v))}</dd>" for k, v in rows) + "</dl>")
    # the unoptimised-build pre-flight warning gets its own box instead of the "noisy" list
    warnings = [w for w in doc.get("warnings", []) if not (unopt and w.startswith("This binary's"))]
    if unopt:
        out.append("<div class='invalid'><b>⛔ Unoptimised build: these results are not valid</b>"
                   f"<p>{e(UNOPTIMISED_WARNING.format(langs=unopt))}</p></div>")
    if warnings:
        out.append("<div class='warn'><b>⚠ Pre-flight warnings: results may be noisy</b><ul>"
                   + "".join(f"<li>{e(w)}</li>" for w in warnings) + "</ul></div>")
    out.append(f"<div class='caution'>{e(HOT_PATH_WARNING)}</div>")
    out.append(f"<p class='guide'>Terms used below (cache line, TLB, AVX-512, read for ownership, p99, ...) are "
               f"explained in the <a href='{GLOSSARY_URL}'>glossary</a>.</p>")
    out.append("<nav>" + "".join(f"<a href='#{a}'>{e(t)}</a>" for a, t, _ in sections) + "</nav>")
    for a, t, body in sections:
        out.append(f"<h2 id='{a}'>{e(t)}</h2>{body}")
    out.append(f"<p class='note'>Generated from {e(src_path.name)} by tools/olt-report.py. "
               f"Charts are also saved as standalone SVG files in {e(charts_dir.name)}/.</p>")
    out.append(f"<script>{JS}</script></main></body></html>")

    # ---- Markdown twin: same content, charts as linked SVG files, renders on GitHub
    md = [f"# Latency report\n\n{plat.get('cpu_model', '')}{board} · {plat.get('arch', '')} · "
          f"Linux {plat.get('kernel_release', '')} [{plat.get('preempt_model', '')}]\n",
          "| | |\n|---|---|\n" + "".join(f"| {k} | {md_cell(str(v))} |\n" for k, v in rows)]
    if unopt:
        md.append(f"> [!CAUTION]\n> **Unoptimised build: these results are not valid.** "
                  f"{UNOPTIMISED_WARNING.format(langs=unopt)}\n")
    if warnings:
        md.append("> **⚠ Pre-flight warnings: results may be noisy**\n>\n"
                  + "".join(f"> - {w}\n" for w in warnings))
    md.append(f"> [!WARNING]\n> {HOT_PATH_WARNING}\n")
    md.append(f"Terms used below (cache line, TLB, AVX-512, read for ownership, p99, ...) are explained in the "
              f"[glossary]({GLOSSARY_URL}).\n")
    md.append("".join(f"- [{t}](#{a})\n" for a, t, _ in sections))
    for a, t, body in sections:
        md.append(f'<a id="{a}"></a>\n\n## {t}\n\n{html_to_markdown(body)}')
    md.append(f"*Generated from {src_path.name} by tools/olt-report.py. An interactive version with hover "
              f"details is in report.html next to this file.*\n")
    return "\n".join(out), "\n".join(md), charts


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
    page, md, charts = build_report(doc, src, charts_dir)
    try:
        charts_dir.mkdir(parents=True, exist_ok=True)
        for ch in charts:
            (charts_dir / f"{ch.cid}.svg").write_text(ch.render(standalone=True))
        out.write_text(page)
        out.with_suffix(".md").write_text(md)
    except PermissionError:
        sys.exit(f"error: cannot write {out} (directory created by a sudo run?). "
                 f"Fix with: sudo chown -R $USER: {out.parent}  or pass -o ~/report.html")
    print(f"report written to {out} and {out.with_suffix('.md').name} ({len(charts)} charts, SVGs in {charts_dir}/)")


if __name__ == "__main__":
    main()
