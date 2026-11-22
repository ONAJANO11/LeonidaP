# Source inventory history

Each panel has its own units and denominator. No aggregate project-completion percentage is computed.
Historical points rescan the last available first-parent source commit in each UTC day using the same rules.
Live points record the last observed source revision that UTC day. Missing days are not measurements.
Source definitions/selectors do not prove runtime correctness, shader correctness, or recovery success.

Rules: `11deb513ea3f1ada`. [D3D12-only reference](https://raw.githubusercontent.com/microsoft/DirectX-Headers/61c7718954ec73dbdd43c723240fe1a0452a3724/include/directx/d3d12.h).

## D3D12 SDK methods

Units: methods.

| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-11-21 | 60 | 8 | 11 | 128 | 207 | [4abd5457](https://github.com/ItsNotRexed/Leonida/commit/4abd54571b6efa02d97eda4711792fa00b455dc3) | rescanned |
| 2026-11-22 | 74 | 14 | 12 | 107 | 207 | [a161e7b3](https://github.com/ItsNotRexed/Leonida/commit/a161e7b3534f500bc9356d94e590b7dff9d0e71c) | observed |

## Shader translator · declared selectors

Units: selectors.

| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-11-21 | 10 | 343 | 20 | 0 | 373 | [4abd5457](https://github.com/ItsNotRexed/Leonida/commit/4abd54571b6efa02d97eda4711792fa00b455dc3) | rescanned |
| 2026-11-22 | 10 | 372 | 19 | 0 | 401 | [a161e7b3](https://github.com/ItsNotRexed/Leonida/commit/a161e7b3534f500bc9356d94e590b7dff9d0e71c) | observed |

## Shader pipeline · Python definitions

Units: definitions.

| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-11-21 | 171 | 61 | 0 | 0 | 232 | [4abd5457](https://github.com/ItsNotRexed/Leonida/commit/4abd54571b6efa02d97eda4711792fa00b455dc3) | rescanned |
| 2026-11-22 | 186 | 65 | 0 | 0 | 251 | [a161e7b3](https://github.com/ItsNotRexed/Leonida/commit/a161e7b3534f500bc9356d94e590b7dff9d0e71c) | observed |

## Runtime · C++ definitions

Units: definitions.

| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-11-21 | 301 | 5 | 4 | 8 | 318 | [4abd5457](https://github.com/ItsNotRexed/Leonida/commit/4abd54571b6efa02d97eda4711792fa00b455dc3) | rescanned |
| 2026-11-22 | 338 | 5 | 3 | 10 | 356 | [a161e7b3](https://github.com/ItsNotRexed/Leonida/commit/a161e7b3534f500bc9356d94e590b7dff9d0e71c) | observed |

## Asset tools · Python and C# definitions

Units: definitions.

| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-11-21 | 554 | 4 | 0 | 0 | 558 | [4abd5457](https://github.com/ItsNotRexed/Leonida/commit/4abd54571b6efa02d97eda4711792fa00b455dc3) | rescanned |
| 2026-11-22 | 655 | 4 | 0 | 0 | 659 | [a161e7b3](https://github.com/ItsNotRexed/Leonida/commit/a161e7b3534f500bc9356d94e590b7dff9d0e71c) | observed |

## Asset exporter · command branches

Units: commands.

| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-11-21 | 5 | 0 | 0 | 0 | 5 | [4abd5457](https://github.com/ItsNotRexed/Leonida/commit/4abd54571b6efa02d97eda4711792fa00b455dc3) | rescanned |
| 2026-11-22 | 5 | 0 | 0 | 0 | 5 | [a161e7b3](https://github.com/ItsNotRexed/Leonida/commit/a161e7b3534f500bc9356d94e590b7dff9d0e71c) | observed |

## Other Python tools

Units: definitions.

| UTC day | Bodies | Guarded | No-op | Stubs | Total | Source | Measurement |
|---|---:|---:|---:|---:|---:|---|---|
| 2026-11-21 | 188 | 9 | 0 | 0 | 197 | [4abd5457](https://github.com/ItsNotRexed/Leonida/commit/4abd54571b6efa02d97eda4711792fa00b455dc3) | rescanned |
| 2026-11-22 | 446 | 26 | 0 | 3 | 475 | [a161e7b3](https://github.com/ItsNotRexed/Leonida/commit/a161e7b3534f500bc9356d94e590b7dff9d0e71c) | observed |

## Unmeasured historical trees

These are not treated as zero.

- 2026-11-20 (bd2dd1c8): Generated D3D12 interface list not found in runtime/CMakeLists.txt
