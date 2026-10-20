#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The Last Guardian frame rate patches (TLG_FPS=30|60|unlocked) and community mods/cheats,
written together to out/patches.bin.

Each built-in frame rate patch is a list of {image offset, expected bytes, new bytes}.
The known eboot 01.00 and 01.03 builds have different patch locations. The generator
recognizes verified executables and validates the original bytes before any write.
Unknown builds are NEVER blindly patched. The
loader (probe.c apply_patches) applies the file after relocation. Image offsets are eboot
virtual addresses; the Ghidra addresses in the comments (base 0x1000000) are offset + 0x1000000.
Research: docs/60fps.md.

30 with no mods enabled writes an empty patch file: the game runs exactly as on PS4.

Mods/cheats (--mods-dir/--mods-config, mod_patches.py): shadPS4/GoldHEN cheat XML files the
player drops in the mods folder. Nothing in there has any effect until it is enabled (the
file's own isEnabled="true", or mods.json), same contract as the built-in patches above.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mod_patches

TITLE_IDS = {'CUSA03627', 'CUSA03745'}   # US, EU (tlg_run.py LANGUAGES_BY_TITLE)
EBOOT_103_SHA256 = '91dce685be81f8f6bc88097d4fc7a62e97ca08d109621c57fe9371ae125feb3c'
# Verified from the original v01.00 PS4 SELF dump. Never include the game file in this repo.
EBOOT_100_SELF_SHA256 = 'a25497ce99c4884dac673872a727330097b97487943affb70d4b1b432969f473'
EBOOT_100_ELF_SHA256 = '283796e8cc3837523bd8bb3a79e5c588ba818e7db25aad0e02d03a804ea0ca61'
SELF_MAGIC = b'O\x15=\x1d'
TEXT_FILE_OFFSET = 0x4000      # PT_LOAD 0 (R E): file offset of vaddr 0
TEXT_SIZE = 0x16b2d5c
GHIDRA_BASE = 0x1000000


def f32(value):
    return struct.pack('<f', value)


def rip_disp(instruction_end, target):
    """disp32 of a rip-relative operand: target - address of the next instruction (Ghidra addresses)."""
    return struct.pack('<i', target - instruction_end)


# 01.03 patches by name: [(ghidra address, expected, replacement, explanation)].
PATCHES_103 = {
    # JsDevice/JsSwapChain init (0x1c4f9f0): sceVideoOutSetFlipRate(handle, 1) -> 0.
    # Rate 1 shows a frame every second vblank (30 Hz on the 60 Hz vblank); 0 every vblank.
    'flip_rate_60': [
        (0x1c5151a, bytes.fromhex('be01000000'), bytes.fromhex('be00000000'),
         'sceVideoOutSetFlipRate(h, 1) -> (h, 0)'),
    ],
    # Lua SetMaxFps(fps) binding (0x12091a0): game+0x180 = 1.0/fps, the engine's minimum frame
    # time. The scripts call SetMaxFps(30) (states/main.lua, common/gamesetuptbl.lua).
    # 60: load 0.5 (0x24f17e8) instead of 1.0 (0x24eaf84) -> 0.5/fps: SetMaxFps(30) means 60.
    'max_fps_double': [
        (0x12091db, bytes.fromhex('c5fa100d') + rip_disp(0x12091e3, 0x24eaf84),
         bytes.fromhex('c5fa100d') + rip_disp(0x12091e3, 0x24f17e8),
         'SetMaxFps(n): minimum frame time 1/n -> 0.5/n'),
    ],
    # Unlocked: vdivss xmm1,xmm1,xmm0 -> vxorps xmm1,xmm1,xmm1: minimum frame time 0 (no limit),
    # as SetMaxFps(0) does.
    'max_fps_off': [
        (0x12091e3, bytes.fromhex('c5f25ec8'), bytes.fromhex('c5f057c9'),
         'SetMaxFps(n): minimum frame time 1/n -> 0'),
    ],
    # SmoothDeltaTime (0x1d16480): a frame time in (0.031667, 0.035) s becomes exactly 1/30 s
    # before the 2nd-order filter and the 1/360 s rounding. These three constants are read only
    # here. 60: the same snap one octave up, (0.0158333, 0.0175) -> 1/60.
    'dt_snap_60': [
        (0x24f17f8, f32(0.035), f32(0.0175), 'dt snap upper bound 0.035 -> 0.0175'),
        (0x24f17fc, f32(1 / 30), f32(1 / 60), 'dt snap target 1/30 -> 1/60'),
        (0x24f1800, f32(0.0316666663), f32(0.0158333331), 'dt snap lower bound 0.031667 -> 0.015833'),
    ],
}

# 01.00 locations verified against the supplied original 01.00 executable.
# The original flip-rate call site is distinct from 01.03, as is the frame-time triple.
PATCHES_100 = {
    'flip_rate_60': [
        (0x1b8be7c, bytes.fromhex('be01000000'), bytes.fromhex('be00000000'),
         '01.00 sceVideoOutSetFlipRate: mov esi,1 -> mov esi,0'),
    ],
    'dt_snap_60': [
        (0x24d9920, f32(0.035), f32(0.0175), '01.00 dt snap upper bound 0.035 -> 0.0175'),
        (0x24d9924, f32(1 / 30), f32(1 / 60), '01.00 dt snap target 1/30 -> 1/60'),
        (0x24d9928, f32(0.0316666663), f32(0.0158333331),
         '01.00 dt snap lower bound 0.031667 -> 0.015833'),
    ],
}

BUILD_PROFILES = {
    '01.00': {'text_size': 0x168a5ec, 'patches': PATCHES_100,
              'source_hashes': {EBOOT_100_SELF_SHA256, EBOOT_100_ELF_SHA256}},
    '01.03': {'text_size': 0x16b2d5c, 'patches': PATCHES_103,
              'source_hashes': {EBOOT_103_SHA256}},
}
PATCHES = PATCHES_103  # backward-compatible alias for 01.03 test fixtures
SUPPORTED_PATCH_NAMES = set(PATCHES_100) | set(PATCHES_103)

MODES = {
    '30': [],
    # max_fps_double/max_fps_off are left out: 60 and unlocked were reached without them
    # (game+0x180 is no frame limiter in the tested scenes; its reader is not found yet).
    # TLG_FPS_PATCHES=flip_rate_60,max_fps_off tries them.
    '60': ['flip_rate_60', 'dt_snap_60'],
    'unlocked': ['flip_rate_60'],
}
# The engine rate each mode presents at most (TLG_ENGINE_FPS: the overlay's frame limiter and
# tlg_graphics.h PresentLimit). Unlocked is paced by the present thread (TLG_VBLANK_HZ=0).
ENGINE_FPS = {'30': 30, '60': 60, 'unlocked': 240}


def normalize_mode(value):
    value = (value or '30').strip().lower()
    return {'unlimited': 'unlocked', 'uncapped': 'unlocked', 'free': 'unlocked', '0': 'unlocked'}.get(value, value)


def mode_writes(eboot, names, profile=None):
    """Create verified image-relative writes for the selected game build.

    The optional profile keeps older 01.03 tests and external scripts compatible.
    """
    profile = profile or BUILD_PROFILES['01.03']
    writes = []
    for name in names:
        if name not in profile['patches']:
            raise ValueError(f'{name} is not available for this eboot version')
        for address, expected, new, what in profile['patches'][name]:
            offset = address - GHIDRA_BASE
            if (not 0 <= offset or offset + len(expected) > profile['text_size']
                    or len(expected) != len(new)):
                raise ValueError(f'{name}: bad patch entry at {address:#x}')
            found = eboot[TEXT_FILE_OFFSET + offset:TEXT_FILE_OFFSET + offset + len(expected)]
            if found != expected:
                raise ValueError(f'{name}: {address:#x} has {found.hex()}, expected {expected.hex()}')
            writes.append((offset, new, f'{name}: {address:#x} {what}'))
    return writes


def identify_build(original, digest, lines):
    """Identify a trusted build, accepting alternate decrypted SELF wrappers only when
    both independent FPS signature regions uniquely match one build.
    """
    version = next((name for name, profile in BUILD_PROFILES.items()
                    if digest in profile['source_hashes']), None)
    elf = original
    if original[:4] == SELF_MAGIC:
        try:
            from prepare import parse_self
            elf = bytes(parse_self(original)[0])
        except (ValueError, IndexError, KeyError) as error:
            lines.append(f'Frame rate: cannot read the SELF eboot.bin ({error})')
            return None, None
        elf_digest = hashlib.sha256(elf).hexdigest()
        if elf_digest == EBOOT_100_ELF_SHA256:
            version = '01.00'
        elif version is None:
            candidates = []
            for name, profile in BUILD_PROFILES.items():
                try:
                    mode_writes(elf, ['flip_rate_60', 'dt_snap_60'], profile)
                    candidates.append(name)
                except ValueError:
                    pass
            if len(candidates) == 1:
                version = candidates[0]
                lines.append(f'Frame rate: validated alternate SELF version {version}')
    if version and elf[:4] != b'\\x7fELF':
        lines.append('Frame rate: executable is not a decrypted ELF')
        return None, None
    return version, elf


def write_patch_file(path, writes):
    """G3PATCH2 (probe.c apply_patches): base 0 (no pointer slots), then (offset, length, bytes).
    Base 0 only matters for a write that lands exactly on a relocated pointer slot (none of the
    built-in frame rate patches or typical shadPS4/GoldHEN literal-value mods do); it does not
    change plain byte/float writes either way."""
    blob = struct.pack('<8sQQ', b'G3PATCH2', 0, len(writes))
    for offset, data, _ in writes:
        blob += struct.pack('<QQ', offset, len(data)) + data
    Path(path).write_bytes(blob)


def read_eboot_elf(game_dir):
    """eboot.bin as a plain ELF, unwrapping a SELF dump of any game version; None if unreadable
    or an unparsable SELF. Independent of the v1.03 pin below: mods filter by their own AppVer."""
    try:
        eboot = (Path(game_dir) / 'eboot.bin').read_bytes()
    except OSError:
        return None
    if eboot[:4] == SELF_MAGIC:
        try:
            from prepare import parse_self
            return bytes(parse_self(eboot)[0])
        except ValueError:
            return None
    return eboot


def compile_mods(game_dir, eboot, mods_dir, mods_config, lines):
    """[(offset, bytes, description)] from the player's mods folder, or [] when mods_dir is not
    set, the eboot could not be read, or nothing in there is enabled."""
    if not mods_dir or not eboot:
        return []
    try:
        segments = mod_patches.eboot_segments(eboot)
        app_version = mod_patches.game_app_version(game_dir)
        found = mod_patches.external_patches(mods_dir, app_version, TITLE_IDS)
        selected = mod_patches.external_selection(found, mods_config)
        return mod_patches.compile_external(selected, segments)
    except (OSError, ValueError, struct.error) as error:
        lines.append(f'Mods: {error}: not applied')
        return []


def prepare(game_dir, out_dir, mode, mods_dir=None, mods_config=None):
    """Writes out/patches.bin for the mode plus any enabled mod; returns (applied mode, lines).
    Any problem with the frame rate patches (other eboot build, unexpected bytes) falls back to
    30 FPS without them; a mod problem only drops that mod, never the frame rate patches."""
    mode = normalize_mode(mode)
    lines = []
    if mode not in MODES:
        lines.append(f'Frame rate: unknown TLG_FPS={mode!r}; using 30')
        mode = '30'
    fps_writes = []
    eboot = None
    # Optional developer override; never apply 01.03-only names to the 01.00 build.
    requested = [n.strip() for n in os.environ.get('TLG_FPS_PATCHES', '').split(',')
                 if n.strip() in SUPPORTED_PATCH_NAMES]
    names = requested or MODES[mode]
    if names:
        try:
            original = (Path(game_dir) / 'eboot.bin').read_bytes()
        except OSError as error:
            original = b''
            lines.append(f'Frame rate: cannot read eboot.bin ({error})')
        digest = hashlib.sha256(original).hexdigest()
        version, eboot = identify_build(original, digest, lines)
        if not version:
            lines.append(f'Frame rate: unknown executable ({digest[:16]}...); '
                         f'{mode} FPS patches not applied, using 30')
            mode = '30'
        else:
            try:
                fps_writes = mode_writes(eboot, names, BUILD_PROFILES[version])
                lines.append(f'Frame rate: eboot version {version} recognized; '
                             f'{len(fps_writes)} validated frame-rate writes')
            except ValueError as error:
                lines.append(f'Frame rate: {version} {error}: patches not applied, using 30')
                mode, fps_writes = '30', []
    for _, data, what in fps_writes:
        lines.append(f'Frame rate patch {what} ({data.hex()})')
    mod_writes = []
    if mods_dir:
        mod_writes = compile_mods(game_dir, eboot or read_eboot_elf(game_dir), mods_dir, mods_config, lines)
        for _, data, what in mod_writes:
            lines.append(f'Mod {what} ({data.hex()})')
    writes = fps_writes + mod_writes
    write_patch_file(Path(out_dir) / 'patches.bin', writes)
    lines.append(f'Frame rate: {mode} FPS mode, {len(writes)} patch writes')
    return mode, lines


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('game_dir')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--mode', default='30', help='30, 60 or unlocked')
    p.add_argument('--mods-dir', help='folder of shadPS4/GoldHEN cheat XML files')
    p.add_argument('--mods-config', help='mods.json: {"enabled": [...], "disabled": [...]}')
    a = p.parse_args()
    _, lines = prepare(a.game_dir, a.out, a.mode, a.mods_dir, a.mods_config)
    print('\n'.join(lines))


if __name__ == '__main__':
    sys.exit(main())
