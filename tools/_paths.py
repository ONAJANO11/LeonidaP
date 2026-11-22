"""Shared path checks for the tools that refuse to write a report over one of their inputs (BUILD-589).

    from _paths import same_report_path

Standard library only. A tool run as `python -I tools/<tool>.py` finds this module next to it.
"""
from __future__ import annotations

import os
import sys
import unicodedata

_PC_CASE_SENSITIVE = 11  # <sys/unistd.h> on macOS; os.pathconf_names doesn't list it


def case_insensitive_volume(path: str) -> bool:
    """True when the volume that will hold `path` ignores letter case, as macOS's default APFS volume does.
    os.path.normcase folds case only on Windows, so on macOS `R.json` and `r.json` would pass as two reports."""
    if sys.platform != "darwin":
        return False
    while not os.path.exists(path):
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    try:
        return os.pathconf(path, _PC_CASE_SENSITIVE) == 0
    except (OSError, ValueError):
        return True  # the macOS default; folding can only refuse a pair, never overwrite one


def path_key(path: str) -> str:
    """A comparison key: the real path, case-folded where the volume ignores case."""
    path = os.path.normcase(os.path.realpath(path))
    if case_insensitive_volume(path):
        # APFS and HFS+ also treat composed and decomposed accents as one name.
        path = unicodedata.normalize("NFD", path.casefold())
    return path


def same_report_path(first: str, second: str) -> bool:
    """True when the two paths name one file: same real path (links and `..` resolved, case folded where the volume
    ignores it), or, for files that exist, the same file through a hard link."""
    if path_key(first) == path_key(second):
        return True
    try:
        return os.path.samefile(first, second)
    except OSError:
        return False
