# SeriousProton fork

This repo is a fork of the [SeriousProton](https://github.com/daid/SeriousProton) C++ game engine coded on from scratch by [daid](https://github.com/daid). This hard fork is _not compatible_ with upstream SeriousProton, and is tested only with the oz-fork branch of https://github.com/oznogon/EmptyEpsilon.

## Build dependencies

### Toolchain

SeriousProton is a C++17 project built with CMake.

### System libraries (must be installed on the host)

| Dependency | How obtained | Notes |
|---|---|---|
| **OpenSSL** | System (`find_package(OpenSSL REQUIRED)`) | TLS networking, primarily to support integrations that require SSL. Both `libssl` and `libcrypto` required. Set `-DWITH_SSL=OFF` to disable. |
| **Threads** | System (`find_package(Threads REQUIRED)`) | Multithreading |

### Vendored libraries (fetched or bundled automatically via CMake)

| Dependency | How obtained | Notes |
|---|---|---|
| **SDL3** | FetchContent (`release-3.4.12`) | Windowing, input, audio |
| **GLM** | System or bundled (`1.0.1`) | OpenGL Mathematics; set `WITH_GLM=system|bundled|auto` |
| **Box2D** | Vendored (`libs/box2d`) | 2D physics engine |
| **glad** | Vendored (`libs/glad`) | OpenGL loader |
| **Lua** | Vendored (`libs/lua`) | Scripting engine |
| **libopus** | System or bundled | Audio codec; set `WITH_OPUS=system|bundled` |
| **FreeType 2** | System or bundled | Font rendering; set `WITH_FREETYPE2=system|bundled|auto` |
| **Basis Universal** | Vendored (`libs/basis_universal`) | Supercompressed GPU texture codec (KTX2) |
| **nlohmann/json** | Vendored header-only (`libs/nlohmann`) | JSON parsing |
| **stb** | Vendored header-only (`libs/stb`) | `stb_image` / `stb_image_write` for screenshot PNG output |
| **dtoa** | Vendored (`libs/dtoa`) | Double-to-ASCII conversion |

### Optional

| Dependency | Option | Notes |
|---|---|---|
| **Steam SDK** | `-DSTEAMSDK=/path/to/sdk` | Steamworks P2P networking; not required if Steam features are not needed |
| **Prometheus** | `-DPROMETHEUS_ENABLE_METRICS=ON` | Exposes a metrics endpoint; requires `prometheus-cpp` |

## Major breaking changes in this fork

- SDL migrated from SDL2 to SDL3
- SDL3 is now vendored via CMake FetchContent
- Control bind interaction types (discrete, repeat, hold, toggle) define behaviors of non-standard control surfaces
- Custom arbitrary clipping regions (`ClipRegion` replacing `ScissorRect`) and render translation facilitate scrollable GUI elements
- Font `line_height` parameterization
- drawLine thickness params, with optional quad-based line drawing, quality improvements to circle drawing, and `drawStretchedHV()`/`drawStretchedHVClipped()` rotation support

## Other changes

- A proxy registry (`proxyregistry/`) service that can automatically spin up reverse proxy servers to bridge peers behind NAT walls, and engine integration to discover proxy servers
- Optional Prometheus metrics endpoint with network stats, entity count, and engine timings
- Screenshot functionality using `stb_image_write` for PNG output, with multimonitor support
- Optional client latency simulation on server for lag/interpolation debugging
- OGG tag retrieval to identify background music details
- Minor bug fixes
  - Master server registration thread deadlock on shutdown
  - Explicit `game_server.isAlive()` Boolean check to avoid deref crashes on exit via Bool comparisons
  - Music looping fixed
- `std::u8string` C++17 compatibility
- MSVC strict compilation fixes and cross-platform warning suppression
- Codebase-wide formatting cleanup
  - Consistent use of spaces for tabs
  - Consistent Unix line endings
  - Excess whitespace removal
  - `#pragma once` header guards instead of `#ifdef`
  - Audited standard library includes and forward declarations
