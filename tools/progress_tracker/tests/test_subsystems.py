import ast
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE))
import subsystems as s
import source_scan as cpp

SHADER = '''
OPS = {'v_add': 'Add', 'v_mul': 'Multiply'}
class Unsupported(Exception): pass
class Gen:
    def _insn(self, ins):
        op = ins['op']
        if op in OPS:
            self.emit(op)
        elif op == 's_wait':
            return
        elif op.startswith('image_') and not op.startswith('image_bad'):
            if invalid(): raise Unsupported('operand')
            self.emit(op)
        else:
            raise Unsupported(op)
    def emit(self, value):
        write(value)
'''


class PythonInventoryTests(unittest.TestCase):
    def test_python_is_parsed_without_execution(self):
        text = "raise RuntimeError('must not execute')\ndef body():\n    return compute()\n"
        _, functions = s.python_definitions('tool.py', text)
        self.assertEqual(functions[0]['name'], 'body')
        self.assertEqual(s.state_of(functions[0]['details'])[0], 'body')

    def test_docstrings_comments_and_nested_helpers_do_not_pollute_parent(self):
        text = '''def parent():
    """Unsupported NotImplementedError"""
    def unused():
        raise NotImplementedError()
    return compute()
'''
        _, functions = s.python_definitions('tool.py', text)
        states = {f['name']: s.state_of(f['details'])[0] for f in functions}
        self.assertEqual(states, {'parent': 'body', 'parent.unused': 'stub'})

    def test_called_local_stub_propagates_and_placeholders_are_visible(self):
        text = '''def parent():
    def helper():
        raise NotImplementedError()
    return helper()
def placeholder(): pass
def constant(): return 0
'''
        _, functions = s.python_definitions('tool.py', text)
        states = {f['name']: s.state_of(f['details'])[0] for f in functions}
        self.assertEqual(states['parent'], 'partial')
        self.assertEqual(states['placeholder'], 'stub')
        self.assertEqual(states['constant'], 'noop')

    def test_conditional_definitions_are_explicitly_guarded(self):
        _, functions = s.python_definitions('tool.py', 'if platform:\n def action():\n  return work()\n')
        self.assertEqual(s.state_of(functions[0]['details'])[0], 'partial')
        self.assertIn('conditional', s.state_of(functions[0]['details'])[1])

    def test_duplicate_or_malformed_definitions_fail(self):
        for text in ('def a(: pass', 'def a(): pass\ndef a(): pass'):
            with self.assertRaises(cpp.ScanError):
                s.python_definitions('tool.py', text)

    def test_scope_roles_and_exclusion_of_tracker_tests(self):
        files = {'tools/rdna2spirv.py': SHADER, 'tools/tex_new.py': 'def read(): return parse()',
                 'tools/fuzz_new.py': 'def check(): return compare()',
                 'tools/progress_tracker/fake.py': 'def fake(): pass', 'tools/tests/fake.py': 'def fake(): pass'}
        panels = {p['id']: p for p in s.python_panels(files, 'example/repo', 'abc')}
        self.assertEqual(len(panels['assets']['symbols']), 1)
        self.assertEqual(len(panels['developer']['symbols']), 1)
        self.assertTrue(all(item['method'] != 'fake' for p in panels.values() for item in p['symbols']))


class ShaderSelectorTests(unittest.TestCase):
    def test_literal_tables_prefixes_and_rejections_have_separate_meanings(self):
        result = s.shader_panel({'tools/rdna2spirv.py': SHADER}, 'example/repo', 'abc')
        states = {x['method']: x['state'] for x in result['symbols']}
        self.assertEqual(states, {'v_add': 'body', 'v_mul': 'body', 's_wait': 'noop', 'image_*': 'partial'})
        self.assertNotIn('image_bad*', states)
        self.assertIn('Not an ISA', result['note'])

    def test_unknown_table_or_missing_dispatcher_is_not_guessed(self):
        for text in (SHADER.replace('op in OPS', 'op in dynamic_table'), SHADER.replace('_insn', '_new_dispatch')):
            with self.assertRaises(cpp.ScanError):
                s.shader_panel({'tools/rdna2spirv.py': text}, 'example/repo', 'abc')

    def test_dynamic_prefix_selector_fails(self):
        with self.assertRaises(cpp.ScanError):
            s.selector_names(ast.parse('op.startswith(dynamic)', mode='eval').body, {})


class RuntimeTests(unittest.TestCase):
    def test_shim_macros_defaults_and_calling_conventions_preserve_lines(self):
        text = '''__declspec(dllexport) int __cdecl Read() { return compute(); }
int __stdcall Other(std::span<int> v = {}) { return 0; }
SHIM_TABLE(example,
 {"module", "Read", reinterpret_cast<void*>(&Read)},
);
'''
        masked = s.runtime_input(text)
        self.assertEqual(len(masked), len(text.encode()))
        self.assertEqual(masked.count(b'\n'), text.count('\n'))
        result = s.runtime_panel({'runtime/shims/example.cpp': text}, 'example/repo', 'abc')
        self.assertEqual([(x['method'], x['line'], x['state']) for x in result['symbols']], [('Read', 1, 'body'), ('Other', 2, 'noop')])

    def test_reference_returns_and_defaulted_members_are_counted(self):
        text = 'struct Item { bool operator==(const Item&) const = default; };\nItem& get() { return object; }'
        result = s.runtime_panel({'runtime/src/example.h': text}, 'example/repo', 'abc')
        self.assertEqual(len(result['symbols']), 2)
        self.assertTrue(any('Compiler-defaulted' in x['reason'] for x in result['symbols']))

    def test_malformed_runtime_and_unknown_macros_fail(self):
        for text in ('int f() { return @; }', 'UNKNOWN_MACRO(X)\nint f() { return 0; }'):
            with self.assertRaises(cpp.ScanError):
                s.runtime_panel({'runtime/src/example.cpp': text}, 'example/repo', 'abc')

    def test_test_sources_and_d3d12_object_files_are_excluded(self):
        files = {'runtime/src/real.cpp': 'int real() { return work(); }',
                 'runtime/src/tests/fake.cpp': 'int fake() { return 0; }',
                 'runtime/shims/d3d12_com.cpp': 'int duplicate() { return 0; }'}
        result = s.runtime_panel(files, 'example/repo', 'abc')
        self.assertEqual([x['method'] for x in result['symbols']], ['real'])


class ExporterTests(unittest.TestCase):
    def test_nested_mode_checks_do_not_double_count_commands(self):
        code = '''string mode = args[0];
if (mode is "models" or "textures") { if (mode == "models") ExportModels(); else ExportTextures(); }
else if (mode == "audio") { ExportAudio(); }
static int helper() => 1;
'''
        definitions, commands = s.csharp_panels({'tools/rpf9dump/Program.cs': code}, 'example/repo', 'abc')
        self.assertEqual({x['method'] for x in commands['symbols']}, {'models', 'textures', 'audio'})
        self.assertEqual(definitions['symbols'][0]['state'], 'noop')

    def test_explicit_unsupported_throw_is_not_a_message_string(self):
        code = '''string mode = args[0];
if (mode == "x") { Export(); }
static int pending() => throw new NotImplementedException();
static int message() { throw new Exception("NotImplementedException"); }
'''
        definitions, _ = s.csharp_panels({'tools/rpf9dump/Program.cs': code}, 'example/repo', 'abc')
        states = {x['method']: x['state'] for x in definitions['symbols']}
        self.assertEqual(states['pending'], 'stub')
        self.assertEqual(states['message'], 'body')

    def test_changed_dispatch_structure_fails(self):
        with self.assertRaises(cpp.ScanError):
            s.csharp_panels({'tools/rpf9dump/Program.cs': 'static int helper() => 1;'}, 'example/repo', 'abc')


if __name__ == '__main__':
    unittest.main()
