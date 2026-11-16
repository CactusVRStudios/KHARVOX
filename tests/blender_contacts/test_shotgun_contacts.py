"""Synthetic collision regressions and checks of the saved, single-weapon scene."""
import sys
sys.dont_write_bytecode = True
import hashlib
import importlib.util
import json
import pathlib
import unittest

import bpy
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('shotgun_contacts',
    ROOT / 'tools/prototype_shotgun_contacts.py')
contact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(contact)


def cube(scale=1):
    vertices = np.array([(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
                         (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)], dtype=float) * scale
    faces = np.array([(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
                      (0, 1, 5), (0, 5, 4), (3, 7, 6), (3, 6, 2),
                      (0, 4, 7), (0, 7, 3), (1, 2, 6), (1, 6, 5)])
    return vertices, faces


def acceptable():
    return dict(crossing_triangle_pairs=0,
                gaps_cm={name: .2 for name in contact.NAMES},
                inside_weapon={name: False for name in contact.NAMES})


class CollisionRegressions(unittest.TestCase):
    def test_volume_detects_inside_and_outside(self):
        weapon = contact.tree(*cube())
        self.assertIs(contact.inside((0, 0, 0), weapon), True)
        for point in ((2, 0, 0), (-2, 0, 0), (0, 0, 2)):
            self.assertIs(contact.inside(point, weapon), False)

    def test_six_points_do_not_replace_triangle_collisions(self):
        weapon = contact.tree(*cube())
        triangle = np.array([(-2, 0, 0), (2, 0, 0), (0, 3, 0)], dtype=float)
        self.assertTrue(all(contact.inside(p, weapon) is False for p in triangle))
        self.assertGreater(len(weapon.overlap(contact.tree(triangle, np.array([(0, 1, 2)])))), 0)

    def test_containment_without_triangle_crossings_is_rejected(self):
        hand_vertices, hand_faces = cube(.1)
        result = contact.metrics(hand_vertices, hand_faces, hand_vertices[:6],
                                 np.eye(4), contact.tree(*cube()))
        self.assertEqual(result['crossing_triangle_pairs'], 0)
        self.assertIn('A contact marker is inside the weapon', contact.contact_rejection_reasons(result))

    def test_floating_hand_is_not_accepted(self):
        result = acceptable()
        result['gaps_cm']['PALM'] = 3.1
        self.assertTrue(contact.contact_rejection_reasons(result))

    def test_ambiguous_volume_is_not_accepted(self):
        result = acceptable()
        result['inside_weapon']['INDEX_TIP'] = None
        self.assertTrue(contact.contact_rejection_reasons(result))

    def test_large_fingertip_gap_is_not_accepted(self):
        result = acceptable()
        result['gaps_cm']['PINKY_TIP'] = 4
        self.assertTrue(contact.contact_rejection_reasons(result))

    def test_known_valid_contacts_pass_gate(self):
        self.assertEqual(contact.contact_rejection_reasons(acceptable()), [])


class SavedPrototypeRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = contact.OUT / 'prototype.blend'
        if not path.exists():
            raise unittest.SkipTest('Local native exports/prototype not available')
        cls.report = json.loads((contact.OUT / 'rapport.json').read_text())
        bpy.ops.wm.open_mainfile(filepath=str(path))
        bpy.context.view_layer.update()
        cls.scene = bpy.context.scene

    def test_one_weapon_and_twelve_parented_markers(self):
        self.assertEqual(len(bpy.data.scenes), 1)
        self.assertEqual(sum(o.type == 'MESH' and o.name.startswith('shotgun_')
                             for o in self.scene.objects), 2)
        markers = [o for o in self.scene.objects if o.name.startswith('CONTACT_')]
        self.assertEqual(len(markers), 12)
        for side in contact.SIDES:
            for name in contact.NAMES:
                marker = self.scene.objects['CONTACT_' + side.upper() + '_' + name]
                self.assertIsNotNone(marker.parent)
                self.assertEqual(marker.parent.type, 'MESH')
                point = self.report['hands'][side]['markers_local'][name]
                self.assertTrue(np.allclose(marker.location[:], point, atol=1e-5))
                expected = contact.transform(np.array([point]), np.array(marker.parent.matrix_world))[0]
                self.assertTrue(np.allclose(marker.matrix_world.translation[:], expected, atol=2e-5))

    def test_mesh_and_scale_preserved(self):
        for side, entry in self.report['hands'].items():
            hand = self.scene.objects['CONTACT_' + side.upper() + '_PALM'].parent
            raw = np.array([v.co[:] for v in hand.data.vertices])
            self.assertEqual(hashlib.sha256(raw.tobytes()).hexdigest(), entry['mesh_sha256'])
            before, after = [np.array(entry[key]['matrix_cm']) for key in ('before', 'after')]
            self.assertTrue(np.allclose(before[:3, :3].T @ before[:3, :3],
                                        after[:3, :3].T @ after[:3, :3], atol=2e-5))
            self.assertGreater(np.linalg.det(after[:3, :3]), 0)

    def test_all_markers_are_on_actual_hand_skin(self):
        for side in contact.SIDES:
            hand = self.scene.objects['CONTACT_' + side.upper() + '_PALM'].parent
            hand.data.calc_loop_triangles()
            geometry = contact.tree(np.array([v.co[:] for v in hand.data.vertices]),
                np.array([t.vertices[:] for t in hand.data.loop_triangles]))
            for point in self.report['hands'][side]['markers_local'].values():
                self.assertLess(geometry.find_nearest(contact.Vector(point))[3], .0001)

    def test_clearance_report_matches_saved_geometry(self):
        vertices, triangles = [], []
        for obj in self.scene.objects:
            if obj.type != 'MESH' or not obj.name.startswith('shotgun_'):
                continue
            offset = len(vertices)
            obj.data.calc_loop_triangles()
            vertices.extend(contact.transform(np.array([v.co[:] for v in obj.data.vertices]),
                                              np.array(obj.matrix_world)).tolist())
            triangles.extend(tuple(offset + v for v in t.vertices) for t in obj.data.loop_triangles)
        weapon = contact.tree(np.array(vertices), np.array(triangles))
        for side, entry in self.report['hands'].items():
            hand = self.scene.objects['CONTACT_' + side.upper() + '_PALM'].parent
            hand.data.calc_loop_triangles()
            raw = np.array([v.co[:] for v in hand.data.vertices])
            placed = contact.transform(raw, np.array(hand.matrix_world))
            faces = np.array([t.vertices[:] for t in hand.data.loop_triangles])
            self.assertEqual(len(weapon.overlap(contact.tree(placed, faces))),
                             entry['after']['crossing_triangle_pairs'])

    def test_source_scenes_and_runtime_package_preserved(self):
        self.assertEqual(contact.digest(contact.SOURCE), self.report['source_scene_sha256'])
        self.assertEqual(contact.digest(contact.CORRECTED), self.report['preserved_left_sha256'])
        runtime = contact.ROOT / 'build/pump-review-v3/runtime-fixture'
        for name, expected in self.report['runtime_preserved_sha256'].items():
            self.assertEqual(contact.digest(runtime / name), expected)

    def test_report_never_marks_failed_contact_as_valid(self):
        for entry in self.report['hands'].values():
            self.assertEqual(entry['rejection_reasons'], contact.contact_rejection_reasons(entry['after']))
            self.assertEqual(entry['contact_accepted'], not entry['rejection_reasons'])
        if not self.report['geometry_contact_accepted']:
            self.assertEqual(self.report['status'], 'CONTACT_REJECTED_NO_RUNTIME_EXPORT')
            self.assertFalse(self.report['runtime_export_written'])


if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__]))
    if not result.wasSuccessful():
        raise RuntimeError('Offline shotgun contact regressions failed')
