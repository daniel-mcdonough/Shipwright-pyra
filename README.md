![Ship of Harkinian](docs/shiptitle.darkmode.png#gh-dark-mode-only)
![Ship of Harkinian](docs/shiptitle.lightmode.png#gh-light-mode-only)

# Ship of Harkinian for the DragonBox Pyra

This is an unofficial port of [Ship of Harkinian](https://github.com/HarbourMasters/Shipwright) 9.2.3
"Ackbar" to the [DragonBox Pyra](https://pyra-handheld.com/) handheld (OMAP5432, PowerVR SGX544MP2,
OpenGL ES 2.0, 1280×720 panel). It targets the Pyra only. It was tuned on the device rather than kept
upstreamable, so use [upstream's releases](https://github.com/HarbourMasters/Shipwright/releases) on
any other platform.

**Do not take problems with this port to the Ship of Harkinian Discord or issue tracker.** They do
not support it. Open an issue here instead.

The port spans three repositories:

| Repository | What it holds |
|---|---|
| **Shipwright-pyra** (this one) | The game: the GLES2 port, Pyra defaults, performance work and save safety. Branch `feature/pyra-port`. |
| [libultraship-pyra](https://github.com/daniel-mcdonough/libultraship-pyra) | The engine library, as a submodule: the GLES2 renderer, the GL render thread, config-file safety. Branch `feature/gles2-pyra`. |
| [dragonbox-pyra](https://github.com/daniel-mcdonough/dragonbox-pyra) | The OS side: the `pyra-soh` launcher, the SGX driver fix, input and thermal fixes, kernel patches. |

## What you get

- **A steady 30 fps on a 750 MHz CPU.** OpenGL work moves to a second core, so the busiest scenes
  tested (Market, Death Mountain Trail) miss the 30 fps deadline on only about 1–2% of frames. A
  lower clock means a cooler device and longer battery life.
- **Settings tuned for the Pyra on first launch.** It renders at 640×360 (half the panel), the frame
  rate is capped at 30, VSync is off, MSAA is off (GLES2 has none) and textures use bilinear
  filtering. These are written only when a setting is missing, so your own menu changes stick.
- **Faster rendering with no visual change.** Draw calls, asset lookups, frame interpolation and GL
  state handling were each reworked. Except for draw grouping, which moves a few pixels by one
  colour step, the output is pixel-identical to before.
- **Saves you can trust.** Every save is written to a temporary file, read back and compared, synced
  to the SD card and only then swapped in. `global.sav` and `shipofharkinian.json` used to be written
  straight over the live file. If a save fails, the game says so on screen and the previous save is
  untouched.
- **A clean quit.** Alt+F4, SIGTERM and Ctrl+C now save and exit in a second or two. Before, quitting
  crashed and SIGTERM hung.
- Everything else is Ship of Harkinian 9.2.3: enhancements, randomizer, mods.

## Measured performance on the Pyra

Measured on a Pyra at the shipped 750 MHz clock, against a stock build. The stock build is the same
GLES2 port with none of this fork's performance work. Each test froze the camera at a fixed spot and
alternated launches of the two builds. Every run had a thermal soak and a pinned CPU clock. The full
method and raw data are in [report 12](docs/pyra/12-stock-vs-optimised.md).

| | Market Day (busiest scene) | Hyrule Field |
|---|---|---|
| **Out of the box**: each build at its own default settings, frame cap on | Stock: **13.3 fps**, short of its own 20 fps cap, so the game runs at about ⅔ speed<br>This port: **30 fps**, at its cap | Stock: **18.1 fps**, short of 20<br>This port: **30 fps**, at its cap |
| **Same settings in both builds**, uncapped | 18.3 → **52.3 fps** (2.9× faster)<br>frame time 54.6 → 19.1 ms, **−65%** | 27.2 → **73.9 fps** (2.7× faster)<br>frame time 36.7 → 13.5 ms, **−63%** |
| CPU time per frame, same settings | **−41%** | **−35%** |

A second test flipped every performance switch inside one build, at three scenes and two clocks.
Frame time fell by 64–67% in all six cases (Market, Hyrule Field and Kokiri Forest, at 750 and
1250 MHz). With the texture filter held at upstream's three-point setting in both arms, it still fell
by 43–62%. The filter change accounts for the rest.

How to read these numbers:
- Every effect is many times larger than the noise. Two launches of the same build differed by at
  most 2.9% at the Market and 1.3% at Hyrule Field. The smallest t-statistic in any test was 86.
- The uncapped fps are from a frozen frame. They show how much spare time each frame has, not the
  frame rate you see in play, which the cap holds at 30.

See [Architectural changes](#architectural-changes-and-what-they-bought) for what each change contributes.

## Getting started

There are no prebuilt binaries. The game must link the PowerVR driver that is on your Pyra, and that
driver cannot be redistributed. You need an x86_64 Linux PC with Docker, plus your own legally dumped
ROM ([supported versions](docs/supportedHashes.json)).

1. **Prepare the Pyra.** The port is built and tested on Debian 13 (trixie) with the fixes from
   [dragonbox-pyra](https://github.com/daniel-mcdonough/dragonbox-pyra). Two of them matter most:
   - The SGX driver patch (`pyra-wsegl-fdfix.sh`). The driver closes file descriptors twice, which
     corrupts logs and can make saves fail.
   - The `pyrainput` service. Without it the nubs and buttons are dead.

   The stock bookworm image has not been tested.
2. **Copy the GPU driver to the PC.** Run
   `rsync -a pyra@<your-pyra>:/opt/omap5-sgx-ddk-um-linux/ ~/.cache/pyra-ddk/`.
3. **Cross-build the game** (`git clone --recursive`, then):
   ```sh
   ./scripts/pyra/build.sh image      # build the cross-compile container
   ./scripts/pyra/build.sh configure
   ./scripts/pyra/build.sh build      # -> build-pyra/soh/soh.elf
   ```
4. **Build the asset archives on the PC.** The extractor cannot run in the cross build, so make a
   normal desktop build of this same branch (see [docs/BUILDING.md](docs/BUILDING.md)). Then:
   - Run `cmake --build build-cmake --target GenerateSohOtr` to make `soh.o2r`. It must come from
     this branch, because it carries this port's shaders.
   - Launch that desktop build once with your ROM next to it to make `oot.o2r`.
5. **Install.** Copy `soh.elf`, `soh.o2r` and `oot.o2r` into `~/soh-pyra` on the Pyra, then start the
   game with `pyra-soh` from dragonbox-pyra. The launcher points SDL at the SGX driver, starts the
   game from its own directory (where the game looks for saves and config) and pins the CPU at
   750 MHz. To launch by hand instead, run
   `cd ~/soh-pyra && SDL_VIDEO_X11_FORCE_EGL=1 SDL_VIDEO_GL_DRIVER=/opt/omap5-sgx-ddk-um-linux/lib/libGLESv2.so.1 ./soh.elf`
   from a logged-in desktop session.

Controls are upstream's standard gamepad mapping. The Pyra's nubs and buttons reach the game as an
SDL gamepad through `pyrainput`. Rebind them in the in-game input editor.

**Known limitations:**
- There is no MSAA.
- On GLES2 the depth buffer cannot be read back, so lens flare and light glows are not hidden behind
  walls.
- Textures that mirror in one direction and clamp in the other fall back to mirroring in both.
- Closing the window from outside (for example `xdotool windowclose`) still hangs the game. Use
  Alt+F4.
- Audio runs at the game's native 32 kHz through PulseAudio. 44.1 kHz was measured and was slower.

## Architectural changes and what they bought

Every number below is a change in Ship of Harkinian or libultraship alone. Each was measured by an
A/B benchmark built into the game: with the camera frozen at a fixed spot (Market Day, Hyrule Field
or Kokiri Forest), both versions alternate in one process, seconds apart, at a fixed CPU clock. The
OS, kernel, driver, temperature and clock are therefore the same in both, so OS-side work (driver
fixes, undervolt, thermal tuning) is excluded. "Real play" rows come from the same A/B run during
normal play.

**The whole fork, measured at once** ([report 12](docs/pyra/12-stock-vs-optimised.md)): every switch below
set to its upstream value against the shipped defaults, in one process, frozen camera, uncapped, at
640×360. Frame time per frame, 6 pairs per cell:

| Clock | Market | Hyrule Field | Kokiri Forest |
|---|---|---|---|
| **750 MHz** (shipped) | 59.0 → 21.4 ms, **−64%** (16.9 → 46.8 fps), t −121, MDE 1.4% | 39.9 → 14.2 ms, **−65%** (25.1 → 70.6 fps), t −138, MDE 1.2% | 54.5 → 18.2 ms, **−67%** (18.4 → 55.0 fps), t −115, MDE 1.5% |
| **1250 MHz** | 42.0 → 14.4 ms, **−66%** (23.8 → 69.7 fps), t −105, MDE 1.6% | 29.4 → 10.7 ms, **−64%** (34.0 → 93.7 fps), t −122, MDE 1.3% | 40.0 → 13.4 ms, **−67%** (25.0 → 74.9 fps), t −195, MDE 0.9% |
| 750 MHz, three-point filtering in both | −62% (17.0 → 44.6 fps) | −53% (25.3 → 53.7 fps) | −61% (18.3 → 47.4 fps) |
| 1250 MHz, three-point filtering in both | −58% (24.0 → 56.8 fps) | −43% (34.1 → 59.8 fps) | −57% (25.1 → 57.9 fps) |

The first table's absolute figures come from a benchmark build whose counters cost both arms 8-11%;
its percentages hold (the second table confirms them), and the second table's absolutes have no counters.

Against a separately built stock binary (the GLES2 port and nothing else; [report 12 §12.6](docs/pyra/12-stock-vs-optimised.md)),
at 750 MHz, launches alternated:

| | Market | Hyrule Field |
|---|---|---|
| Same menu settings, uncapped | 18.3 → 52.3 fps, **−65%** frame time, t −330 | 27.2 → 73.9 fps, **−63%**, t −240 |
| Out of the box: stock at upstream settings (1.0×, 20 fps cap, VSync, three-point) against shipped, both capped | 13.3 fps (short of its 20 fps cap) → **30 fps** | 18.1 fps (short of 20) → **30 fps** |

The launch-to-launch noise floor was 2.9% at the Market and 1.3% at Hyrule Field.

In practice: at the shipped 750 MHz clock, stock settings draw the busiest scenes at 17-18 fps, under
the 30 fps cap, so the game would run slow. This port draws them at 47-55 fps and holds the cap with
room to spare. The fps are uncapped figures from a frozen frame, not what you see in play, and the first
table's arms are this build with its switches flipped; the stock binary above agrees with it within two
points (report 12 has the caveats). An earlier three-change chain (33.4 → 23.8 ms at the
Market, 1250 MHz) understated the total by about half.

The per-change rows below each answer a different question, one change against that day's baseline.
They measure different things (frame time, render-thread CPU, window CPU), so **do not add them up**.

| Change | Why | Measured gain |
|---|---|---|
| **GLES2 renderer** in libultraship (`USE_OPENGLES2`) | The SGX544 only speaks OpenGL ES 2.0; upstream's renderer targets desktop GL and GLES 3 | Makes it run at all. Not a speed change |
| **GL render thread** (`RenderThread 2`): renderer calls are recorded and replayed on a GL thread on the second core | The game and GL driver shared one core | Frame time −28% at Market (~34 → 47 fps), −33% at Hyrule Field, at 750 MHz. In real play, frames over 40 ms went from 4.3% to 1.6%. Uses ~10% more total CPU across both cores |
| **Client-side vertex arrays** (`LeanGLState`), replacing a `glBufferData` upload per draw | Buffer uploads are slow on this driver | Frame time −13.4% Market (36.3 → 42.0 fps), −7.3% Field. Real play −13.6% render CPU, Kokiri −20% |
| **Per-frame asset-lookup cache** (`LeanResLookup`) | About 418 resource lookups per frame, each hashing a path | Frame time −12.1% Market, −14.6% Field. Real play −11.7% |
| **Arena-allocated interpolation recording** (`LeanInterp`, `LeanInterpMap`) | Building the frame-interpolation tree cost 2.15 ms | Render CPU −8.0% Market (+0.9% more from moving the map instead of copying it), −2.7% Field. Real play −8.1% |
| **Exact cache of the game's own path lookups** (`LeanGameLookup`) | Repeated string building and hashing | −7.3% Market, −4.4% Field. Real play −5.6% |
| **Grouping opaque draws by shader and texture** (`LeanGroup`, `LeanTexReuse`) | Every program or texture switch flushes a batch | Draws per frame 261 → 145. Window CPU −5.8% and −1.1% |
| **Simpler colour-combiner shaders** (`ShaderOpts 3`): drop a no-op wrap, pass sizes as `vec2` | Fragment ALU is scarce on the SGX | Frame time −4.9% Market (29.9 → 31.4 fps), −4.6% Field |
| **Bilinear texture filtering by default** instead of three-point | Three-point costs three fetches plus ALU per pixel in the shader; on this content bilinear looks the same at 3× zoom | With the rest of the fork on: −19 to −36% frame time at 1250 MHz, −5 to −24% at 750 MHz, depending on the scene (report 12 §12.3, read across runs). Largest at Hyrule Field |
| **Allocation-free hot paths** (`LeanTexLoad`, `LeanCVar`, `LeanGeom`, `LeanCosmetics`) | Heap allocations per frame | −1% to −5% each. Allocations per frame 3,202 → 178 across these and the caches above |
| **Blend off for the present pass** (`LeanPresent`) | The final full-screen blit does not need blending | −0.32 ms of GPU time per frame (−14% of that pass) |
| **640×360 internal resolution** by default | Halving each axis quarters the pixels the GPU fills | ~+75% fps (16.4 → 28.7) against 1280×720. An early, noisier measurement taken before audio moved off the CPU, so indicative only |
| **Verified, synced saves** (SaveManager, libultraship `Config::Save`) | Saves were being silently corrupted, and `global.sav` was written over the live file | Safety, not speed. A full save with both syncs takes 38 ms |

Every Lean change is a `gSettings.Lean*` setting, on by default. Set it to 0 in
`shipofharkinian.json` to compare. The method, every measurement and the ideas that were tried and
dropped are in [docs/pyra/](docs/pyra/README.md). Start with
[08-optimisation-opportunities.md](docs/pyra/08-optimisation-opportunities.md) and
[11-two-core-render.md](docs/pyra/11-two-core-render.md).

---

# Upstream Ship of Harkinian

The rest of this README is upstream's, lightly trimmed. The links below point to the upstream project.

- Website: https://www.shipofharkinian.com/
- Discord: https://discord.com/invite/shipofharkinian (upstream support; not for this port)

The Ship does not include any copyrighted assets. You are required to provide a supported copy of the
game, and we do not condone piracy.

# Configuration

### Default keyboard configuration
| N64 | A | B | Z | Start | Analog stick | C buttons | D-Pad |
| - | - | - | - | - | - | - | - |
| Keyboard | X | C | Z | Space | WASD | Arrow keys | TFGH |

### Other shortcuts
| Keys | Action |
| - | - |
| ESC | Toggle menu |
| F2 | Toggle capture mouse input |
| F5 | Save state |
| F6 | Change state |
| F7 | Load state |
| F9 | Toggle Text-to-Speech (Windows and Mac only) |
| F11 | Fullscreen |
| Tab | Toggle Alternate assets |
| Ctrl+R | Reset |

# Project Overview
Ship of Harkinian (SOH) is built atop a custom library dubbed libultraship (LUS). Back in the N64 days, there was an SDK distributed to developers named libultra; LUS is designed to mimic the functionality of libultra on modern hardware. In addition, we are dependant on the source code provided by the OOT decompilation project.

In order for the game to function, you will require a **legally acquired** ROM for Ocarina of Time. Click [here](https://ship.equipment/) to check the compatibility of your specific rom. Any copyrighted assets are extracted from the ROM and reformatted as a .o2r archive file which the code uses.

### Graphics Backends
Currently, there are three rendering APIs supported: DirectX11 (Windows), OpenGL (all platforms), and Metal (MacOS). You can change which API to use in the `Settings` menu of the menubar, which requires a restart.  If you're having an issue with crashing, you can change the API in the `shipofharkinian.json` file by finding the line `gfxbackend:""` and changing the value to `sdl` for OpenGL. DirectX 11 is the default on Windows.

# Custom Assets

Custom assets are packed in `.otr` archive files. To use custom assets, place them in the `mods` folder.

If you're interested in creating and/or packing your own custom asset `.otr` files, check out the following tools:
* [**retro - OTR generator**](https://github.com/HarbourMasters64/retro)
* [**fast64 - Blender plugin**](https://github.com/HarbourMasters/fast64)

# Development
### Building

If you want to manually compile SoH, please consult the [building instructions](docs/BUILDING.md).

### Playtesting
If you want to playtest a continuous integration build, you can find them at the links below. Keep in mind that these are for playtesting only, and you will likely encounter bugs and possibly crashes. 

* [Windows](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-windows.zip)
* [macOS](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-mac.zip)
* [Linux](https://nightly.link/HarbourMasters/Shipwright/workflows/generate-builds/develop/soh-linux.zip)

### Further Reading
More detailed documentation can be found in the 'docs' directory, including the aforementioned [building instructions](docs/BUILDING.md).

* [Credits](docs/CREDITS.md)
* [Custom Music](docs/CUSTOM_MUSIC.md)
* [Controller Mapping](docs/GAME_CONTROLLER_DB.md)
* [Modding](docs/MODDING.md)
* [Versioning](docs/VERSIONING.md)

<a href="https://github.com/Kenix3/libultraship/">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/poweredbylus.darkmode.png">
    <img alt="Powered by libultraship" src="./docs/poweredbylus.lightmode.png">
  </picture>
</a>
