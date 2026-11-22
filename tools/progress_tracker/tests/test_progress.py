import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

HERE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('tracker_run', HERE / 'run.py')
p = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(p)


def snapshot(group_count=1):
    groups, symbols = [], []
    for i in range(group_count):
        name = 'ID3D12Sample' + str(i)
        counts = {'body': i + 1, 'partial': 1, 'noop': 1, 'stub': 1}
        groups.append({'name': name, 'total': sum(counts.values()), 'counts': counts})
        for state, count in counts.items():
            symbols.extend({'panel': 'methods', 'interface': name, 'method': f'{state}{n}', 'state': state,
                            'reason': 'Synthetic source evidence', 'path': 'runtime/src/sample.cpp', 'line': n + 1,
                            'url': f'https://github.com/example/repo/blob/{"a" * 40}/runtime/src/sample.cpp#L{n + 1}'} for n in range(count))
    return {'revision': 'a' * 40, 'rules_id': 'rule-a', 'files_scanned': 1,
            'reference': {'url': 'https://example.org/sdk', 'sha256': 'b' * 64},
            'groups': groups, 'symbols': symbols, 'domains': []}


class NativeTrackerTests(unittest.TestCase):
    def setUp(self):
        self.source = snapshot()
        self.data = p.collect({'repository': 'example/repo'}, '2026-01-01 00:00 UTC', self.source)
        self.history = p.report_assets.update_history(None, self.source, '2026-01-01')

    def test_named_square_and_color_for_every_source_unit(self):
        svg = ET.fromstring(p.render_svg(self.data))
        cells = [node for node in svg.iter() if node.get('class') == 'cell']
        self.assertEqual(len(cells), 4)
        self.assertTrue(all(node.get('fill') == p.COLORS[node.get('data-state')] for node in cells))
        self.assertEqual(len([n for n in svg.iter() if n.tag.endswith('a')]), len(cells))
        self.assertNotIn('omdev74', p.render_svg(self.data))

    def test_missing_named_evidence_stops_render(self):
        self.source['symbols'].pop()
        with self.assertRaises(p.SourceError):
            p.render_svg(self.data)

    def test_svg_and_markdown_escape_names(self):
        self.source['symbols'][0]['method'] = '<script> | function'
        svg = p.render_svg(self.data)
        ET.fromstring(svg)
        self.assertNotIn('<script>', svg)
        self.assertIn('\\|', p.render_report(self.data))

    def test_expansion_keeps_cells_inside_category_bounds(self):
        data = p.collect({'repository': 'example/repo'}, 'now', snapshot(35))
        svg = ET.fromstring(p.render_svg(data))
        ns = {'s': 'http://www.w3.org/2000/svg'}
        for group in svg.findall('s:g', ns):
            border = group.find('s:rect', ns)
            x, y, w, h = (float(border.get(k)) for k in ('x', 'y', 'width', 'height'))
            for cell in group.findall('.//s:rect[@class="cell"]', ns):
                cx, cy, cw, ch = (float(cell.get(k)) for k in ('x', 'y', 'width', 'height'))
                self.assertGreaterEqual(cx, x)
                self.assertGreaterEqual(cy, y)
                self.assertLessEqual(cx + cw, x + w + 0.2)
                self.assertLessEqual(cy + ch, y + h + 0.2)

    def test_failed_generation_preserves_existing_assets(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            p.publish(self.data, output, self.history)
            before = {f.name: f.read_bytes() for f in output.iterdir()}
            self.source['groups'][0]['total'] += 1
            with self.assertRaises(p.SourceError):
                p.publish(self.data, output, self.history)
            self.assertEqual(before, {f.name: f.read_bytes() for f in output.iterdir()})

    def test_outputs_include_native_history_badges_and_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            p.publish(self.data, output, self.history)
            self.assertEqual(json.loads((output / 'progress.json').read_text(encoding='utf-8')), self.data)
            for name in ('dashboard.svg', 'history.svg', 'badge-methods.svg', 'badge-updated.svg'):
                ET.parse(output / name)
            self.assertIn('Named source evidence', (output / 'report.md').read_text())

    def test_subsystem_units_are_not_aggregated(self):
        domain = {'id': 'shader', 'title': 'Shader selectors', 'unit': 'selectors', 'note': 'Distinct denominator',
                  'states': list(p.source_scan.STATES), 'groups': [{'name': 'vector', 'total': 2,
                       'counts': {'body': 1, 'partial': 1, 'noop': 0, 'stub': 0}}]}
        self.source['domains'] = [domain]
        self.source['symbols'] += [dict(self.source['symbols'][i], panel='shader', interface='vector') for i in (0, 1)]
        data = p.collect({'repository': 'example/repo'}, 'now', self.source)
        history = p.report_assets.update_history(None, self.source, '2026-01-01')
        point = p.report_assets.current_series(history)['points'][0]
        self.assertEqual(point['metrics']['methods']['total'], 4)
        self.assertEqual(point['metrics']['shader']['total'], 2)
        self.assertNotIn('overall', point['metrics'])
        self.assertEqual([panel['unit'] for panel in data['panels']], ['methods', 'selectors'])
        ET.fromstring(p.report_assets.render_history(history, 'example/repo'))

    def test_integration_has_no_personal_repo_or_tokens(self):
        root = HERE.parents[1]
        readme = (root / 'README.md').read_text(encoding='utf-8')
        workflow = (root / '.github/workflows/progress.yml').read_text(encoding='utf-8')
        self.assertNotIn('omdev74/leonida-progress', readme)
        self.assertIn('docs/progress/dashboard.svg', readme)
        self.assertIn("github.event_name != 'pull_request'", workflow)
        self.assertIn('!github.event.repository.fork', workflow)
        self.assertIn('contents: write', workflow)
        self.assertNotIn('secrets.', workflow)
        self.assertNotIn('pull_request_target', workflow)
        self.assertIn('git add docs/progress', workflow)


if __name__ == '__main__':
    unittest.main()
