"""Regression tests for launcher-integrated stable GPU and FPS version selection."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import tlg_run


class GpuLauncherTests(unittest.TestCase):
    def settings(self, **overrides):
        result = dict(tlg_run.DEFAULT_SETTINGS)
        result['game_dir'] = ''
        result.update(overrides)
        return result

    def test_default_stability_applied_without_batch_launcher(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            env = tlg_run.build_env(self.settings())
        self.assertEqual(env['TLG_PREP_WORKERS'], '2')
        self.assertEqual(env['TLG_ASYNC_SHADERS'], '0')
        self.assertNotIn('TLG_NATIVE_DRAW_BRIDGE', env)
        self.assertNotIn('TLG_NATIVE_COMPUTE_RECIPES', env)

    def test_disable_stability_uses_upstream_defaults(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            env = tlg_run.build_env(self.settings(gpu_stability=False))
        self.assertNotIn('TLG_PREP_WORKERS', env)
        self.assertNotIn('TLG_ASYNC_SHADERS', env)

    def test_explicit_environment_wins(self):
        with mock.patch.dict(os.environ, {
                'TLG_PREP_WORKERS': '0', 'TLG_ASYNC_SHADERS': '1'}, clear=True):
            env = tlg_run.build_env(self.settings(native_draw_bridge=True))
        self.assertEqual(env['TLG_PREP_WORKERS'], '0')
        self.assertEqual(env['TLG_ASYNC_SHADERS'], '1')
        self.assertEqual(env['TLG_NATIVE_DRAW_BRIDGE'], '1')
        self.assertEqual(env['TLG_NATIVE_COMPUTE_RECIPES'], '1')

    def test_executable_version_hints_are_hash_pinned(self):
        import tlg_patches
        with tempfile.TemporaryDirectory() as root:
            Path(root, 'eboot.bin').write_bytes(b'fake ELF')
            for digest, expected in [
                (tlg_patches.EBOOT_100_SELF_SHA256, '01.00'),
                (tlg_patches.EBOOT_103_SHA256, '01.03'),
                ('0' * 64, None),
            ]:
                tlg_run._recognized_eboot_digest.cache_clear()
                dummy = mock.Mock()
                dummy.hexdigest.return_value = digest
                with mock.patch.object(tlg_run.hashlib, 'file_digest', return_value=dummy):
                    self.assertEqual(tlg_run.fps_patch_version(root), expected)
        self.assertIsNone(tlg_run.fps_patch_version('/a/missing/folder'))


if __name__ == '__main__':
    unittest.main()
