# Memory Limit Expansion

Lets *Vampire: The Masquerade - Bloodlines* use more memory than its 32-bit address space holds.

## The problem

Bloodlines is a 32-bit program. Even with the Unofficial Patch's large-address-aware `Vampire.exe`,
every pointer the game uses must fall within 4 GB of virtual address space, and everything the game
touches has to fit there: code, the engine's 200 MB hunk, the heaps, sound, the Direct3D runtime and
driver, and a second copy of every texture and mesh. High-resolution texture packs and long sessions
fill that space, and the game crashes when an allocation fails.

A 32-bit pointer cannot address more than 4 GB directly, so Memory Limit Expansion works around the
limit in two ways: keeping GPU resources **outside** the game's address space (parts 1 and 2), and
optionally running the game as **two processes**, each with its own 4 GB (part 3).

## What it does

### 1. Textures and meshes leave the address space (Direct3D 9Ex)

The renderer (`shaderapidx9.dll`) creates every normal texture and every static vertex and index
buffer in Direct3D's *managed pool*. In that pool the Direct3D runtime keeps a system-memory copy of
each resource **inside the game's 4 GB address space**, so it can restore video memory after a device
loss.

Memory Limit Expansion creates the device as **Direct3D 9Ex** instead, and places those resources in
video memory only (`D3DPOOL_DEFAULT`). A 9Ex device does not lose its video memory, so the in-process
system-memory copy is no longer needed. When video memory fills up, Windows (WDDM) pages the data to
system RAM outside the game's 4 GB address space. A machine with 16 GB of RAM and a 4 GB graphics
card can hold several GB of textures without the game's address space seeing any of it.

Key details:

- **Emulated texture locks:** The game writes into a small reusable upload surface, and unlock copies
  the updated region into video memory (`UpdateSurface`). Tiny DXT mip levels that drivers refuse as
  standalone textures are uploaded through a lower level of a larger upload surface.
- **Frequently rewritten textures:** Dynamic textures the engine updates repeatedly (such as particle
  sheets and dynamic lightmaps, for renderable formats where GPU readback is supported) automatically
  get a small system-memory backing after 32 writes, flushing dirty rectangles to video memory once
  when bound, drawn with, or at `Present` (capped at 1 MB per texture and 64 MB total).
- **Alt+Tab and mode changes:** A 9Ex device reports `S_PRESENT_MODE_CHANGED` instead of a lost
  device; Memory Limit Expansion translates this so the game's own reset path runs cleanly while
  offloaded resources survive `Reset`.

### 2. Bigger engine hunk (optional)

The engine keeps the current map, and in whatever space remains its sound and model cache, in one
fixed pool (the "hunk"), sized to half of RAM but capped at 200 MB (`engine.dll` 1.2,
`Sys_InitMemory`). A map that exceeds the cap ends the game with `Hunk_Alloc: failed`.

Setting `[Engine] hunk_mb` (200–1024 MB) raises that cap by byte-checking and patching the cap
immediates in `engine.dll` as it loads (and again if the engine restarts). Off by default (`0`),
since stock maps use well under 200 MB. `-heapsize` on the command line still takes precedence.

### 3. Client/Server process split (experimental, on by default: `[Split] enabled = 1`)

Bloodlines is built on the Source engine, which separates the game into two halves communicating over
a network channel: the **server** (`vampire.dll`: characters, AI, physics, Python scripts, saves) and
the **client** (`client.dll`: rendering, sound, interface). In single player both run in one process.

With `[Split] enabled = 1`, the game launches a second, windowless `Vampire.exe` as a local server
and connects to it over loopback (`127.0.0.1` UDP). Each process gets its own 4 GB address space (up
to 8 GB combined), while Direct3D 9Ex keeps textures and meshes outside both:

- **Client side (`src/split_client.cpp`):** Launches and supervises the local server in a Windows job
  object (so it exits with the game), forwards `map`, `load`, `save`, `changelevel`, `reload`,
  `restart`, and console Python expressions, synchronizes aliases (`cfg/user.cfg`), gameplay convars,
  captions, and pause commands, shows the loading screen on New Game and Load, keeps 64 snapshots for
  delta updates, and releases static props cleanly across level changes. Falls back to normal single
  player if the server process cannot be started.
- **Server side (`src/split_server.cpp`, `src/shaderapiempty/`):** Runs `engine.dll`'s dedicated
  server interface with a headless `shaderapiempty.dll` (no D3D device or textures, while keeping real
  hardware config flags so materials match), sends snapshots every world frame at `[Split] server_fps`
  (default 100) using a high-resolution timer, preserves single-player console command routing and
  pause behavior, and integrates with BLSE's split-process link when BLSE 0.7.1+ is installed.

Known limitations of the experimental split mode:

- Movement goes through the local server without client-side prediction (movement is shown
  about 15 to 20 ms late, up to 40 ms at 30 fps; mouse look remains immediate).
- BLSE plugins that need both halves of the game must support BLSE's split link; other loader plugins
  still load in both processes.
- Client state (`.HL2` map decals and client entities) is written through a save handshake with the
  game process, though transient client-only visual effects are not saved.
- A small number of client-only cvars set directly by `vampire.dll` (such as `r_cloth` and
  `r_shadows`) do not propagate across processes.

### 4. Diagnostics

`memory_expansion.log` (written next to `memory_expansion.ini`) records periodic address-space
statistics (`[Diagnostics] stats_seconds`, default 60). Setting `trace_allocs_mb` logs every large
allocation above that threshold along with its call site, and `console_log = 1` mirrors the game's
console output to `game_console.log` (the split server process always writes `server_console.log`
and `memory_expansion_server.log`).

Each log is started fresh when its process starts; the previous run's log is kept as
`<name>.prev.log` (for example `memory_expansion_server.prev.log`), so a server process that was
ended and replaced still leaves its last lines behind. The server log notes how long every save
took and, when the world stops, where its main thread was every 5 seconds. If the game has to end
a server process that stopped responding, it first writes `memory_expansion_server_hang.dmp` (the
server's threads at that moment) next to the logs. Attach the logs, their `.prev.log` copies and
the `.dmp` file to a bug report.

## DXVK compatibility

- **You do not need DXVK for memory savings:** On Windows, Memory Limit Expansion already offloads
  textures and vertex/index buffers out of the 4 GB address space natively via Direct3D 9Ex, and adds
  optional engine hunk expansion and the experimental 8 GB process split.
- **Compatible if you still want DXVK:** If you use DXVK for Vulkan rendering (for example on
  AMD/Intel GPUs or Linux/Steam Deck), they work together out of the box. With the default
  `allow_d3d9_wrapper = 0`, Memory Limit Expansion detects DXVK's `d3d9.dll`, lets DXVK handle
  Direct3D resource offloading, and still runs hunk expansion and the process split alongside it.

## Requirements

- *Vampire: The Masquerade - Bloodlines* with the Unofficial Patch's `Vampire.exe` loader.
- Windows with Direct3D 9Ex (Windows Vista or newer) for texture/mesh offloading.
- BLSE is **not** required.

## Installing

Copy the release files into your game folder:

- `Bin/loader/memory_expansion.vtm`
- `Bin/loader/memory_expansion.ini`
- `Bin/shaderapiempty.dll` (used by `[Split] enabled = 1`, on by default)

The Unofficial Patch loader loads `.vtm` plugins from `Bin/loader/` automatically at startup. Edit
`Bin/loader/memory_expansion.ini` to configure options.

## Building

Requires Visual Studio C++ x86 build tools (`cl.exe` and `rc.exe`). Run:

```cmd
build.cmd
```

This builds `build\memory_expansion.vtm` and `build\shaderapiempty.dll`, and stages the ready-to-copy
folder layout in `dist\`.

## License

Licensed under **GPL-3.0-only** with additional terms (attribution and game interoperability
permission). See [LICENSE](LICENSE) and [NOTICE](NOTICE) for full details.
