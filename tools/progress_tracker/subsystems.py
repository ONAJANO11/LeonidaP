"""Static subsystem inventories; imports no code from the measured repository.

Requires the pinned Tree-sitter C++/C# grammars in requirements.txt. Python is
parsed with ast. Units are deliberately kept separate and imply no correctness.
"""
import ast
from collections import Counter, defaultdict
from pathlib import PurePosixPath
import re

from tree_sitter import Language, Parser
import tree_sitter_cpp
import tree_sitter_c_sharp

import source_scan as cpp

SHADER_FILES = {'rdna2spirv.py', 'cfg_structure.py', 'lane_masks.py', 'shader_ir.py', 'shader_meta.py',
                'shader_disasm.py', 'shader_carve.py', 'fssm_header.py', 'fssm_unpack.py', 'spirv_stubs.py'}
ASSET_PREFIXES = ('amd_swizzle', 'anim_', 'audio_', 'awc_', 'bc', 'bink_', 'bvh_index', 'classify_carve',
                  'dds_', 'gamedata_', 'glb_', 'hashdict', 'manifest', 'navmesh_', 'page_', 'psin_',
                  'rbf0_', 'restofworld_', 'rpf', 'save', 'story_', 'swizzle_', 'tex_', 'tile_', 'ttpf_', 'world_')
UNSUPPORTED = {'Unsupported', 'NotImplementedError', 'NotSupportedError'}


def symbol(panel, group, name, state, path, line, repo, revision, reason):
    return {'panel': panel, 'interface': group, 'method': name, 'state': state, 'path': path, 'line': line,
            'reason': reason, 'url': f'https://github.com/{repo}/blob/{revision}/{path}#L{line}'}


def panel(key, title, unit, note, symbols):
    counts = defaultdict(Counter)
    for item in symbols:
        counts[item['interface']][item['state']] += 1
    groups = [{'name': name, 'total': sum(c.values()), 'counts': {s: c[s] for s in cpp.STATES}}
              for name, c in counts.items()]
    if not groups:
        raise cpp.ScanError(f'No measurable units found for {title}')
    return {'id': key, 'title': title, 'unit': unit, 'note': note, 'states': list(cpp.STATES),
            'groups': sorted(groups, key=lambda g: (-g['total'], g['name'])), 'symbols': symbols}


def bounded_walk(nodes):
    """Visit a body without attributing a nested definition's code to its parent."""
    for node in nodes:
        yield node
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Lambda)):
            yield from bounded_walk(ast.iter_child_nodes(node))


def py_details(body):
    statements = [n for n in body if not (isinstance(n, ast.Expr) and isinstance(n.value, ast.Constant)
                                         and isinstance(n.value.value, str))]
    nodes = list(bounded_walk(statements))
    raises = [n for n in nodes if isinstance(n, ast.Raise)]
    unsupported = any(isinstance(n, ast.Name) and n.id in UNSUPPORTED for r in raises for n in ast.walk(r))
    calls = set()
    for node in nodes:
        if isinstance(node, ast.Call):
            if isinstance(node.func, ast.Name):
                calls.add(node.func.id)
            elif isinstance(node.func, ast.Attribute) and isinstance(node.func.value, ast.Name) and node.func.value.id in ('self', 'cls'):
                calls.add(node.func.attr)
    unsupported |= bool(calls & {'not_implemented', 'unimplemented', 'stub'})
    trivial = not statements or all(isinstance(n, ast.Pass) or
        (isinstance(n, ast.Expr) and isinstance(n.value, ast.Constant) and n.value.value is Ellipsis) or
        (isinstance(n, ast.Return) and (n.value is None or isinstance(n.value, ast.Constant))) for n in statements)
    pure_stub = unsupported and all(isinstance(n, (ast.Raise, ast.Pass)) for n in statements)
    if any(isinstance(n, ast.Pass) or (isinstance(n, ast.Expr) and isinstance(n.value, ast.Constant)
                                      and n.value.value is Ellipsis) for n in statements) and trivial:
        pure_stub = True
    return {'unsupported': unsupported, 'calls': calls, 'trivial': trivial, 'pure_stub': pure_stub}


def state_of(details):
    if details['pure_stub']:
        return 'stub', 'Explicit placeholder or unsupported-only body'
    if details['unsupported']:
        if details.get('conditional'):
            return 'partial', 'Definition depends on a conditional source branch'
        return 'partial', 'Recognized unsupported marker in body or local helper'
    if details['trivial']:
        return 'noop', 'Empty or constant-return body; may be intentional'
    return 'body', 'Nontrivial body; no recognized unsupported marker'


def python_definitions(path, text):
    try:
        tree = ast.parse(text, filename=path)
    except SyntaxError as error:
        raise cpp.ScanError(f'Python parse error in {path}:{error.lineno}') from error
    functions = []
    def visit(nodes, prefix='', conditional=False):
        seen = set()
        for node in nodes:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name in seen:
                    raise cpp.ScanError(f'Duplicate Python definition in {path}: {prefix}{node.name}')
                seen.add(node.name)
                name = prefix + node.name
                details = py_details(node.body)
                if conditional:
                    details['conditional'], details['unsupported'] = True, True
                functions.append({'name': name, 'owner': prefix, 'line': node.lineno, 'node': node,
                                  'details': details})
                visit(node.body, name + '.', conditional)
            elif isinstance(node, ast.ClassDef):
                visit(node.body, prefix + node.name + '.', conditional)
            elif isinstance(node, (ast.If, ast.Try, ast.With, ast.For, ast.While, ast.AsyncWith, ast.AsyncFor)):
                visit(node.body, prefix, True)
                visit(getattr(node, 'orelse', []), prefix, True)
                visit(getattr(node, 'finalbody', []), prefix, True)
                for handler in getattr(node, 'handlers', []):
                    visit(handler.body, prefix, True)
    visit(tree.body)
    by_name = defaultdict(list)
    for f in functions:
        by_name[f['name']].append(f)
    changed = True
    while changed:
        changed = False
        for f in functions:
            d = f['details']
            if d['unsupported']:
                continue
            callees = [candidate for name in d['calls'] for candidate in
                       by_name.get(f['name'] + '.' + name, by_name.get(f['owner'] + name, by_name.get(name, [])))]
            if any(candidate['details']['unsupported'] for candidate in callees):
                d['unsupported'], changed = True, True
    return tree, functions


def python_panels(files, repo, revision):
    buckets = {'assets': [], 'developer': [], 'shader_py': []}
    for path, text in sorted(files.items()):
        parts = PurePosixPath(path).parts
        if (not (len(parts) == 2 and parts[0] == 'tools') and path != 'make_game.py') or not path.endswith('.py'):
            continue
        name = parts[-1]
        key = 'shader_py' if name in SHADER_FILES else 'assets' if name.startswith(ASSET_PREFIXES) else 'developer'
        _, functions = python_definitions(path, text)
        for function in functions:
            state, reason = state_of(function['details'])
            buckets[key].append(symbol(key, name.removesuffix('.py'), function['name'], state, path,
                                       function['line'], repo, revision, reason))
    return [panel('assets', 'Asset tools · Python', 'definitions',
                  'Defined functions in asset-format tools; not asset recovery success or all missing features.', buckets['assets']),
            panel('developer', 'Other Python tools', 'definitions',
                  'Other top-level tool definitions, including analysis/fuzz helpers; not test-pass counts.', buckets['developer']),
            panel('shader_py', 'Shader pipeline · Python definitions', 'definitions',
                  'Translator and shader-pipeline helper definitions; not translated shaders or validated output.', buckets['shader_py'])]


def selector_names(test, constants):
    if isinstance(test, ast.UnaryOp) and isinstance(test.op, ast.Not):
        return set()
    if isinstance(test, ast.BoolOp):
        return set().union(*(selector_names(v, constants) for v in test.values))
    if isinstance(test, ast.Compare) and len(test.ops) == 1 and isinstance(test.left, ast.Name) and test.left.id == 'op':
        right = test.comparators[0]
        if isinstance(test.ops[0], ast.Eq) and isinstance(right, ast.Constant) and isinstance(right.value, str):
            return {right.value}
        if isinstance(test.ops[0], ast.In):
            if isinstance(right, (ast.Tuple, ast.List, ast.Set)):
                if not all(isinstance(v, ast.Constant) and isinstance(v.value, str) for v in right.elts):
                    raise cpp.ScanError('Nonliteral shader opcode selector sequence')
                return {v.value for v in right.elts}
            name = ast.unparse(right).removeprefix('self.').removeprefix('cls.')
            if name not in constants:
                raise cpp.ScanError(f'Unresolved shader opcode selector table: {name}')
            return constants[name]
    if isinstance(test, ast.Call) and isinstance(test.func, ast.Attribute) and isinstance(test.func.value, ast.Name) and test.func.value.id == 'op':
        if test.func.attr == 'startswith':
            if len(test.args) != 1:
                raise cpp.ScanError('Unrecognized shader prefix selector')
            value = test.args[0]
            values = value.elts if isinstance(value, ast.Tuple) else [value]
            if not all(isinstance(v, ast.Constant) and isinstance(v.value, str) for v in values):
                raise cpp.ScanError('Nonliteral shader prefix selector')
            return {v.value + '*' for v in values}
    return set()


def shader_panel(files, repo, revision):
    path = 'tools/rdna2spirv.py'
    if path not in files:
        raise cpp.ScanError('Shader translator source missing')
    tree, functions = python_definitions(path, files[path])
    constants = {}
    assignments = list(tree.body)
    assignments += [node for cls in tree.body if isinstance(cls, ast.ClassDef) and cls.name == 'Gen' for node in cls.body]
    for node in assignments:
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            value = node.value
            values = value.keys if isinstance(value, ast.Dict) else value.elts if isinstance(value, (ast.Set, ast.Tuple, ast.List)) else []
            if values and all(isinstance(v, ast.Constant) and isinstance(v.value, str) for v in values):
                targets = node.targets if isinstance(node, ast.Assign) else [node.target]
                for target in targets:
                    if isinstance(target, ast.Name):
                        constants[target.id] = {v.value for v in values}
    selectors = {}
    precedence = {'body': 0, 'noop': 1, 'partial': 2, 'stub': 3}
    for function in functions:
        args = {a.arg for a in function['node'].args.args}
        if function['name'] != 'Gen._insn' and not ('op' in args and function['name'].startswith('Gen.')):
            continue
        for node in bounded_walk(function['node'].body):
            if not isinstance(node, ast.If):
                continue
            names = selector_names(node.test, constants)
            if not names:
                continue
            details = py_details(node.body)
            # A called Gen helper's explicit operand refusal is still only static
            # evidence, and is labelled guarded rather than silently fully covered.
            for call in details['calls']:
                helper = next((f for f in functions if f['name'] == 'Gen.' + call), None)
                if helper and helper['details']['unsupported']:
                    details['unsupported'] = True
            state, reason = state_of(details)
            for name in names:
                previous = selectors.get(name)
                if previous is None or precedence[state] > precedence[previous['state']]:
                    prefix = name.split('_', 1)[0]
                    group = {'s': 'Scalar', 'v': 'Vector', 'ds': 'LDS', 'image': 'Images', 'buffer': 'Buffers',
                             'flat': 'Flat memory', 'global': 'Global memory', 'scratch': 'Scratch', 'exp': 'Exports'}.get(prefix, prefix)
                    selectors[name] = symbol('shader', group, name, state, path, node.lineno, repo, revision, reason)
    if 'Gen._insn' not in {f['name'] for f in functions}:
        raise cpp.ScanError('Shader dispatcher Gen._insn missing; update the adapter')
    return panel('shader', 'Shader translator · declared selectors', 'selectors',
                 'Named opcodes and prefix families found in dispatch code; overlaps possible. Not an ISA denominator.',
                 [selectors[name] for name in sorted(selectors)])


def runtime_input(text):
    data = cpp.cpp_input(text.encode())
    data = re.sub(rb'\b__cdecl\b|__declspec\s*\((?:[^()]|\([^()]*\))*\)', lambda m: b' ' * len(m[0]), data)
    # A grammar-only placeholder for empty default initializers. It preserves byte
    # offsets and does not compile or interpret the altered expression.
    data = re.sub(rb'=\s*\{\}', lambda m: b'=0' + b' ' * (len(m[0]) - 2), data)
    macros = rb'(?m)^\s*(?:SHIM_TABLE|LEONIDA_VK_DEVICE_FUNCTIONS|LEONIDA_BINDLESS_FUNCTIONS)\s*\('
    for match in reversed(list(re.finditer(macros, data))):
        pos, depth = match.end(), 1
        while pos < len(data) and depth:
            char = data[pos:pos + 1]
            if data[pos:pos + 2] == b'//':
                end = data.find(b'\n', pos + 2)
                pos = len(data) if end < 0 else end
                continue
            if data[pos:pos + 2] == b'/*':
                end = data.find(b'*/', pos + 2)
                if end < 0:
                    raise cpp.ScanError('Unterminated comment in recognized runtime macro')
                pos = end + 2
                continue
            if char in (b'"', b"'"):
                quote = char
                pos += 1
                while pos < len(data) and data[pos:pos + 1] != quote:
                    pos += 2 if data[pos:pos + 1] == b'\\' else 1
            elif char == b'(':
                depth += 1
            elif char == b')':
                depth -= 1
            pos += 1
        if depth:
            raise cpp.ScanError('Unterminated recognized runtime macro')
        masked = bytes(c if c in (10, 13) else 32 for c in data[match.start():pos])
        data = data[:match.start()] + masked + data[pos:]
    return data


def runtime_panel(files, repo, revision):
    parser = Parser(Language(tree_sitter_cpp.language()))
    functions = []
    excluded = {'d3d12_com.cpp', 'd3d12_com_object.h', 'd3d12_vulkan_device.cpp'}
    for path, text in sorted(files.items()):
        parts = PurePosixPath(path).parts
        if not path.startswith(('runtime/src/', 'runtime/shims/')) or not path.endswith(('.cpp', '.h', '.hpp')):
            continue
        if cpp.EXCLUDED.intersection(parts) or parts[-1] in excluded:
            continue
        data = runtime_input(text)
        root = parser.parse(data).root_node
        if root.has_error:
            errors = [str(n.start_point.row + 1) for n in cpp.walk(root) if n.type == 'ERROR' or n.is_missing]
            raise cpp.ScanError(f'Runtime C++ parse error in {path}: {", ".join(errors)}')
        for node in cpp.walk(root):
            if node.type != 'function_definition':
                continue
            name = cpp.declarator_name(node.child_by_field_name('declarator'))
            body = node.child_by_field_name('body')
            generated = any(n.type == 'default_method_clause' for n in node.named_children)
            deleted = any(n.type == 'delete_method_clause' for n in node.named_children)
            if not name or (body is None and not generated and not deleted):
                raise cpp.ScanError(f'Unrecognized runtime function in {path}:{node.start_point.row + 1}')
            scope, ancestor = [], node.parent
            while ancestor:
                if ancestor.type in ('class_specifier', 'struct_specifier', 'namespace_definition'):
                    n = ancestor.child_by_field_name('name')
                    if n:
                        scope.insert(0, n.text.decode())
                ancestor = ancestor.parent
            details = cpp.details({'body': body}) if body is not None else {
                'calls': set(), 'unsupported': deleted, 'trivial': False, 'pure_stub': deleted}
            functions.append({'name': name, 'owner': '::'.join(scope), 'path': path, 'line': node.start_point.row + 1,
                              'body': body, 'details': details, 'generated': generated})
    changed = True
    while changed:
        changed = False
        for f in functions:
            if f['details']['unsupported']:
                continue
            if any(g['details']['unsupported'] and g['name'] in f['details']['calls'] and g['path'] == f['path']
                   and g['owner'] == f['owner'] for g in functions):
                f['details']['unsupported'], changed = True, True
    symbols = []
    for f in functions:
        state, reason = state_of(f['details'])
        if f['generated']:
            reason = 'Compiler-defaulted definition; not a behavioral verification'
        group = PurePosixPath(f['path']).stem
        symbols.append(symbol('runtime', group, (f['owner'] + '::' if f['owner'] else '') + f['name'], state,
                              f['path'], f['line'], repo, revision, reason))
    return panel('runtime', 'Runtime · C++ definitions', 'definitions',
                 'Loader, shims and backend definitions outside the D3D12 object files. Not all required imports.', symbols)


def csharp_panels(files, repo, revision):
    parser = Parser(Language(tree_sitter_c_sharp.language()))
    definitions, commands = [], []
    for path, text in sorted(files.items()):
        if not path.startswith('tools/rpf9dump/') or not path.endswith('.cs') or {'bin', 'obj'}.intersection(PurePosixPath(path).parts):
            continue
        root = parser.parse(text.encode()).root_node
        if root.has_error:
            raise cpp.ScanError(f'C# parse error in {path}')
        for node in cpp.walk(root):
            if node.type in ('method_declaration', 'local_function_statement', 'constructor_declaration'):
                body = node.child_by_field_name('body')
                name = node.child_by_field_name('name')
                if body is None or name is None:
                    continue
                state, reason = csharp_state(body)
                definitions.append(symbol('asset_cs', PurePosixPath(path).stem, name.text.decode(), state, path,
                                           node.start_point.row + 1, repo, revision, reason))
            if node.type == 'if_statement':
                current = node
                while current.parent is not None:
                    parent = current.parent
                    if parent.type == 'else_clause' or (parent.type == 'if_statement' and parent.child_by_field_name('alternative') == current):
                        current = parent
                    else:
                        break
                if current.parent is None or current.parent.type != 'global_statement':
                    continue  # Nested mode checks refine an existing command.
                condition = node.child_by_field_name('condition')
                if condition is None:
                    continue
                names = cs_modes(condition)
                if names:
                    body = node.child_by_field_name('consequence')
                    state, reason = csharp_state(body)
                    for name in names:
                        commands.append(symbol('exporter', 'rpf9dump commands', name, state, path,
                                               node.start_point.row + 1, repo, revision, reason))
    if len({s['method'] for s in commands}) != len(commands):
        raise cpp.ScanError('Duplicate exporter command selectors; update adapter')
    return [panel('asset_cs', 'Asset exporter · C# definitions', 'definitions',
                  'Defined methods/local functions in the exporter; excludes its third-party submodule.', definitions),
            panel('exporter', 'Asset exporter · command branches', 'commands',
                  'Explicit mode branches in rpf9dump. A branch body does not establish a successful export.', commands)]


def csharp_state(body):
    if body is None:
        raise cpp.ScanError('Exporter command has no recognized body')
    nodes = list(cpp.walk(body))
    unsupported = any(creation.child_by_field_name('type') is not None and
                      creation.child_by_field_name('type').text.decode().split('.')[-1] in ('NotImplementedException', 'NotSupportedException')
                      for n in nodes if n.type in ('throw_statement', 'throw_expression')
                      for creation in cpp.walk(n) if creation.type == 'object_creation_expression')
    statements = [n for n in body.named_children if n.type != 'comment']
    trivial = not statements or all(n.type == 'return_statement' and re.fullmatch(
        rb'return\s*(?:null|true|false|\d+|"[^"\n]*")?\s*;', n.text) for n in statements)
    if body.type == 'arrow_expression_clause' and len(statements) == 1:
        trivial = statements[0].type in ('integer_literal', 'real_literal', 'boolean_literal', 'null_literal', 'string_literal')
    pure = unsupported and all(n.type in ('throw_statement', 'throw_expression') for n in statements)
    return state_of({'unsupported': unsupported, 'trivial': trivial, 'pure_stub': pure})


def cs_modes(condition):
    if condition.type == 'binary_expression':
        left, right = condition.child_by_field_name('left'), condition.child_by_field_name('right')
        operators = {child.type for child in condition.children if not child.is_named}
        if left is not None and right is not None and left.text == b'mode' and '==' in operators:
            return [n.text.decode() for n in cpp.walk(right) if n.type == 'string_literal_content']
        if operators.intersection({'&&', '||'}):
            return [*cs_modes(left), *cs_modes(right)]
    if condition.type == 'is_pattern_expression':
        expression = condition.child_by_field_name('expression')
        if expression is not None and expression.text == b'mode':
            def patterns(node):
                if node.type == 'constant_pattern':
                    return [n.text.decode() for n in cpp.walk(node) if n.type == 'string_literal_content']
                if node.type == 'or_pattern':
                    return [name for child in node.named_children for name in patterns(child)]
                return []
            return patterns(condition.child_by_field_name('pattern'))
    return []


def scan(files, repo, revision):
    py = {p['id']: p for p in python_panels(files, repo, revision)}
    cs_defs, commands = csharp_panels(files, repo, revision)
    for item in cs_defs['symbols']:
        item['panel'], item['interface'] = 'assets', 'rpf9dump/' + item['interface']
    assets = panel('assets', 'Asset tools · Python and C# definitions', 'definitions',
                   'Known asset-tool definitions, including rpf9dump. Not total formats or recovery success.',
                   py['assets']['symbols'] + cs_defs['symbols'])
    return [shader_panel(files, repo, revision), py['shader_py'], runtime_panel(files, repo, revision),
            assets, commands, py['developer']]
