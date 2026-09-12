import importlib.util
from pathlib import Path
import unittest
import math

spec = importlib.util.spec_from_file_location('pose_analysis', Path(__file__).parents[1]/'tools/analyze_pose_trace.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def event(kind, data=None, **kwargs):
    row = dict(kind=kind, qpc=100, eye=0, poseId=7, thread=1, source=10, revision=1, flags=1, status=1)
    row.update(kwargs)
    for i, value in enumerate((data or [])+[0.]*(16-len(data or []))):
        row['d'+str(i)] = value
    return row


class AnalysisTests(unittest.TestCase):
    def test_cinematic_cache_uses_completed_eye_not_schedule(self):
        wrong=self.analyze([event(3,flags=262,poseId=988,eye=0),event(15,status=6,poseId=988,eye=0),
                            event(8),event(4,poseId=990,eye=1)])
        self.assertEqual(wrong['cinematic_producer_cache_observations'],1)
        self.assertEqual(wrong['cinematic_producer_cache_eye_mismatches'],1)
        self.assertEqual(wrong['cinematic_producer_cache_pose_mismatches'],1)
        correct=self.analyze([event(3,flags=262,poseId=988,eye=0),event(15,status=6,poseId=988,eye=0),
                              event(8),event(4,poseId=988,eye=0)])
        self.assertEqual(correct['cinematic_producer_cache_eye_mismatches'],0)
        self.assertEqual(correct['cinematic_producer_cache_pose_mismatches'],0)
        transition=self.analyze([event(3,flags=5,poseId=988,eye=1),event(15,status=6,poseId=988,eye=1),
                                event(8),event(3,flags=262,poseId=988,eye=1),event(4,poseId=988,eye=1)])
        self.assertEqual(transition['cinematic_producer_cache_observations'],0)

    def test_draw_model_status_and_cinematic_domains(self):
        d=[0.]*16;d[12]=8.5
        result=self.analyze([event(24,d,status=2),event(24,status=3),event(24,status=6),event(18,flags=1),event(18,flags=2)])
        self.assertEqual(result['weapon_draw_model_status_counts'],{2:1,3:1,6:1})
        self.assertEqual(result['weapon_draw_cached_matrix_delta_max'],8.5)
        self.assertEqual(result['aer_source_domain_counts'],{1:1,2:1})
        self.assertFalse(result['gpu_image_provenance_verified'])

    def test_anatomical_baseline_sign_and_native_exclusion(self):
        left = [0., .032, 0., 1., 0., 0., 0., 1., 0., 0., 0., 1.]
        right = left[:]; right[1] = -.032
        result = self.analyze([event(3, right, eye=1, flags=5), event(3, left, eye=0, flags=5),
                               event(3, left, eye=1, flags=5, poseId=9), event(3, right, eye=0, flags=5, poseId=9),
                               event(3, left, eye=1), event(3, right, eye=0)])
        self.assertEqual(result['aer_anatomical_eye_pairs'], 2)
        self.assertEqual(result['aer_reversed_eye_baseline_pairs'], 1)
        self.assertAlmostEqual(result['aer_eye_baseline_left_axis_units_min'], -.064)
        self.assertAlmostEqual(result['aer_eye_baseline_left_axis_units_max'], .064)

    def test_controller_head_turn_frame_mismatch_is_detected(self):
        result = self.analyze([event(23, [2., 5., 10., 2.]), event(23, [-3., -7., 12., -3.])])
        self.assertEqual(result['controller_turn_frame_samples'], 2)
        self.assertEqual(result['controller_vs_head_turn_deg_max'], 0.)
        self.assertEqual(result['pending_vs_published_turn_deg_max'], 4.)
        result = self.analyze([event(23, [5., 5., 10., 2.])])
        self.assertEqual(result['controller_vs_head_turn_deg_max'], 3.)

    def test_weapon_pairs_match_source_identity_not_scheduled_phase(self):
        pose = [1., 2., 3., 1., 0., 0., 0., 1., 0., 0., 0., 1.]
        result = self.analyze([event(3, eye=1), event(20, pose, eye=1),
                               event(2, eye=0, poseId=9), event(3, eye=0),
                               event(20, pose, eye=0, status=2), event(21, status=0)])
        self.assertEqual(result['weapon_source_matched_pairs'], {20:1})
        self.assertEqual(result['weapon_source_max_pair_component_error'][20], 0.)
        self.assertEqual(result['weapon_source_without_observed_camera'], 0)
        self.assertEqual(result['weapon_source_prop_status_counts'], {0:1})
        self.assertFalse(result['gpu_image_provenance_verified'])

    def test_weapon_changed_pair_is_detected(self):
        result = self.analyze([event(20, [1.], eye=1), event(20, [2.], eye=0, status=2)])
        self.assertEqual(result['weapon_source_max_pair_component_error'][20], 1.)
        self.assertEqual(result['weapon_source_without_observed_camera'], 2)

    def test_source_binding_is_not_gpu_proof(self):
        result = self.analyze([event(15,status=6),event(18,status=1),event(19,status=1),
                               event(18,status=2),event(19,status=0)])
        self.assertEqual(result['world_producer_status_counts'], {6:1})
        self.assertEqual(result['aer_source_window_status_counts'], {1:1,2:1})
        self.assertEqual(result['aer_source_resolved_status_counts'], {1:1,0:1})
        self.assertFalse(result['gpu_image_provenance_verified'])

    def test_world_producer_updates_are_not_gpu_proof(self):
        result = self.analyze([event(15,status=3),event(16,status=3),event(17,status=3),
                               event(15,status=1),event(15,status=-1)])
        self.assertEqual(result['world_producer_status_counts'], {3:1,1:1,-1:1})
        self.assertFalse(result['gpu_image_provenance_verified'])

    def analyze(self, rows):
        return module.analyze(rows, dict(qpcFrequency=1000, dropped=0))

    def test_quaternion_sign_does_not_move_quad(self):
        result = self.analyze([event(12, [0,0,-2,0,0,0,1]), event(12, [0,0,-2,0,0,0,-1])])
        self.assertEqual(result['fixed_quads'][0]['max_rotation_delta_deg'], 0)

    def test_missing_camera_is_not_proof(self):
        result = self.analyze([event(4)])
        self.assertEqual(result['cache_without_matching_cpu_camera_candidate'], 1)
        self.assertFalse(result['gpu_image_provenance_verified'])

    def test_matrix_error_and_body_offset_detected(self):
        basis = [0,0,0,1,0,0,0,1,0,0,0,1]
        changed = basis[:]; changed[3] = 0.5
        anchor = basis[:]; anchor[0] = 2
        result = self.analyze([event(9), event(10, basis), event(13, anchor), event(3, changed)])
        self.assertEqual(result['camera_matrix_pairs'], 1)
        self.assertEqual(result['max_matrix_component_error'], 0.5)
        self.assertEqual(result['max_camera_base_vs_stable_body_units'], 2)

    def test_pose_id_mismatch_is_unmatched(self):
        pose = [0,0,0,0,0,0,1]
        result = self.analyze([event(4, pose), event(5, pose, poseId=8, flags=0)])
        self.assertEqual(result['unmatched_aer_submissions'], 1)
        self.assertIsNone(result['max_cache_submit_rotation_delta_deg'])

    def test_matching_chain(self):
        pose = [0,0,0,0,0,0,1]
        rows = [event(2, qpc=10), event(3, qpc=20), event(4, pose, qpc=30), event(5, pose, qpc=40, flags=0)]
        result = self.analyze(rows)
        self.assertEqual(result['cache_with_matching_cpu_camera_candidate'], 1)
        self.assertEqual(result['matched_aer_cache_submissions'], 1)
        self.assertEqual(result['max_cache_submit_position_delta_m'], 0)

    def test_pure_pitch_matrix(self):
        c, s = math.cos(math.pi/6), math.sin(math.pi/6)
        basis = [0,0,0,1,0,0,0,1,0,0,0,1]
        pitched = [0,0,0,c,0,s,0,1,0,-s,0,c]
        result = self.analyze([event(9, [0,30,0]), event(10, basis), event(3, pitched)])
        self.assertLess(result['max_matrix_component_error'], 1e-12)

    def test_moving_quad_detected(self):
        result = self.analyze([event(12, [0,0,-2,0,0,0,1]),
                               event(12, [0,0,-1,math.sin(math.pi/12),0,0,math.cos(math.pi/12)])])
        quad = result['fixed_quads'][0]
        self.assertEqual(quad['max_position_delta_m'], 1)
        self.assertAlmostEqual(quad['max_rotation_delta_deg'], 30)


if __name__ == '__main__':
    unittest.main()
