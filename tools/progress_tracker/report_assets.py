"""Badges and a dated history for comparable source measurements."""
from collections import Counter
from copy import deepcopy
import json
from xml.sax.saxutils import escape

BG, TEXT, MUTED = "#0d1117", "#e6edf3", "#8b949e"


def totals(snapshot):
    counts = Counter()
    for group in snapshot["groups"]:
        counts.update(group["counts"])
    return {**dict(counts), "total": sum(group["total"] for group in snapshot["groups"])}


def history_point(snapshot, day, origin):
    metrics = {'methods': totals(snapshot)}
    for domain in snapshot.get('domains', []):
        metrics[domain['id']] = totals({'groups': domain['groups']})
    return {"date": day, "revision": snapshot["revision"], "origin": origin, **totals(snapshot), 'metrics': metrics}


def update_history(previous, snapshot, day, backfill=(), skipped=()):
    history = deepcopy(previous) if previous else {"schema_version": 1, "series": []}
    series = next((s for s in history["series"] if s["rules_id"] == snapshot["rules_id"]), None)
    if series is None:
        series = {"rules_id": snapshot["rules_id"], "reference": snapshot["reference"], "points": [],
                  'metrics': [{'id': 'methods', 'title': 'D3D12 SDK methods', 'unit': 'methods'},
                              *[{k: d[k] for k in ('id', 'title', 'unit')} for d in snapshot.get('domains', [])]]}
        history["series"].append(series)
    points = {point["date"]: point for point in series["points"]}
    for point in backfill:
        points.setdefault(point["date"], point)
    points[day] = history_point(snapshot, day, "observed")
    series["points"] = [points[d] for d in sorted(points)]
    history["current_rules_id"] = snapshot["rules_id"]
    if skipped:
        series["unmeasured_history"] = list(skipped)
    return history


def current_series(history):
    return next(s for s in history["series"] if s["rules_id"] == history["current_rules_id"])


def text(x, y, value, size=13, fill=TEXT):
    return f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}">{escape(value)}</text>'






def badge(label, value, color):
    left, right = max(50, 13 + len(label) * 7), max(35, 14 + len(value) * 7)
    width = left + right
    title = escape(f"{label}: {value}")
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="22" role="img" aria-label="{title}">'
            f'<title>{title}</title><rect width="{width}" height="22" rx="4" fill="#30363d"/>'
            f'<path d="M{left} 0h{right}v22H{left}z" fill="{color}"/>'
            '<g fill="#fff" text-anchor="middle" font-family="Segoe UI,Arial,sans-serif" font-size="11">'
            f'<text x="{left / 2}" y="15">{escape(label)}</text>'
            f'<text x="{left + right / 2}" y="15">{escape(value)}</text></g></svg>\n')


def metric_at(point, key):
    return point.get('metrics', {}).get(key, point if key == 'methods' else None)


def render_history(history, repo):
    from datetime import date
    import math
    series = current_series(history)
    descriptors = series.get('metrics', [{'id': 'methods', 'title': 'D3D12 SDK methods', 'unit': 'methods'}])
    width, height = 1200, 100 + 260 * len(descriptors) + 38
    parts = [f'<rect width="{width}" height="{height}" rx="8" fill="{BG}"/>',
             text(28, 34, 'Leonida subsystem source history', 22),
             text(28, 58, 'Separate scales and units · same scanner rules · no overall completion percentage', 13, MUTED),
             text(28, 80, 'Green: bodies without a recognized unsupported marker. Dashed: known units in that source inventory.', 12, MUTED)]
    for index, descriptor in enumerate(descriptors):
        key = descriptor['id']
        points = [p for p in series['points'] if metric_at(p, key) is not None]
        if not points:
            continue
        left, top, chart_width, chart_height = 66, 135 + index * 260, 1090, 170
        maximum = max(10, math.ceil(max(metric_at(p, key)['total'] for p in points) / 50) * 50)
        first = date.fromisoformat(points[0]['date'])
        days = max(1, (date.fromisoformat(points[-1]['date']) - first).days)
        def x(point):
            return left + chart_width / 2 if len(points) == 1 else left + (date.fromisoformat(point['date']) - first).days / days * chart_width
        def y(value):
            return top + chart_height * (1 - value / maximum)
        parts.append(text(28, top - 18, f"{descriptor['title']} · {descriptor['unit']}", 16))
        for i in range(6):
            value = maximum * i / 5
            py = y(value)
            parts += [f'<path d="M {left} {py:.1f} H {left + chart_width}" stroke="#21262d"/>',
                      text(18, py + 4, str(round(value)), 12, MUTED)]
        for field, color, dash in (('total', '#8b949e', ' stroke-dasharray="6 5"'), ('body', '#2ea043', '')):
            coords = ' '.join(f"{x(p):.1f},{y(metric_at(p, key)[field]):.1f}" for p in points)
            parts.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="3"{dash}/>')
            for point in points:
                title = f"{point['date']}: {metric_at(point, key)[field]} {field} {descriptor['unit']} · {point['revision'][:8]}"
                parts.append(f'<a href="https://github.com/{repo}/commit/{point["revision"]}">'
                             f'<circle cx="{x(point):.1f}" cy="{y(metric_at(point, key)[field]):.1f}" r="4" fill="{color}">'
                             f'<title>{escape(title)}</title></circle></a>')
        stride = max(1, math.ceil(len(points) / 8))
        for i, point in enumerate(points):
            if i % stride == 0 or i == len(points) - 1:
                parts.append(text(min(max(x(point) - 34, 20), width - 95), top + chart_height + 22, point['date'], 12, MUTED))
    parts.append(text(28, height - 14, f"Rules {series['rules_id']} · historic trees rescanned; source presence is not verified correctness.", 12, MUTED))
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
            'role="img" aria-labelledby="title desc" font-family="Segoe UI,Arial,sans-serif">'
            '<title id="title">Leonida source inventories over time</title><desc id="desc">'
            'One chart per measured subsystem, with its own units and scale. See history.md for dated source evidence.</desc>'
            + ''.join(parts) + '</svg>\n')


def history_report(history, repo):
    series = current_series(history)
    lines = ['# Source inventory history', '',
             'Each panel has its own units and denominator. No aggregate project-completion percentage is computed.',
             'Historical points rescan the last available first-parent source commit in each UTC day using the same rules.',
             'Live points record the last observed source revision that UTC day. Missing days are not measurements.',
             'Source definitions/selectors do not prove runtime correctness, shader correctness, or recovery success.', '',
             f"Rules: `{series['rules_id']}`. [D3D12-only reference]({series['reference']['url']}).", '']
    for descriptor in series.get('metrics', [{'id': 'methods', 'title': 'D3D12 SDK methods', 'unit': 'methods'}]):
        lines += ['## ' + descriptor['title'], '', 'Units: ' + descriptor['unit'] + '.', '',
                  '| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |',
                  '|---|---:|---:|---:|---:|---:|---|---|']
        for point in series['points']:
            count = metric_at(point, descriptor['id'])
            if count is None:
                continue
            lines.append(f"| {point['date']} | {count['body']} | {count['partial']} | {count['noop']} | {count['stub']} | {count['total']} | "
                         f"[{point['revision'][:8]}](https://github.com/{repo}/commit/{point['revision']}) | {point['origin']} |")
        lines.append('')
    if series.get('unmeasured_history'):
        lines += ['## Unmeasured historical trees', '', 'These are not treated as zero.', '']
        lines += [f"- {p['date']} ({p['revision'][:8]}): {p['reason']}" for p in series['unmeasured_history']]
    if len(history['series']) > 1:
        lines += ['', 'Older rule versions remain separate in history.json and are not joined to this series.']
    return '\n'.join(lines) + '\n'


def assets(data, history):
    snapshot = data['source']
    count = totals(snapshot)
    result = {'history.json': json.dumps(history, indent=2) + '\n',
              'history.svg': render_history(history, data['repository']),
              'history.md': history_report(history, data['repository']),
              'badge-methods.svg': badge('D3D12 bodies', f"{count['body']}/{count['total']}", '#238636'),
              'badge-source.svg': badge('source', snapshot['revision'][:8], '#1f6feb'),
              'badge-updated.svg': badge('checked', data['observed_at'], '#30363d')}
    for domain in snapshot.get('domains', []):
        count = totals({'groups': domain['groups']})
        result['badge-' + domain['id'] + '.svg'] = badge(domain['id'] + ' bodies', f"{count['body']}/{count['total']}", '#238636')
    return result
