#!/usr/bin/env python3
"""Draw the CPU warm-up of each benchmark from results.json as one HTML page.

Usage: warmup-chart.py RESULTS_JSON OUT_HTML

Each panel plots the reference loop (ns per multiply, lower is faster) against the
time since the warm-up started. The dashed line marks where the clock settled (the
start of the stable windows), and the shaded band is the hold that follows it. The
curve should be flat inside the band; a hold that ends while the curve still falls
is too short.
"""

import html
import json
import statistics
import sys
from pathlib import Path

W, H, PAD = 640, 220, 48
ACCENT = "#1f6fb4"


def esc(s):
    return html.escape(str(s), quote=True)


def panel(w):
    trace = w.get("trace") or []
    if not trace:
        return "<p class=muted>No warm-up trace (warm-up off or not run).</p>"
    xs = [t for t, _ in trace]
    ys = [v for _, v in trace]
    x_max = max(xs) or 1.0
    y_lo, y_hi = min(ys), max(ys)
    pad = (y_hi - y_lo) * 0.1 or 0.01
    y_lo, y_hi = y_lo - pad, y_hi + pad

    def X(t):
        return PAD + t / x_max * (W - 2 * PAD)

    def Y(v):
        return H - PAD - (v - y_lo) / (y_hi - y_lo) * (H - 2 * PAD)

    out = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="warm-up trace">']
    out.append(f'<line x1="{PAD}" y1="{H - PAD}" x2="{W - PAD}" y2="{H - PAD}" stroke="#999"/>')
    out.append(f'<line x1="{PAD}" y1="{PAD}" x2="{PAD}" y2="{H - PAD}" stroke="#999"/>')
    out.append(f'<text x="{PAD}" y="{H - PAD + 16}" class=axis>0 ms</text>')
    out.append(f'<text x="{W - PAD}" y="{H - PAD + 16}" text-anchor="end" class=axis>{x_max:.0f} ms</text>')
    out.append(f'<text x="{PAD - 6}" y="{Y(y_hi) + 4:.1f}" text-anchor="end" class=axis>{y_hi:.2f}</text>')
    out.append(f'<text x="{PAD - 6}" y="{Y(y_lo) - 2:.1f}" text-anchor="end" class=axis>{y_lo:.2f}</text>')

    ramp = float(w.get("ramp_ms", 0))
    total = float(w.get("total_ms", 0))
    if w.get("stable"):
        x0, x1 = X(ramp), X(total)
        out.append(
            f'<rect x="{x0:.1f}" y="{PAD}" width="{x1 - x0:.1f}" height="{H - 2 * PAD}" '
            f'fill="{ACCENT}" fill-opacity="0.08"/>'
        )
        out.append(
            f'<line x1="{x0:.1f}" x2="{x0:.1f}" y1="{PAD}" y2="{H - PAD}" '
            f'stroke="{ACCENT}" stroke-dasharray="4 3"/>'
        )
        out.append(f'<text x="{x0 + 4:.1f}" y="{PAD + 12}" class=label>settled at {ramp:.0f} ms</text>')
    else:
        out.append(f'<text x="{W - PAD - 4}" y="{PAD + 12}" text-anchor="end" class=warn>never settled</text>')

    pts = " ".join(f"{X(t):.1f},{Y(v):.1f}" for t, v in trace)
    out.append(f'<polyline points="{pts}" fill="none" stroke="{ACCENT}" stroke-width="1.5"/>')
    out.append("</svg>")
    return "\n".join(out)


def summary_rows(warmups):
    rows = []
    for w in warmups:
        rows.append(
            "<tr>"
            f"<td>{esc(w.get('benchmark'))}</td>"
            f"<td>{'yes' if w.get('stable') else 'no'}</td>"
            f"<td>{float(w.get('ramp_ms', 0)):.1f}</td>"
            f"<td>{float(w.get('total_ms', 0)):.1f}</td>"
            f"<td>{float(w.get('reference_ns', 0)):.3f}</td>"
            "</tr>"
        )
    return "\n".join(rows)


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    data = json.loads(Path(argv[1]).read_text())
    warmups = data.get("warmups") or []
    ramps = [float(w["ramp_ms"]) for w in warmups if w.get("stable")]
    summary = (
        f"Settled in {len(ramps)} of {len(warmups)} warm-ups. "
        f"Median time to settle {statistics.median(ramps):.1f} ms, slowest {max(ramps):.1f} ms."
        if ramps
        else "No warm-up settled."
    )
    platform = data.get("platform", {})
    panels = "\n".join(
        f"<section><h2>{esc(w.get('benchmark'))}</h2>{panel(w)}</section>" for w in warmups
    )
    page = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CPU warm-up</title>
<style>
:root {{ --ink:#1d1d1f; --muted:#666; --bg:#fff; --rule:#ddd; }}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ --ink:#e8e8ea; --muted:#9a9aa0; --bg:#141416; --rule:#333; }} }}
:root[data-theme="dark"] {{ --ink:#e8e8ea; --muted:#9a9aa0; --bg:#141416; --rule:#333; }}
body {{ background:var(--bg); color:var(--ink); font:15px/1.45 system-ui,sans-serif; margin:0 auto; max-width:760px; padding:16px; }}
h1 {{ font-size:20px; margin:4px 0 8px; }}
h2 {{ font-size:15px; margin:18px 0 4px; font-family:ui-monospace,monospace; }}
svg {{ width:100%; height:auto; display:block; }}
.axis {{ font-size:11px; fill:var(--muted); }}
.label {{ font-size:12px; fill:{ACCENT}; }}
.warn {{ font-size:12px; fill:#b03a2e; }}
.muted {{ color:var(--muted); }}
table {{ border-collapse:collapse; width:100%; font-size:13px; margin:8px 0 16px; }}
td, th {{ border-bottom:1px solid var(--rule); padding:4px 6px; text-align:right; }}
td:first-child, th:first-child {{ text-align:left; font-family:ui-monospace,monospace; }}
</style></head>
<body>
<h1>CPU warm-up before each benchmark</h1>
<p class=muted>{esc(platform.get('cpu_model', ''))} · governor {esc(platform.get('governor', ''))}</p>
<p>{esc(summary)}</p>
<p class=muted>Each panel plots the reference loop (ns per multiply, lower is faster)
against time since the warm-up started. The dashed line marks where the clock settled,
and the shaded band is the hold after it. The curve should be flat inside the band.</p>
<table><thead><tr><th>benchmark</th><th>settled</th><th>ramp ms</th><th>total ms</th><th>ns / multiply</th></tr></thead>
<tbody>
{summary_rows(warmups)}
</tbody></table>
{panels}
</body></html>
"""
    Path(argv[2]).write_text(page)
    print(f"wrote {argv[2]} ({len(warmups)} warm-ups)")


if __name__ == "__main__":
    main(sys.argv)
