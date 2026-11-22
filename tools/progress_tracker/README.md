# Repository-owned source tracker

This directory owns the scanner, renderer, pinned parser dependencies, counting rules and synthetic tests. Generated files live in [`docs/progress`](../../docs/progress). The README embeds those local files. No account, token, image, service, or repository belonging to the original contributor is required.

## Measurements

| Panel | Unit and denominator | Scope |
|---|---|---|
| D3D12 | SDK methods, including inherited methods except `IUnknown` | Interfaces requested by the CMake stub generator, compared with concrete classes and their shared bases |
| Shader selectors | Opcode names and prefix families explicitly selected by dispatcher code | `Gen._insn` and `Gen` handlers with an `op` parameter in `rdna2spirv.py`; families can overlap |
| Shader pipeline | Source function/method definitions | Translator, shader IR, metadata, disassembly, carving, FSSM and control-flow helpers |
| Runtime | C++ source definitions | Production `runtime/src` and `runtime/shims`, excluding the three D3D12 object files already represented in the method panel |
| Asset tools | Python and C# source definitions | Asset-format Python modules and the in-repository `rpf9dump` implementation |
| Exporter commands | Explicit top-level `mode` branches | `rpf9dump` command dispatcher; nested mode refinements do not count again |
| Other tools | Python source definitions | Remaining top-level Python tools and `make_game.py`, including diagnostics, references and fuzz helpers |

These are different source inventories. **They are never added into an overall completion percentage.** Only the D3D12 panel has an external API reference denominator. Function, command and selector totals describe known definitions, not every missing feature, hardware instruction, import, asset format, or test case. A tool's presence does not prove it works on the game. Prefix selectors can overlap explicit opcodes and one another.

The asset module prefixes and shader helper names are explicit in `subsystems.py`. New matching modules and new definitions/selectors in measured sources are discovered automatically; newly introduced subsystem layouts can require updating the adapter. Test fixtures, generated code, the tracker itself, `bin`/`obj`, and third-party sources/submodules are excluded. Macro-generated C++ members are not expanded or counted as independently written functions. Compiler-defaulted functions are recorded explicitly.

## Classification

- **Green — body present:** a nontrivial source definition or selector branch with no recognized unsupported marker. This is not verified correctness.
- **Amber — guarded / unsupported:** recognized unsupported markers in the body or a local helper, or a conditionally defined Python function. Guarded shader operand refusal can be intentional. Amber is not a bug count.
- **Blue — no-op / constant:** an empty or simple constant-return body. Some no-ops are intentional.
- **Grey — stub / missing:** an inherited D3D12 fallback, explicit placeholder, deleted declaration, or unsupported-only body.

Python is parsed with `ast` and never imported. C++ and C# use the versions pinned in `requirements.txt`. Comments and string literals are not executed or mistaken for function bodies. Recognized Python markers include `Unsupported`, `NotImplementedError`, `NotSupportedError`, and explicit stub helper calls. C++ recognizes the D3D12 stub helpers and `E_NOTIMPL`. C# recognizes explicit `NotImplementedException`/`NotSupportedException` throws. Helper propagation is limited to recognized local scopes and is conservative. There is no whole-program control-flow proof, arbitrary cross-file call resolution, preprocessor execution, virtual-dispatch analysis, or runtime testing.

The C++ adapter masks calling conventions, `__declspec`, known registration/function-list macro invocations, and a known grammar ambiguity in empty default parameters. The masks retain byte positions; source links still refer to the original file and line. It does not compile the masked text. New unsupported syntax or ambiguous D3D12 ownership fails the scan rather than silently declaring support. Direct shader dispatch tables and literal selectors are inspected without evaluating arbitrary Python expressions.

## Reproduce

From the repository root, with Python 3.12+ and Git:

```sh
python -m pip install -r tools/progress_tracker/requirements.txt
python -I -W error -m unittest discover -s tools/progress_tracker/tests -v
python -I tools/progress_tracker/run.py
```

The script reads one **committed** revision from the current Git checkout, defaulting to `HEAD`. Uncommitted runtime/tool changes are not a measurement; commit them or pass `--revision` to select another existing commit. `--output` chooses an alternative output directory for previews. No measured code, shader, asset, game binary, submodule, or build script is run. No game copy is required.

The D3D12 reference is an official Microsoft DirectX header pinned by commit and SHA-256 in `tracker.json`. It is cached in ignored `build/progress-tracker`; a fresh run downloads that public header over HTTPS and verifies its hash. The header itself is not committed. Updating the reference requires reviewing its revision/checksum and running the tests. No proprietary SDK installation is required for the tracker.

## History

One latest point is retained per UTC day for each measured panel. Each history subplot has its own units and scale. Initial history rescans the last first-parent commit on available UTC days over the prior 14 days using the same rules. Unscannable historical trees are explicitly unmeasured, not zero. Changes to scanner code, parser versions or the SDK reference start a separate series; prior series remain in `history.json` and are not joined to the new chart.

Changing the definition count can change the denominator. Removal is not completion, a body can regress to a stub, and adding a body is not a passing behavioral test. The source links and history point metadata identify the exact input revision.

## Automation and ownership

[`.github/workflows/progress.yml`](../../.github/workflows/progress.yml) runs on relevant pushes and PRs, hourly at minute 23 UTC, and manually. Its check job runs tests and generates a temporary preview with read-only repository permissions. The publish job runs only on non-PR `main` events in the non-fork repository, uses that repository's built-in `GITHUB_TOKEN`, and commits only `docs/progress`. It does not require a personal token or permission to create PRs. Fork PRs cannot publish snapshots.

The workflow and local README paths become active in upstream after this PR is merged. Normal code changes are picked up automatically. GitHub can delay schedules, disable inactive schedules, restrict Actions, or block direct pushes with branch rules. If `main` requires every change to go through a PR, maintainers must choose a compatible publication policy before enabling the publish job; this workflow does not bypass those rules. Failed checks or source scans do not publish fresh output. The old timestamp remains visible and Actions reports the failure. Generated snapshots do not match the workflow's source path filters, so they do not trigger a refresh loop.

Maintainers can change counting rules, schedules, reference pins, output placement, or publication policy in this repository. Components adapted from the original tracker retain the [MIT notice](LICENSE); this is compatible with the repository's license.
