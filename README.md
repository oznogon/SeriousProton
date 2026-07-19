# SeriousProton fork

This repo is a fork of the [SeriousProton](https://github.com/daid/SeriousProton) C++ game engine coded on from scratch by [daid](https://github.com/daid). This hard fork is _not compatible_ with upstream SeriousProton, and is tested only with the oz-fork branch of https://github.com/oznogon/EmptyEpsilon.

## Major breaking changes in this fork

- SDL migrated from SDL2 to SDL3
- Control bind interaction types (discrete, repeat, hold, toggle) to define behaviors of non-standard control surfaces
- Custom arbitrary clipping regions (ClipRegion replacing ScissorRect) and render translation, to facilitate scrollable GUI elements
- Font line_height parameterization
- drawLine thickness params, with optional quad-based line drawing, quality improvements to circle drawing, and `drawStretchedHV()`/`drawStretchedHVClipped()` rotation support

## Other changes

- A proxy registry (`proxyregistry/`) service that can automatically spin up reverse proxy servers to bridge peers behind NAT walls, and engine integration to discover proxy servers
- Optional Prometheus metrics endpoint with network stats, entity count, and engine timings
- Screenshot functionality using stb_image_write for PNG output, with multimonitor support
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
