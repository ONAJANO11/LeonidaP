"""Which shaders are current, for make_game.py's --indexed runs of fssm_unpack.py, shader_disasm.py and rdna2spirv.py.

A carve folder keeps the files of an earlier carve (a game update): .dxbc, fssm/*.fssm and disasm/*.s of shaders no
longer in the game. The current shaders are the ones listed by the files each stage writes last: the carve's index.json
("file" of each record), fssm_unpack.py's fssm_index.json and shader_disasm.py's disasm/selected.json ("name").
Only bare file names are taken from them, so no record leads outside its folder. Nothing is ever deleted.
"""
import json
import os

SELECTED = "selected.json"   # shader_disasm.py --indexed: the shaders that produced a .s file, written last


def bare(name) -> bool:
    """A plain file name (spaces and hyphens are fine): no folder part, not "." or "..", no NUL."""
    return (isinstance(name, str) and name not in ("", ".", "..") and "\0" not in name
            and "/" not in name and "\\" not in name and os.path.basename(name) == name)


def listed(path: str, key: str, suffix: str = "") -> set[str]:
    """Read the names from one producer's inventory. A partial or invalid inventory must not certify a build."""
    try:
        with open(path, encoding="utf-8") as f:
            rows = json.load(f)
    except (OSError, ValueError) as e:
        raise ValueError(f"cannot read {path}: {e}") from None
    if not isinstance(rows, list) or not rows:
        raise ValueError(f"{path} must list at least one shader")
    names = set()
    for row in rows:
        value = row.get(key) if isinstance(row, dict) else None
        if not isinstance(value, str) or not value.endswith(suffix):
            raise ValueError(f"{path} has an invalid {key} entry")
        name = value.removesuffix(suffix)
        if not bare(name):
            raise ValueError(f"{path} has an invalid shader name: {value!r}")
        names.add(name)
    return names


def current(root: str) -> list:
    """Sorted: the shaders both the carve and the unpack in `root` (the carve folder) list."""
    unpacked = listed(os.path.join(root, "fssm_index.json"), "name")
    names = sorted(unpacked & listed(os.path.join(root, "index.json"), "file", ".dxbc"))
    if not names:
        raise ValueError(f"no shader is listed by both index.json and fssm_index.json in {root}")
    return names


def disassembled(disasm: str) -> list:
    """The .s files in `disasm` of current() shaders, only those in selected.json when it exists: a disassembly
    from before selected.json has none, and there every .s file of a current shader counts."""
    if not os.path.isdir(disasm):
        raise ValueError(f"{disasm} is not a folder")
    names = current(os.path.dirname(os.path.abspath(disasm)))
    sel = os.path.join(disasm, SELECTED)
    if os.path.isfile(sel):
        made = listed(sel, "name")
        files = [os.path.join(disasm, n + ".s") for n in names if n in made]
        for path in files:
            if not os.path.isfile(path):
                raise ValueError(f"missing disassembly listed by {sel}: {path}")
    else:
        # Older successful disassemblies recorded only disasm.done: no listing can be expected for a no-code FSSM.
        files = [os.path.join(disasm, n + ".s") for n in names]
        files = [path for path in files if os.path.isfile(path)]
    if not files:
        raise ValueError(f"no .s file of a current shader in {disasm}")
    return files


def write(path: str, names) -> None:
    """`names` as a JSON list of {"name": ...} records, renamed into place so a stopped run leaves none."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump([{"name": n} for n in names], f)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
