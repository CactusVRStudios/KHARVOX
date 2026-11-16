"""Regressions for export gating, independent of Blender and game sessions."""
import sys
sys.dont_write_bytecode = True
import importlib.util
from pathlib import Path
import unittest
import numpy as np

spec = importlib.util.spec_from_file_location('grip_checks', Path(__file__).resolve().parents[1]/'tools/weapon_grip_checks.py')
checks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checks)


class GripChecksTests(unittest.TestCase):
    def fixture(self, mode='authored_grip', **changes):
        data = dict(pivot=[-.05,0,0], grip=[.4,0,0], primary_grip=[0,0,0],
            forward=[1,0,0], up=[0,0,1], radius=.2, target_mode=mode)
        data.update(changes)
        return checks.check_geometry(**data)

    def test_animation_pivot_can_stay_distant_when_runtime_uses_actual_grip(self):
        report = self.fixture()
        self.assertTrue(report['passed'])
        self.assertAlmostEqual(report['pivot_to_grip_m'], .45)
        self.assertEqual(report['capture_error_m'], 0)
        self.assertTrue(all(case['forward_dot'] > .99 for case in report['orientation_cases']))

    def test_old_pivot_capture_is_blocked(self):
        report = self.fixture('native_anchor')
        self.assertFalse(report['passed'])
        self.assertGreater(report['capture_error_m'], report['capture_radius_m'])

    def test_reversed_stock_aim_is_blocked_even_inside_capture_radius(self):
        report = self.fixture('native_anchor', radius=.5)
        self.assertLess(report['capture_error_m'], report['capture_radius_m'])
        self.assertTrue(any(case['forward_dot'] < 0 for case in report['orientation_cases']))
        self.assertTrue(any('retourne' in error for error in report['errors']))

    def test_matching_pivot_and_real_grip_remains_valid(self):
        self.assertTrue(self.fixture('native_anchor', pivot=[.4,0,0])['passed'])

    def test_degenerate_aim_or_coincident_hands_cannot_pass(self):
        self.assertFalse(self.fixture(grip=[0,0,0])['passed'])
        with self.assertRaises(ValueError):
            self.fixture(forward=[0,0,0])
        with self.assertRaises(ValueError):
            self.fixture(grip=[float('nan'),0,0])

    def test_new_unintegrated_weapon_does_not_silently_gain_authored_target(self):
        self.assertEqual(checks.runtime_target_mode('heavy_assault_rifle'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('plasma_rifle'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('rocket_launcher'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('gauss_cannon'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('chaingun'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('bfg'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('chainsaw'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('pistol'), 'authored_grip')
        self.assertEqual(checks.runtime_target_mode('next_weapon'), 'native_anchor')
        self.assertFalse(self.fixture(checks.runtime_target_mode('next_weapon'))['passed'])

    def test_rigid_change_of_reference_preserves_result(self):
        rotation = np.array(((0,-1,0),(1,0,0),(0,0,1)))
        offset = np.array((1,2,3))
        report = self.fixture(pivot=rotation@np.array([-.05,0,0])+offset,
            grip=rotation@np.array([.4,0,0])+offset, primary_grip=offset,
            forward=rotation@np.array([1,0,0]), up=rotation@np.array([0,0,1]))
        self.assertTrue(report['passed'])
        self.assertAlmostEqual(report['pivot_to_grip_m'], .45)

    def test_side_handle_roll_never_reverses_barrel(self):
        report = self.fixture(grip=[0,.4,0], aim_mode='side-grip')
        self.assertTrue(report['passed'])
        self.assertEqual(report['orientation_cases'][0]['forward_dot'], 1.)

    def test_relative_pistol_grip_respects_primary_rotation_translation_and_units(self):
        rotation = np.array(((0,-1,0),(1,0,0),(0,0,1)))
        primary = np.array((2.,3.,4.))
        support = primary+rotation@np.array((.12,-.03,.08))
        np.testing.assert_allclose(checks.controller_relative_support(support,primary,rotation*7),(.12,-.03,.08))

    def test_mirrored_sheared_or_missing_controller_frame_blocks_new_profile(self):
        for basis in (np.diag((-1,1,1)),np.zeros((3,3)),np.array(((1,.2,0),(0,1,0),(0,0,1)))):
            with self.assertRaises(ValueError):
                checks.controller_relative_support((.1,0,0),(0,0,0),basis)


if __name__ == '__main__':
    unittest.main()
