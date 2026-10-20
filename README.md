<p align="center"><img src="port/packaging/icon/tlg-icon-1024.png" width="160" alt=""></p>

<h1 align="center">The Last Guardian — Native PC Port</h1>

<p align="center">
  <a href="https://github.com/HayatoG/tlg_pc/releases/latest">Download</a> ·
  <a href="https://discord.gg/UMkdMYZh9g">Discord</a>
</p>

# The Last Guardian — native PC port

**The Last Guardian** (PS4, CUSA03627 US / CUSA03745 EU, update 01.03) running on PC with graphics, audio, controller, mouse and keyboard and saves, at a steady 30 FPS (optional, experimental 60 FPS and unlocked modes) and without a CPU emulator. The game's original code runs natively on the processor. The PS4 system is reimplemented, and the GPU is translated to Vulkan.

> **No game files come with this project.** To play, you need your own copy, dumped from your own PS4.

The screenshots and the performance chart are at the top of this page.

## What's new

- **60 FPS and unlocked frame rate (experimental):** launcher → Video → "Frame rate (experimental)", or `TLG_FPS=30|60|unlocked`. The default is still 30 FPS, as on PS4. Details in [`docs/60fps.md`](docs/60fps.md) (in Portuguese, [#5](https://github.com/HayatoG/tlg_pc/issues/5)).
- **Image filters:** SMAA anti-aliasing, RCAS (FSR 1) or CAS (AMD) sharpening and colour adjustments (brightness, contrast, saturation, gamma), in the launcher and live in the in-game menu (Insert → Image) ([#2](https://github.com/HayatoG/tlg_pc/issues/2)).
- **Mouse and keyboard:** the mouse turns the camera (sensitivity and inverted Y in the launcher), and keys and mouse buttons can be remapped ([#8](https://github.com/HayatoG/tlg_pc/issues/8)).
- **Motion sensor:** the DualShock 4/DualSense gyro works as is. On controllers without one, a tap on Share (or L3+R3) toggles "sensor mode": the left stick tilts the controller and cross/A shakes it. On the keyboard, T/G tilt and Y shakes.
- **PS4-like brightness and lighting:** the game's auto exposure now reads the right value from the GPU, so outdoor scenes no longer blow out to white (`TLG_READBACK_LINEAR=1`, on by default; `0` restores the old behaviour) ([#38](https://github.com/HayatoG/tlg_pc/issues/38), [#40](https://github.com/HayatoG/tlg_pc/issues/40)).
- **Sticks:** a 10% dead zone against drift (`TLG_STICK_DEADZONE`), and the last controller used is the one that counts.
- **Touchpad without a touchpad:** hold View/Select (or Tab) and move the right stick (or the arrow keys) to swipe; a quick tap still clicks.
- **Performance:** FPS no longer drops over time (shader variants are now reused), and repeated partial image transitions are skipped.
- **Automatic shader cache:** it lives in `user/cache/`, next to the saves (Linux and Windows; the old cache in `~/.local/share/shadPS4` or `user/user/` is moved over on the first start). The folder is keyed by the shader recompiler version, and the old cache is deleted on its own after an update. To rebuild everything, the launcher has a **Clear shader cache** button (or `./run_tlg.sh --clear-shader-cache`); it deletes only this game's compiled shaders, never saves or settings.

## Status

| Area | State |
|---|---|
| Boot, menu, intro and gameplay | ✅ Works; 10+ minute sessions without failures |
| Graphics | ✅ Native 1080p, no shimmering; optional SMAA, RCAS/CAS and colour adjustments |
| Audio | ✅ With `libSceNgs2.sprx` from your console's firmware |
| Controller | ✅ DualSense, DualShock 4, Xbox and 8BitDo via SDL3, with hot-plug; real or simulated motion sensor |
| Mouse and keyboard | ✅ Mouse camera and remapping in the launcher |
| Saves | ✅ Autosave and Continue |
| Performance | ✅ 30 FPS, as on PS4; 🧪 optional 60 FPS and unlocked, being tested ([#5](https://github.com/HayatoG/tlg_pc/issues/5)) |
| Linux | ✅ Tested (Nobara/Fedora 44) |
| Windows | 🧪 Automatic build, not yet tested in game |
| macOS (Apple Silicon) | 📋 Planned ([#1](https://github.com/HayatoG/tlg_pc/issues/1)) |

## Download

The [**latest release**](https://github.com/HayatoG/tlg_pc/releases/latest) is built by GitHub Actions on every change; older ones stay under [Releases](https://github.com/HayatoG/tlg_pc/releases) as history:

- **Linux x86-64** (glibc 2.39+: Ubuntu 24.04, Fedora 40, SteamOS 3.6 or newer): `tlg-linux-x86_64.tar.gz`
- **Windows 10/11 64-bit**: `tlg-windows-x86_64.zip`

## Required game files

Everything comes from **your own** PS4 (GoldHEN + Itemzflow). None of it is in the download. Use **The Last Guardian CUSA03627 (US) or CUSA03745 (EU), with update 01.03**, merged into one folder, with the base game and the update on top. About 25 GB.

| File | From | Size | Purpose |
|---|---|---|---|
| `eboot.bin` | update 01.03 | 89 MB | the game code (must be the 1.03 one) |
| `sce_module/libc.prx`, `sce_module/libSceFios2.prx` | update 01.03 | 1.5 MB | the game's own libraries |
| `sce_sys/` (`param.sfo`, `playgo-chunk.dat`, `icon0.png`, `pic0.png`, `pic1.png`) | base + update | ~5 MB | metadata, install info and the launcher art |
| `trico_boot.psarc`, `trico_001.psarc`, `trico_002.psarc`, `trico_003.psarc` | base game | 2.8 + 5.8 + 5.4 + 6.8 GB | main content |
| `trico_bootp.psarc`, `trico_001p.psarc`, `trico_002p.psarc`, `trico_003p.psarc` | update 01.03 | ~1.6 GB | update fixes |
| `trico_enu.psarc` / `trico_ptb.psarc` / `trico_esl.psarc` / `trico_frc.psarc` (+ the update's `…p.psarc`) | base + update | ~280 MB each | voices and text: English, Portuguese, Spanish, French |
| `sce_discmap.plt`, `sce_discmap_patch.plt` | base, update | small | file map (recommended) |

**CUSA03745 (EU)** ships 15 languages instead of CUSA03627's four: English, French, Spanish (Spain), German, Italian, Dutch, Portuguese (Portugal), Russian, Finnish, Swedish, Danish, Norwegian, Polish, Turkish and Arabic. The launcher lists the languages of the loaded CUSA.

**From the console firmware (optional, for audio):** `libSceNgs2.sprx` (372 KB), from `/system/common/lib/` on your PS4. It goes in the `system_lib/` folder next to the executable.

**How to build the folder:**
1. In Itemzflow, dump the game (`CUSA03627` or `CUSA03745`) and the update (`CUSA03627-patch` or `CUSA03745-patch`) to an exFAT USB drive.
2. On the PC, copy the base game folder, then copy the update's contents **over it**, replacing files (`eboot.bin`, `sce_module/`, `sce_sys/`).
3. Check the sizes. Files over 4 GB sent through GoldHEN 2.2's FTP arrive truncated, so use the USB drive.

The full list, which also goes in the notes of every Release, is in [`docs/ARQUIVOS_NECESSARIOS.md`](docs/ARQUIVOS_NECESSARIOS.md).

**PC:** a graphics card with Vulkan 1.3 (6 GB of VRAM recommended) and ~25 GB free.

## How to play

1. Extract the package and put `libSceNgs2.sprx` in the `system_lib/` folder.
2. Open **The Last Guardian**, pick the game folder and the language (CUSA03627 has English, Portuguese, Spanish and French; CUSA03745 has 15) and, if you like, the frame rate and image filters. Click **Play**.
3. In game, **F10** toggles the FPS counter, and **Insert** (or R3 + L2 on a controller) opens the port's menu.
4. Controller (PS4/PS5, Xbox) or keyboard and mouse: the mouse turns the camera, **Esc** releases the mouse and a click takes it back. The layout and remapping are in [`README-player.md`](port/packaging/README-player.md) and in the launcher's **Controls** box.

The full guide, with common problems, is in [`README-player.md`](port/packaging/README-player.md), which also ships inside the package.

**Mods and cheats.** The launcher's **Mods / Cheats…** button opens the `user/mods/` folder and lists any cheat XML files (shadPS4/GoldHEN format) you drop there; nothing has any effect until you turn it on in the list (or the file's own `isEnabled="true"`). A cheat written for another game version is skipped on its own, without affecting the others.

**CPUs without MOVBE, BMI1 or LZCNT** (Intel before Haswell): the game's code uses those instructions (1941 sites in the 1.03 eboot with its libc and Fios2; no SSE4A). On those CPUs the port replaces them with an emulation at start (needs `iced-x86`, included in the packages). `TLG_ISA_REWRITE=1` forces the emulation of every set (testing), a list such as `TLG_ISA_REWRITE=movbe,bmi1` forces only those and `TLG_ISA_REWRITE=0` turns it off. The log shows `ISA: CPU lacks ...` or `ISA: forced rewrite of N sites`.

**Windows red-zone protection.** PS4 code keeps variables in the 128 bytes below `rsp` (the "red zone"), and Windows writes there on every exception (the port uses exceptions to track GPU memory), which crashed inFAMOUS at random places. At start the port rewrites the memory instructions of the functions that use the red zone so they run with `rsp` 128 bytes lower (from inFAMOUS, iss_pc #29/#41). On by default on Windows; `TLG_RED_ZONE_REWRITE=0` turns it off and `TLG_RED_ZONE_REWRITE=1` turns it on on Linux (testing). Linux test: `TLG_CLOBBER_RED_ZONE=<bytes>` zeroes that many bytes below `rsp` on every exception, like Windows (`1` = 128). Details: [port/docs/red_zone.md](port/docs/red_zone.md).

**Windows file read and write errors.** On Windows the port keeps each file's position itself, reads and writes through its own buffer (not straight into game memory the GPU protects) and retries the errors that case gives. Every failed read, write, open or rename goes to the log (`Runtime: read(...) ... Win32 error N`), up to 32 of each kind: send that log if the game says it could not read or save its data (on inFAMOUS it was "disc is corrupt").

## Building from source

```bash
git clone --recursive https://github.com/HayatoG/tlg_pc.git && cd tlg_pc
cd port && TLG_LTO=OFF bash build.sh   # Linux: SDL3, Vulkan, FFmpeg, Boost, fmt, Zydis, glslang
cd .. && ./tlg_launcher.sh             # or ./run_tlg.sh from a terminal
```

On Windows, the build runs in MSYS2 CLANG64 ([`port/packaging/windows/README.md`](port/packaging/windows/README.md)). The workflow [`.github/workflows/build.yml`](.github/workflows/build.yml) shows the full process on both platforms.

## How it works

| Layer | What it does | Based on |
|---|---|---|
| Loader | Loads `eboot.bin` 1.03 and links the game's modules (`libc.prx`, `libSceFios2.prx`) and the console's `libSceNgs2.sprx` | [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), [gow3_pc](https://github.com/FelipePrado0/gow3_pc) |
| Runtime | Reimplements the PS4 libraries: kernel, threads, files, controller, audio, saves | gow3_pc + 54 new functions for TLG |
| Video | Translates Gnm commands and GCN shaders to Vulkan/SPIR-V | [shadPS4](https://github.com/shadps4-emu/shadPS4) video core |

Technical notes (Ghidra findings, fixed bugs, `TLG_*` options) are in [`docs/TECNICO.md`](docs/TECNICO.md), in Portuguese. The history of what was done and what is left is in [`docs/PROGRESSO.md`](docs/PROGRESSO.md).

## Next steps

[Graphics roadmap (#7)](https://github.com/HayatoG/tlg_pc/issues/7): testing 60 FPS through the whole game (#5), game effects (#3), internal resolution (#4) and FSR/DLSS (#6). Audio without the firmware file (#11), importing the game from a PKG (#9), macOS (#1) and new fixes are tracked in the issues as well.

## Credits and legal notice

Code base: [gow3_pc](https://github.com/FelipePrado0/gow3_pc) (FelipePrado0), [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) and [shadPS4](https://github.com/shadps4-emu/shadPS4), all GPL-2.0, as is this project ([LICENSE](port/LICENSE)). ATRAC9 audio via [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9).

This project is not affiliated with Sony Interactive Entertainment, SIE Japan Studio or genDESIGN. The Last Guardian is a trademark of Sony Interactive Entertainment. No game code, files or art are distributed here. The icon is original artwork made for this project.
