#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Inventory GCN shaders embedded in a legally obtained PS4 eboot.bin.

No proprietary data is bundled. Default mode writes JSON metadata only.
--dump-code extracts the raw GCN bytecode into a caller-selected local folder;
it must not be committed or redistributed. Full ahead-of-time SPIR-V requires
stage specialization, resource descriptors, and the Vulkan GPU profile.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

from tlg_patches import (
    EBOOT_100_SELF_SHA256, EBOOT_100_ELF_SHA256, EBOOT_103_SHA256,
)

SELF_MAGIC = b'O\x15=\x1d'
GCN_HEADER = bytes.fromhex('ff03ebbe')
BINARY_MAGIC = b'OrbShdr'
STAGES = {
    0: 'unknown', 1: 'vertex', 2: 'pixel', 3: 'geometry',
    4: 'compute', 5: 'export', 6: 'local', 7: 'hull',
}


def executable_bytes(path):
    original = Path(path).read_bytes()
    source_sha256 = hashlib.sha256(original).hexdigest()
    elf = original
    if original.startswith(SELF_MAGIC):
        from prepare import parse_self
        elf = bytes(parse_self(original)[0])
    if elf[:4] != b'\x7fELF':
        raise ValueError('eboot must be a decrypted ELF or a supported plaintext SELF')
    if elf[4] != 2 or elf[5] != 1:
        raise ValueError('expected ELF64, little-endian')
    phoff = struct.unpack_from('<Q', elf, 32)[0]  # ELF64 e_phoff, not e_entry
    phentsize, phnum = struct.unpack_from('<HH', elf, 54)
    if phentsize < 56 or phnum < 1 or phnum > 64:
        raise ValueError('invalid ELF program-header table')
    segments = []
    for index in range(phnum):
        at = phoff + index * phentsize
        if at + 56 > len(elf):
            raise ValueError('truncated ELF program headers')
        kind, flags, file_off, vaddr, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', elf, at)
        if kind == 1 and flags & 1 and size and file_off + size <= len(elf):
            segments.append((vaddr, elf[file_off:file_off + size]))
    if not segments:
        raise ValueError('no executable PT_LOAD segment')
    return source_sha256, segments


def discover(segments):
    """Return verified code intervals and shader metadata, not raw asset bytes."""
    found = []
    for guest_base, segment in segments:
        search_at = 0
        while True:
            marker = segment.find(BINARY_MAGIC, search_at)
            if marker < 0:
                break
            search_at = marker + len(BINARY_MAGIC)
            if marker + 28 > len(segment):
                continue
            # ShaderBinaryInfo, size 0x1c. Length is the upper 24 bits of
            # the DWORD after signature[7] + version[1].
            magic, version, packed = struct.unpack_from('<7sBI', segment, marker)
            if magic != BINARY_MAGIC or version not in (6, 7):
                continue
            byte_count = packed >> 8
            shader_type = (packed >> 2) & 0xf
            if not 4 <= byte_count <= 4 * 1024 * 1024 or byte_count % 4:
                continue
            chunk_offset, input_slots = struct.unpack_from('<BB', segment, marker + 12)
            hash_low, hash_high, crc = struct.unpack_from('<III', segment, marker + 16)
            # GPU code begins with the known shader header. The metadata may
            # have input-usage tables between the code and OrbShdr footer.
            candidates = []
            begin = max(0, marker - byte_count - 128)
            pos = segment.find(GCN_HEADER, begin, marker)
            while pos != -1 and pos < marker:
                table_bytes = marker - pos - byte_count
                if (table_bytes >= 0 and table_bytes <= 128
                        and table_bytes % 4 == 0):
                    candidates.append((pos, table_bytes))
                pos = segment.find(GCN_HEADER, pos + 4, marker)
            if len(candidates) != 1:
                continue  # never guess at ambiguous or corrupt binary boundaries
            code_at, extra_bytes = candidates[0]
            code = segment[code_at:code_at + byte_count]
            found.append({
                'stage': STAGES.get(shader_type, 'other'),
                'type': shader_type,
                'binary_version': version,
                'code_guest_offset': guest_base + code_at,
                'binary_info_guest_offset': guest_base + marker,
                'code_size': byte_count,
                'usage_table_size': extra_bytes,
                'usage_slots': input_slots,
                'chunk_offset_dwords': chunk_offset,
                'shader_hash': f'{hash_high:08x}{hash_low:08x}',
                'crc32': f'{crc:08x}',
                'sha256': hashlib.sha256(code).hexdigest(),
                '_code': code,  # removed before serializing metadata
            })
    return found


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('eboot', type=Path, help='own legally obtained game eboot.bin')
    p.add_argument('--out', type=Path, help='where to write metadata manifest JSON')
    p.add_argument('--dump-code', type=Path,
                   help='optional local GCN bytecode output folder (never publish these files)')
    p.add_argument('--allow-unknown-build', action='store_true',
                   help='accept unrecognized executable only when each shader validates')
    args = p.parse_args(argv)
    sha, segments = executable_bytes(args.eboot)
    known = {
        EBOOT_100_SELF_SHA256: '01.00', EBOOT_100_ELF_SHA256: '01.00',
        EBOOT_103_SHA256: '01.03',
    }
    version = known.get(sha)
    if not version and not args.allow_unknown_build:
        p.error('unknown eboot SHA-256; use --allow-unknown-build to scan by metadata')
    found = discover(segments)
    if not found:
        raise ValueError('no validated GCN shader binaries found')
    serializable = [{k: v for k, v in entry.items() if k != '_code'} for entry in found]
    manifest = {
        'schema': 1, 'build_version': version or 'unidentified',
        'source_sha256': sha,
        'count': len(found),
        'embedded_shaders': serializable,
        'note': 'metadata only; GCN translation to SPIR-V still requires specialization',
    }
    text = json.dumps(manifest, indent=2) + '\n'
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text, encoding='utf-8')
    else:
        sys.stdout.write(text)
    if args.dump_code:
        args.dump_code.mkdir(parents=True, exist_ok=True)
        for entry in found:
            name = (f"{entry['stage']}-{entry['code_guest_offset']:08x}-"
                    f"{entry['sha256'][:12]}.gcn")
            (args.dump_code / name).write_bytes(entry['_code'])
        print(f'Extracted {len(found)} raw shaders locally: {args.dump_code}',
              file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
