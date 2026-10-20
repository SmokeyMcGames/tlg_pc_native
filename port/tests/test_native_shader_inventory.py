"""Synthetic OrbShdr regression tests. Never contains proprietary game code."""
from paths import ROOT  # noqa: F401
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import native_shader_inventory as inv


def synthetic_shader(kind=4):
    code = inv.GCN_HEADER + bytes(40)
    usage = bytes(20)
    packed = (len(code) << 8) | (kind << 2)
    footer = (
        inv.BINARY_MAGIC + bytes((7,)) + struct.pack('<I', packed)
        + bytes((0, 0, 0, 0)) + struct.pack('<III', 0x1234, 0x5678, 0xabcd)
    )
    return code + usage + footer, code


class NativeShaderInventoryTests(unittest.TestCase):
    def test_valid_shader_metadata_and_code_bytes(self):
        blob, code = synthetic_shader()
        found = inv.discover([(0x4000, blob)])
        self.assertEqual(len(found), 1)
        entry = found[0]
        self.assertEqual(entry['stage'], 'compute')
        self.assertEqual(entry['code_size'], len(code))
        self.assertEqual(entry['usage_table_size'], 20)
        self.assertEqual(entry['code_guest_offset'], 0x4000)
        self.assertEqual(entry['_code'], code)
        self.assertEqual(entry['sha256'], hashlib.sha256(code).hexdigest())

    def test_invalid_metadata_never_extracts_code(self):
        blob, _ = synthetic_shader()
        bad = bytearray(blob)
        struct.pack_into('<I', bad, len(inv.GCN_HEADER) + 40 + 20 + 8,
                         (8192 << 8) | (4 << 2))
        self.assertEqual(inv.discover([(0, bytes(bad))]), [])
        self.assertEqual(inv.discover([(0, bytes(bad[:-5]))]), [])

    def test_recursive_loose_assets_manifest_and_optional_dump(self):
        with tempfile.TemporaryDirectory() as base:
            root = Path(base, 'assets')
            folder = root / 'data_trico' / 'ps4_shader_cache' / 'material'
            folder.mkdir(parents=True)
            good = folder / 'example.sb'
            blob, code = synthetic_shader(kind=0)
            good.write_bytes(blob)
            (folder / 'invalid.sb').write_bytes(b'not a shader' * 10)
            assets, skipped = inv.scan_shader_root(root)
            self.assertEqual((len(assets), skipped), (1, 1))
            self.assertEqual(assets[0]['stage'], 'pixel')
            self.assertEqual(assets[0]['asset_path'],
                             'data_trico/ps4_shader_cache/material/example.sb')
            output = Path(base, 'manifest.json')
            dump = Path(base, 'local_gcns')
            self.assertEqual(inv.main(['--shader-root', str(root), '--out', str(output),
                                       '--dump-code', str(dump)]), 0)
            self.assertTrue(output.read_text().endswith('\n'))
            result = json.loads(output.read_text())
            self.assertEqual(result['schema'], 2)
            self.assertEqual(result['count'], 1)
            self.assertEqual(result['skipped_shader_assets'], 1)
            self.assertEqual(len(list(dump.glob('*.gcn'))), 1)
            self.assertEqual(next(dump.glob('*.gcn')).read_bytes(), code)


if __name__ == '__main__':
    unittest.main()
