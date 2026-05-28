# ebsrc

An open-source reimplementation of **EarthBound** (US) / **Mother 2** (Japan) for the Super Nintendo — written in C, playable on modern platforms.

Play EarthBound natively on your PC, a Raspberry Pi Pico, or build a byte-perfect SNES ROM from the fully disassembled 65816 assembly. All game data is extracted from your own legally-obtained ROM — no copyrighted content is included in this repository.

## Platforms

| Port | Directory | Status | Notes |
|------|-----------|--------|-------|
| **Desktop** (Windows, macOS, Linux) | `port/unix/` | Fully playable | SDL2, 60 FPS, audio, fast-forward |
| **RP2040** (Pico LCD 1.3) | `port/waveshare/pico-lcd-1.3/` | Playable (no audio) | 240x240 ST7789 display, 9 inputs |
| **SNES ROM** (assembly) | `asm/` | Complete | Reassembles a byte-perfect ROM |
| **SNES ROM** (C back-port) | `port/snes/` | Scaffolding only | Goal: compile C back to 65816 |

The C port is structured as a platform-agnostic game library (`src/`) that any port links against. Adding a new platform means implementing a thin `platform.h` interface — see [docs/porting-guide.md](docs/porting-guide.md).

---

## Quick Start: Desktop (Recommended)

### 1. Install Prerequisites

**macOS** (using [Homebrew](https://brew.sh)):
```bash
brew install cmake sdl2 pkg-config git
```

**Ubuntu / Debian Linux**:
```bash
sudo apt update
sudo apt install cmake libsdl2-dev build-essential pkg-config git
```

**Fedora**:
```bash
sudo dnf install cmake SDL2-devel gcc pkg-config git
```

You also need **Python 3.10+** (pre-installed on most systems — check with `python3 --version`).

### 2. Clone, Add Your ROM, and Build

```bash
git clone https://github.com/Herringfield/earthbound.git
cd earthbound
cp path/to/your/earthbound.sfc earthbound.sfc
make unix
```

That's it! `make unix` automatically sets up the Python environment, extracts game assets from your ROM, and compiles the game. When it finishes, run:

```bash
./port/unix/build/earthbound
```

If any dependencies are missing, `make unix` will tell you exactly what to install.

### Manual Build (Advanced)

If you prefer to run each step yourself:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
ebtools extract
cd port/unix
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/earthbound
```

---

## Controls

EarthBound was designed for comfortable one-handed play — many buttons are intentionally redundant.

| Button | Function |
|--------|----------|
| **D-Pad** | Move character; navigate menus |
| **A** | Confirm / interact / open command window |
| **B** | Cancel / back; show HP/PP window on overworld |
| **X** | Toggle town map (when obtained) |
| **L** | "Check" / talk (like A, but prioritizes dialogue). Also confirms in menus |
| **Select** | Same as B (one-handed play) |
| **Start** | Start game from title screen |
| **R** | Ring bicycle bell (cosmetic) |

**Minimum for hardware ports:** D-Pad + A + B is fully playable. Add X for the town map and Start for the title screen.

### Default Keyboard Mapping

| Key | SNES Button |
|-----|-------------|
| Arrow keys | D-Pad |
| X | A |
| Z | B |
| S | X |
| A | Y |
| Q | L |
| W | R |
| Enter | Start |
| Right Shift | Select |
| Tab | Fast-forward (4x speed) |

---

## Debug Hotkeys

| Key | Action |
|-----|--------|
| F1  | Screenshot + VRAM/tile dump to `debug/` (BMP) |
| F2  | VRAM tile visualization dump (4bpp + 2bpp BMPs) |
| F3  | Toggle FPS / profiling overlay |
| F4  | Dump full game state to `debug/state_NNN.bin` (~190KB, diff with `cmp`) |
| F5  | Toggle in-game debug mode (`ow.debug_flag`) — enables debug menus |

### FPS Overlay (F3)

A scanline-stamped overlay drawn in the in-game TINY font in the top-right corner. Because it writes directly into the rendered scanline buffer, it works identically on every port regardless of BG/tilemap state and uses integer-only math (shift-based IIR smoothing — no floating point).

**Default rows** — `XX.X` values are in milliseconds; the FPS row is frames/second:

| Row | Color  | Meaning |
|-----|--------|---------|
| FPS | Green  | Frames per second over the recent window (target 60.0) |
| LOG | Yellow | Game logic time per frame — fade, palette sync, script VM, NMI-equivalent work |
| PPU | Orange | Software PPU render time per frame (BG/OBJ/window/composite) |
| IDL | Gray   | Idle headroom remaining in the frame budget (`16.7ms − LOG − PPU`) |

`LOG + PPU + IDL ≈ 16.7ms` at 60 FPS. If `IDL` trends to 0 the port is CPU-bound for that frame; if `FPS` drops below 60 you're either CPU-bound or vsync is stalling.

**Optional rows:**

- **SKP** (red when active, gray when idle) — appears only when the port is built with `MAX_FRAME_SKIP > 0` (e.g. the Pico port, `MAX_FRAME_SKIP=2`). Shows the length of the most recent run of skipped render frames. Dynamic frame-skipping drops PPU rendering while keeping game logic at real-time when the port falls behind schedule.
- **CLR / BG / OBJ / WIN / CMP / SND** (cyan) — appear only in builds compiled with `-DPPU_PROFILE`. Per-section render time in tenths of a millisecond (so `BG 310` = 31.0 ms): clear, backgrounds, sprites, window/HDMA, BG/OBJ compositing, and the final scanline send to the platform.

---

## Building the Assembly ROM

If you want a reassembled SNES ROM for use with an emulator or flash cart, you'll need additional tools.

### Additional Prerequisites

- [ca65 v2.19+](https://github.com/cc65/cc65) — 65816 assembler (part of the cc65 suite)
- [spcasm v1.1.0+](https://github.com/kleinesfilmroellchen/spcasm/) — SPC700 audio assembler
- GNU make

### Build

Make sure you've cloned the repo, installed ebtools, and extracted assets (steps 2–3 above), then from the repository root:

**EarthBound (US Retail)**:
```bash
make
```

**Mother 2 (Japan)** — requires a Mother 2 ROM:
```bash
uv run ebtools extract mother2.yml "path/to/mother2.sfc"
make mother2
```

**US Localization Prototype (1995-03-27)** — requires the prototype ROM:
```bash
uv run ebtools extract earthbound-1995-03-27.yml "path/to/prototype.sfc"
make proto19950327
```

Output goes to `build/` (e.g. `build/earthbound.sfc`).

---

## Modding & Asset Editing

Game data lives in human-editable JSON files under `src/assets/` — items, enemies, NPCs, PSI, and more. Edit the JSON, rebuild, and your changes are packed into the game automatically.

Overworld sprites are extracted as indexed PNGs with JSON metadata. Custom sprites go in `src/custom_assets/overworld_sprites/png/` and override originals at build time. See [docs/editing-sprites.md](docs/editing-sprites.md) for the full guide.

---

## Project Structure

```
src/                    Game library (platform-agnostic C)
  core/                   Math, memory, decompression
  entity/                 Entity system, scripts, sprites
  game/                   Battle, text, overworld, inventory, audio
  intro/                  Title screen, file select, naming
  snes/                   Software PPU renderer, DMA, SPC700 emulator
  platform/platform.h     Interface that ports implement

port/
  unix/                   Desktop port (SDL2) — Windows, macOS, Linux
  waveshare/pico-lcd-1.3/ RP2040 embedded port
  snes/                   SNES native port (scaffolding)

asm/                    Complete 65816 disassembly, organized by subsystem
  battle/  overworld/  text/  system/  audio/  ...

docs/                   Guides: porting, assembly-to-C, sprites, assets
```

---

## Documentation

- [Porting Guide](docs/porting-guide.md) — how to add a new platform port
- [Assembly-to-C Guide](docs/assembly-to-c.md) — porting conventions, VUCC calling convention, worked examples
- [Editing Overworld Sprites](docs/editing-sprites.md) — viewing, editing, and repacking sprites
- [Asset Documentation](docs/assets.md) — game asset formats

---

## Troubleshooting

**"ebtools: command not found"** — Use `uv run ebtools` instead of bare `ebtools`, or activate the venv first: `source .venv/bin/activate` (macOS/Linux) or `.venv\Scripts\activate` (Windows).

**"SDL2 not found" during cmake** — Install SDL2 dev libraries. macOS: `brew install sdl2`. Linux: `sudo apt install libsdl2-dev`.

**"ca65: command not found"** — Install the cc65 suite. macOS: `brew install cc65`. Linux: [build from source](https://github.com/cc65/cc65).

**Missing files in `asm/bin/`** — Run `ebtools extract` first. Assets must be extracted from your ROM before building.

---

## Contributing

Contributions are welcome! Current focus areas:

- Bug fixes and visual glitches in the C port
- Performance optimization (especially for embedded targets)
- New platform ports
- Better asset editing tools and formats
