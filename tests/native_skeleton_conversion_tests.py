"""Exercise native resource evidence and reject inconsistent conversions offline."""
import sys
sys.dont_write_bytecode = True
import importlib.util
from pathlib import Path
import struct
import unittest
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('native_checks', ROOT/'KHARVOX/tools/verify_native_skeleton_conversion.py')
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)
sys.path.insert(0, str(ROOT/'.tools/cast/libraries/python'))
from cast import Cast, Model


class NativeConversionTests(unittest.TestCase):
    def raw(self, stem='plasmarifle'):
        return (ROOT/'build/hand-workshop/plasma_rifle/native-reference'/f'{stem}.bmd6skl').read_bytes()

    def bones(self, stem='plasmarifle'):
        path = next((ROOT/'build/native-weapons-export').rglob(stem+'.cast'))
        return next(m for r in Cast.load(str(path)).Roots() for m in r.ChildrenOfType(Model)).Skeleton().Bones()

    def test_all_four_independent_native_hierarchies_match_exports(self):
        for stem in ('shotgun', 'double_barrel', 'heavy_ar', 'plasmarifle'):
            with self.subTest(stem=stem):
                report = checks.compare_cast(checks.parse_skeleton(self.raw(stem)), self.bones(stem))
                self.assertAlmostEqual(report['factor'], .5, places=8)
                self.assertLess(report['global_position_error'], .00001)

    def test_bad_headers_and_truncated_pose_fail_closed(self):
        for raw in (b'', b'wrong'+self.raw()[5:], self.raw()[:800]):
            with self.assertRaises(ValueError):
                checks.parse_skeleton(raw)

    def test_rocket_reference_verifies_all_38_joints(self):
        raw = (ROOT/'build/hand-workshop/rocket_launcher/native-reference/rocketlauncher.bmd6skl').read_bytes()
        report = checks.compare_cast(checks.parse_skeleton(raw), self.bones('rocketlauncher'))
        self.assertEqual(report['joint_count'], 38)
        self.assertAlmostEqual(report['factor'], .5, places=8)
        self.assertLess(report['global_position_error'], .00001)

    def test_gauss_reference_verifies_distinct_handle_joints(self):
        raw = (ROOT/'build/hand-workshop/gauss_cannon/native-reference/gauss_rifle.bmd6skl').read_bytes()
        native = checks.parse_skeleton(raw)
        report = checks.compare_cast(native, self.bones('gauss_rifle'))
        self.assertEqual(report['joint_count'], 36)
        self.assertAlmostEqual(report['factor'], .5, places=8)
        self.assertEqual(native['names'][6], 'handle_pivot_3_jnt')
        self.assertEqual(native['names'][2], 'back_grip_jnt')
        self.assertGreater(np.linalg.norm(native['worlds'][6,:3,3]-native['worlds'][2,:3,3]), 1.)

    def test_chaingun_reference_verifies_all_77_joints_and_handles(self):
        raw = (ROOT/'build/hand-workshop/chaingun/native-reference/chaingun.bmd6skl').read_bytes()
        native = checks.parse_skeleton(raw)
        report = checks.compare_cast(native, self.bones('chaingun'))
        self.assertEqual(report['joint_count'], 77)
        self.assertAlmostEqual(report['factor'], .5, places=8)
        self.assertEqual(native['names'][7], 'front_grip_jnt')
        self.assertEqual(native['names'][2], 'base_grip_adjust_jnt')
        self.assertLess(report['global_position_error'], .00001)

    def test_bfg_reference_verifies_56_joints_and_muzzle_parent(self):
        raw = (ROOT/'build/hand-workshop/bfg/native-reference/bfg.bmd6skl').read_bytes()
        native = checks.parse_skeleton(raw)
        report = checks.compare_cast(native, self.bones('bfg'))
        self.assertEqual(report['joint_count'], 56)
        self.assertAlmostEqual(report['factor'], .5, places=8)
        self.assertEqual(native['names'][1], 'base_jnt')
        np.testing.assert_allclose(native['worlds'][1,:3,:3], np.eye(3), atol=1e-5)
        self.assertLess(report['global_position_error'], .00001)

    def test_chainsaw_reference_verifies_body_and_separate_starter_handle(self):
        raw = (ROOT/'build/hand-workshop/chainsaw/native-reference/chainsaw.bmd6skl').read_bytes()
        native = checks.parse_skeleton(raw)
        report = checks.compare_cast(native, self.bones('chainsaw'))
        self.assertEqual(report['joint_count'], 64)
        self.assertAlmostEqual(report['factor'], .5, places=8)
        self.assertEqual(native['names'][1], 'root')
        self.assertEqual(native['names'][15], 'handle')
        self.assertGreater(np.linalg.norm(native['worlds'][15,:3,3]-native['worlds'][1,:3,3]), 1.)
        self.assertLess(report['global_position_error'], .00001)

    def test_pistol_reference_verifies_14_joints_and_fixed_grip(self):
        raw = (ROOT/'build/hand-workshop/pistol/native-reference/pistol.bmd6skl').read_bytes()
        native = checks.parse_skeleton(raw)
        report = checks.compare_cast(native, self.bones('pistol'))
        self.assertEqual(report['joint_count'], 14)
        self.assertAlmostEqual(report['factor'], .5, places=8)
        self.assertEqual(native['names'][1], 'base_jnt')
        self.assertEqual(native['names'][8], 'slide_jnt')
        self.assertLess(report['global_position_error'], .00001)

    def test_corrupted_inverse_bind_is_rejected(self):
        raw = bytearray(self.raw())
        offset = checks.parse_skeleton(raw)['offsets']['inverse_binds']
        struct.pack_into('>f', raw, offset+12, 100.)
        with self.assertRaises(ValueError):
            checks.parse_skeleton(raw)

    def test_parent_or_local_translation_mismatch_blocks_conversion(self):
        native = checks.parse_skeleton(self.raw())
        native['parents'][2] = 0
        with self.assertRaises(ValueError):
            checks.compare_cast(native, self.bones())
        native = checks.parse_skeleton(self.raw())
        native['local_positions'][3, 1] += 1.
        with self.assertRaises(ValueError):
            checks.compare_cast(native, self.bones())

    def test_scaled_hierarchy_is_not_silently_accepted(self):
        raw = bytearray(self.raw())
        struct.pack_into('>f', raw, 460, 2.)
        with self.assertRaises(ValueError):
            checks.parse_skeleton(raw)

    def test_wrong_rotation_cannot_be_hidden_by_matching_positions(self):
        native = checks.parse_skeleton(self.raw())
        native['local_quaternions'][3] = np.array([1.,0.,0.,0.])
        with self.assertRaises(ValueError):
            checks.compare_cast(native, self.bones())


if __name__ == '__main__':
    unittest.main()
