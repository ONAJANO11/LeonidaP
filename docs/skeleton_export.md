# Optional skeleton metadata export

This integration depends on **unmerged rpf9-explorer PR #3**, commit
`374f9a74df49ea22d8ac6e095196f56a03994de3`. The submodule pin is explicit;
do not merge this dependency change without maintainer review.

## Upstream integration and dependency boundary

ASSET-10 was claimed and reported in Leonida issue #156; current coordination is
continued in issue #374. PR #96's merged animation/rig metadata index remains
separate from this dependency API. No skeleton parser is duplicated here.

This branch merges upstream main at `d937b7f092dbc188b59754e5c73c1a9dac553cf9`,
including merged PR #162. Its existing `Util.WriteAllText` / `WriteAtomic` helper
now writes the sidecar through `<path>.partial`, then renames the complete file.
The upstream OBJ, DDS and audio writers and atomic-write diagnostics are retained
unchanged. There is no second atomic implementation. Serialization completes
before writing; failed writes remove their temporary output. Existing final files
survive a failed replacement. This is per-file atomicity, not a transaction across
OBJ, metadata and textures, and it is not a power-loss durability guarantee.

`SkeletonExport` is the small exporter boundary around the dependency API. Both
compilation and runtime still require the pinned unmerged skeleton PR. Synthetic
tests verify metadata and writes, but do not make the feature independent of that
parser. A supported merged revision, or an explicit maintainer decision to accept
this pin, is needed before describing dependency integration as resolved.

Normal OBJ export is unchanged. For local research, a caller who has independently
identified the correct external drawable may opt into JSON sidecars:

```powershell
rpf9dump models <archive.rpf> <output> --skeleton-system <system-page-file> --skeleton-drawable-offset <decimal-offset>
```

The skeleton file must be the decoded **system page**, not an RPF archive or a
combined system/graphics file. The offset is the drawable's byte offset in that
page. The tool reuses the dependency's validated parser. Selection is explicit;
hash resolution alone is not evidence that this is the original character's rig.
This mode is intended for a matching model collection, not a heterogeneous archive.
Meshes without palettes or with missing/ambiguous bindings fail rather than receive
guessed assignments. In this mode a failed resource makes the command exit nonzero.

The supported layout is the pinned dependency's external-drawable wrapper lookup
and a valid model hash-palette table. A directly referenced skeleton header that
the convenience lookup does not recognize, or a model for which the palette API
returns no table, remains unsupported and is rejected. Equal joint counts do not
justify assuming that vertex indices address the skeleton directly. This exporter
does not add a second parser or an implicit identity-mapping fallback.

Each successful model receives `<stem>.skeleton.json` with joint hashes, nullable
parents, inverse bind matrices, and per-mesh palette hashes and joint indices in
original slot order. Matrices are flattened row-major System.Numerics row-vector
matrices; translation is at indices 12–14. Coordinates are unchanged. Sidecars
explicitly report `HasVertexSkinning=false` and `HasAnimation=false`.

This is **metadata preservation**, not glTF, weighted skinning, facial animation,
automatic skeleton discovery, or native clip decoding. No vertex weights are
invented. Sidecars and all other extracted content stay local under CONTRIBUTING.md.
Existing OBJ files are not rewritten. Failed resources do not remove outputs from
earlier runs; check exit status and use a fresh output directory for validation.

Synthetic CPU checks:

```powershell
dotnet build tools/rpf9dump/rpf9dump.csproj -c Release
dotnet run --project tests/Rpf9DumpSkeletonExport -c Release
dotnet run --project tests/Rpf9DumpDiagnostics -c Release
dotnet run --project third_party/rpf9-explorer/Tests/SkeletonSmoke -c Release
```

The synthetic tests check serialized hierarchy, matrix elements against an
independent bind-space coordinate calculation, repeated/reordered palettes,
missing and ambiguous bindings, malformed skeletons, explicit file/offset loading,
invalid offsets, whole-model rejection, CLI opt-out, deterministic/culture-independent
JSON, and OBJ non-mutation. The production sidecar-writing path is checked for
stale temporary files, replacement, serialization failure and rename failure;
fault injection also verifies that the shared writer preserves a previous final
sidecar after an interrupted replacement. They do not demonstrate a deformed
real character.
