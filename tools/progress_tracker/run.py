"""Generate Leonida-owned source inventories, history, SVGs and badges.

Python 3.12+, Git and the pinned parser packages in requirements.txt. This tool
reads committed source trees; it never imports or executes measured code.
"""
from collections import Counter
from datetime import datetime, timezone
import argparse
import hashlib
from importlib.metadata import version
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape, quoteattr

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import source_scan
import subsystems
import report_assets

SourceError = source_scan.ScanError
COLORS = {'body': '#2ea043', 'partial': '#d29922', 'noop': '#79c0ff', 'stub': '#484f58'}
LABELS = {'body': 'Body present', 'partial': 'Guarded / unsupported', 'noop': 'No-op / constant', 'stub': 'Stub / missing'}
BG, PANEL, TEXT, MUTED, BORDER = '#0d1117', '#161b22', '#e6edf3', '#8b949e', '#30363d'


def scan(repository, revision, config, header, reference):
    files = repository.files(revision)
    result = source_scan.scan(files, header, revision, config['repository'], reference)
    for item in result['symbols']:
        item['panel'] = 'methods'
    domains = subsystems.scan(files, config['repository'], revision)
    result['domains'] = [{k: v for k, v in domain.items() if k != 'symbols'} for domain in domains]
    result['symbols'] += [s for domain in domains for s in domain['symbols']]
    result['files_read'] = len(files)
    rules = b''.join((HERE / name).read_text(encoding='utf-8').replace('\r\n', '\n').encode()
                     for name in ('source_scan.py', 'subsystems.py'))
    packages = ':'.join(version(name) for name in ('tree-sitter', 'tree-sitter-cpp', 'tree-sitter-c-sharp'))
    result['rules_id'] = hashlib.sha256(rules + reference['sha256'].encode() + packages.encode()).hexdigest()[:16]
    return result


def collect(config, observed_at, source):
    panels = [{'id': 'methods', 'title': 'D3D12 · SDK method inventory', 'unit': 'methods',
               'note': 'SDK inheritance chains excluding IUnknown; source classification, not tested compatibility.',
               'states': list(source_scan.STATES), 'groups': source['groups']}, *source.get('domains', [])]
    return {'schema_version': 3, 'repository': config['repository'], 'upstream': config['repository'],
            'observed_at': observed_at, 'source': source, 'panels': panels,
            'verification': 'Source inventories only. Units and denominators differ by panel; no overall completion percentage. '
                            'No GPU, game, shader-corpus or asset-recovery tests are run by this tracker.'}


def treemap(items, x, y, width, height):
    remaining = [(name, size * width * height / sum(v for _, v in items)) for name, size in items]
    output = {}
    def aspect(row, side):
        total = sum(v for _, v in row)
        return max(max(side * side * v / total ** 2, total ** 2 / (side * side * v)) for _, v in row)
    while remaining:
        side, count = min(width, height), 1
        while count < len(remaining) and aspect(remaining[:count + 1], side) <= aspect(remaining[:count], side):
            count += 1
        row, remaining = remaining[:count], remaining[count:]
        area = sum(v for _, v in row)
        if width >= height:
            strip, offset = area / height, y
            for name, value in row:
                output[name] = (x, offset, strip, value / strip)
                offset += value / strip
            x, width = x + strip, width - strip
        else:
            strip, offset = area / width, x
            for name, value in row:
                output[name] = (offset, y, value / strip, strip)
                offset += value / strip
            y, height = y + strip, height - strip
    return output


def fit_cells(total, width, height):
    for size in range(min(30, int(width), int(height)), 1, -1):
        columns = int((width + 2) // (size + 2))
        if columns and math.ceil(total / columns) * (size + 2) - 2 <= height:
            return size, columns
    raise SourceError('Category too small for all cells; increase its panel size')


def svg_text(x, y, value, size=14, fill=TEXT, bold=False):
    weight = ' font-weight="600"' if bold else ''
    return f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}"{weight}>{escape(value)}</text>'


def render_svg(data):
    width, y = 1200, 144
    source = data['source']
    parts = [svg_text(28, 40, 'Leonida source tracker', 25, bold=True),
             svg_text(28, 66, 'Separate source inventories · not a measure of game completion.', 14, MUTED),
             svg_text(28, 90, f"Checked {data['observed_at']} · source {source['revision'][:12]} · automated in this repository", 13, MUTED),
             svg_text(28, 114, 'One square per stated unit. Definitions and selectors do not establish correct behavior.', 13, MUTED)]
    for panel in data['panels']:
        groups, states = panel['groups'], panel['states']
        total = sum(g['total'] for g in groups)
        chart_height = max(290, math.ceil(total / 1500) * 290, math.ceil(len(groups) / 30) * 290)
        if panel['id'] == 'exporter':
            chart_height = 110
        boxes = treemap([(g['name'], g['total']) for g in groups], 40, y + 108, width - 80, chart_height)
        contents, unlabelled = [], []
        for group in groups:
            gx, gy, gw, gh = boxes[group['name']]
            short = group['name'].removeprefix('ID3D12')
            summary = ' / '.join(str(group['counts'][s]) for s in states)
            labelled = max(len(short) * 7.2, len(summary) * 6.5) <= gw - 14 and gh > 88
            inset = 52 if labelled else 6
            size, cols = fit_cells(group['total'], gw - 12, gh - inset - 6)
            details = ', '.join(f"{group['counts'][s]} {LABELS[s]}" for s in states)
            contents.append(f'<g><title>{escape(group["name"] + ": " + details)}</title>'
                            f'<rect x="{gx:.1f}" y="{gy:.1f}" width="{gw:.1f}" height="{gh:.1f}" '
                            f'fill="{PANEL}" stroke="{BG}" stroke-width="3"/>')
            if labelled:
                contents += [svg_text(gx + 7, gy + 20, short, 14, bold=True), svg_text(gx + 7, gy + 39, summary, 12, MUTED)]
            else:
                unlabelled.append(f'{short}: {summary}')
            index = 0
            for state in states:
                symbols = [s for s in source['symbols'] if s.get('panel', 'methods') == panel['id']
                           and s['interface'] == group['name'] and s['state'] == state]
                if len(symbols) != group['counts'][state]:
                    raise SourceError(f"Missing named evidence for {panel['id']}/{group['name']}")
                for item in symbols:
                    cx, cy = gx + 6 + index % cols * (size + 2), gy + inset + index // cols * (size + 2)
                    title = escape(f"{item['method']}: {item['reason']}")
                    contents.append(f'<a href={quoteattr(item["url"])}><title>{title}</title>'
                                    f'<rect class="cell" data-state="{state}" x="{cx:.1f}" y="{cy:.1f}" '
                                    f'width="{size}" height="{size}" fill="{COLORS[state]}"/></a>')
                    index += 1
            contents.append('</g>')
        notes, line = [], ''
        for label in unlabelled:
            if len(line + label) > 140:
                notes.append(line)
                line = ''
            line += (' · ' if line else '') + label
        if line:
            notes.append(line)
        panel_height = 126 + chart_height + 19 * len(notes)
        parts.append(f'<rect x="24" y="{y}" width="{width - 48}" height="{panel_height}" rx="8" '
                     f'fill="{PANEL}" stroke="{BORDER}"/>')
        counts = Counter()
        for group in groups:
            counts.update(group['counts'])
        parts += [svg_text(40, y + 27, f"{panel['title']}: {total} {panel['unit']}", 18, bold=True),
                  svg_text(40, y + 51, panel['note'], 13, MUTED)]
        lx = 40
        for state in states:
            label = f'{LABELS[state]} ({counts[state]})'
            parts.append(f'<rect x="{lx}" y="{y + 72}" width="11" height="11" fill="{COLORS[state]}"/>')
            parts.append(svg_text(lx + 18, y + 82, label, 12))
            lx += 34 + len(label) * 7
        parts.append(svg_text(40, y + 101, 'Group counts follow legend order. Open the evidence table for names and source links.', 11, MUTED))
        parts.extend(contents)
        for i, note in enumerate(notes):
            parts.append(svg_text(40, y + 108 + chart_height + 17 + 19 * i, note, 11, MUTED))
        y += panel_height + 18
    parts.append(svg_text(28, y + 12, 'Definitions, scope and source links: docs/progress/report.md · no overall completion percentage.', 12, MUTED))
    height = y + 36
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
            'role="img" aria-labelledby="title desc" font-family="Segoe UI,Helvetica,Arial,sans-serif">'
            '<title id="title">Leonida subsystem source inventories</title>'
            '<desc id="desc">Separate D3D12, shader, runtime and tool panels. Read report.md for units, limits, and named source evidence.</desc>'
            f'<rect width="{width}" height="{height}" fill="{BG}"/>' + ''.join(parts) + '</svg>\n')


def md(value):
    return str(value).replace('|', '\\|').replace('\n', ' ')


def render_report(data):
    source = data['source']
    lines = ['# Leonida source inventories', '', f"Checked: {data['observed_at']}", '', data['verification'], '',
             f"[Measured source {source['revision'][:12]}](https://github.com/{data['repository']}/commit/{source['revision']}).",
             f"Read {source.get('files_read', source['files_scanned'])} committed production source files. Rules: `{source['rules_id']}`.",
             f"D3D12 only: [pinned Microsoft reference]({source['reference']['url']}), SHA-256 `{source['reference']['sha256']}`.", '',
             '## Classification', '',
             'Green: nontrivial body without a recognized unsupported marker. Amber: guarded/unsupported or conditionally defined. '
             'Blue: simple no-op/constant-return body, which may be intentional. Grey: inherited stub, explicit placeholder, or unsupported-only body.', '',
             'The shader selector denominator contains names and prefix families declared by dispatch code. Families may overlap; '
             'it is not a complete ISA reference. Definitions in the other panels count known source definitions, not missing features. '
             'Compiler-defaulted C++ functions are identified in the evidence. Macro expansions, third-party code and test fixtures are excluded.', '',
             'See [tracker documentation](../../tools/progress_tracker/README.md) for scope and limitations.', '']
    for panel in data['panels']:
        lines += ['## ' + panel['title'], '', panel['note'], '',
                  '| Group | Total | ' + ' | '.join(LABELS[s] for s in panel['states']) + ' |',
                  '|---|---:|' + '---:|' * len(panel['states'])]
        for group in panel['groups']:
            lines.append(f"| {md(group['name'])} | {group['total']} | " + ' | '.join(str(group['counts'][s]) for s in panel['states']) + ' |')
        lines += ['', '<details><summary>Named source evidence</summary>', '',
                  '| Group | Unit | Classification | Reason | Source |', '|---|---|---|---|---|']
        for item in source['symbols']:
            if item.get('panel', 'methods') == panel['id']:
                lines.append(f"| {md(item['interface'])} | {md(item['method'])} | {LABELS[item['state']]} | {md(item['reason'])} | "
                             f"[{md(item['path'])}:{item['line']}]({item['url']}) |")
        lines += ['', '</details>', '']
    return '\n'.join(lines).rstrip() + '\n'


def publish(data, output, history):
    count = 0
    for panel in data['panels']:
        for group in panel['groups']:
            values = [group['counts'][s] for s in panel['states']]
            if any(not isinstance(v, int) or v < 0 for v in values) or sum(values) != group['total'] or not group['total']:
                raise SourceError('Group counts do not match the denominator')
            count += group['total']
    if count > 50000:
        raise SourceError('Inventory exceeds 50,000 units; review renderer capacity')
    payloads = {'progress.json': json.dumps(data, indent=2, ensure_ascii=False) + '\n',
                'dashboard.svg': render_svg(data), 'report.md': render_report(data), **report_assets.assets(data, history)}
    output.mkdir(parents=True, exist_ok=True)
    for name, content in payloads.items():
        fd, temporary = tempfile.mkstemp(dir=output, prefix='.pending-')
        try:
            with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as handle:
                handle.write(content)
            os.replace(temporary, output / name)
        finally:
            Path(temporary).unlink(missing_ok=True)


def local_repository(root, config):
    result = subprocess.run(['git', '-C', str(root), 'rev-parse', '--absolute-git-dir'], check=True,
                            capture_output=True, text=True)
    repository = source_scan.SourceRepository(config['repository'], root / 'build/progress-tracker')
    repository.gitdir = Path(result.stdout.strip())
    return repository


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=PROJECT)
    parser.add_argument('--config', type=Path, default=HERE / 'tracker.json')
    parser.add_argument('--output', type=Path, default=PROJECT / 'docs/progress')
    parser.add_argument('--revision', help='Committed source revision; defaults to the checked-out HEAD')
    args = parser.parse_args(argv)
    try:
        config = json.loads(args.config.read_text(encoding='utf-8'))
        repository = local_repository(args.root, config)
        revision = repository.git('rev-parse', '--verify', (args.revision or 'HEAD') + '^{commit}').decode().strip()
        now = datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M UTC')
        header, reference = source_scan.load_reference(config['sdk_reference'], args.root / 'build/progress-tracker')
        snapshot = scan(repository, revision, config, header, reference)
        history_path = args.output / 'history.json'
        previous = json.loads(history_path.read_text(encoding='utf-8')) if history_path.exists() else None
        backfill, skipped = [], []
        if not previous or not any(s['rules_id'] == snapshot['rules_id'] for s in previous['series']):
            for day, historical_revision, _ in repository.history_revisions():
                if day >= now[:10]:
                    continue
                try:
                    historical = scan(repository, historical_revision, config, header, reference)
                    backfill.append(report_assets.history_point(historical, day, 'rescanned'))
                except SourceError as error:
                    skipped.append({'date': day, 'revision': historical_revision, 'reason': str(error)})
        history = report_assets.update_history(previous, snapshot, now[:10], backfill, skipped)
        data = collect(config, now, snapshot)
        publish(data, args.output, history)
    except (SourceError, OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        print(f'Tracker refresh failed; previous published output retained: {error}', file=sys.stderr)
        return 1
    print('Updated source inventories: ' + ', '.join(f"{p['id']}={sum(g['total'] for g in p['groups'])} {p['unit']}" for p in data['panels']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
