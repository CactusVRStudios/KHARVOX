"""Second, offline-only contact review using the exported native geometry.

No runtime assets, binaries, user calibration or original exports are written.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import itertools
import pathlib
import sys

sys.dont_write_bytecode = True
import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/praetor-placement-review-v2'
SKIN_VALIDATION = {}


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


review = module('review', ROOT / 'KHARVOX/tools/preview_praetor_weapon_grips.py')
builder = module('builder', ROOT / 'KHARVOX/tools/build_praetor_hands.py')
review.OUT = OUT


def native_poses():
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / 'build/praetor-imported.blend'))
    obj = next(o for o in bpy.context.scene.objects if o.type == 'MESH' and o.get('kharvox_native_hands'))
    rig = next(m.object for m in obj.modifiers if m.type == 'ARMATURE')
    if any(m.use_deform_preserve_volume for m in obj.modifiers if m.type == 'ARMATURE'):
        raise RuntimeError('Independent finger composition requires linear blend skinning')
    wrist, basis, _ = builder.landmarks(rig, 'left')
    builder.isolate(obj, rig, 'left')
    poses = {}
    mapping = None
    groups = None
    recipes = {
        'firm': dict(index=(40, 50, 25), middle=(38, 48, 25), ring=(34, 43, 23), pinky=(29, 38, 22), thumb=(15, 32, 20)),
        'medium': dict(index=(32, 42, 22), middle=(30, 40, 22), ring=(27, 36, 20), pinky=(23, 32, 18), thumb=(12, 28, 18)),
        'closed': dict(index=(46, 58, 28), middle=(44, 55, 28), ring=(40, 50, 25), pinky=(34, 45, 24), thumb=(18, 38, 24)),
        'wrap': dict(index=(20, 35, 30), middle=(18, 32, 25), ring=(12, 30, 25), pinky=(3, 25, 25), thumb=(10, 20, 10)),
        'wide': dict(index=(10, 32, 35), middle=(10, 30, 30), ring=(8, 28, 28), pinky=(0, 24, 25), thumb=(8, 18, 10)),
        'neutral': {f: ((0, 0, 2) if f == 'thumb' else (0, 9, 5)) for f in builder.FINGERS},
    }
    base_recipe = recipes['medium']
    for profile in ('firm', 'closed', 'wrap', 'wide'):
        for finger in builder.FINGERS:
            recipes[profile + ':' + finger] = dict(base_recipe, **{finger: recipes[profile][finger]})
    for angle in (-70, -50, -20, 0, 20):
        recipes['thumb@' + str(angle)] = dict(base_recipe, thumb=recipes['wrap']['thumb'])
    for finger in builder.FINGERS[:4]:
        for angles in itertools.product((-20, -5, 10, 25, 40), (15, 35, 55, 75), (0, 20, 40)):
            recipes['joint:' + finger + ':' + ':'.join(map(str, angles))] = dict(base_recipe, **{finger: angles})
    for angles in itertools.product((0, 15, 30), (10, 30, 50), (10, 30)):
        for abduction in (-70, -50, -30, -10, 10):
            recipes['joint:thumb:' + ':'.join(map(str, angles)) + '@' + str(abduction)] = dict(base_recipe, thumb=angles)
    for key, recipe in recipes.items():
        for finger, angles in recipe.items():
            first = rig.data.bones['lefthand' + finger + '1'].matrix_local
            second = rig.data.bones['lefthand' + finger + '2'].matrix_local
            sign = 1 if first.col[1].xyz.cross(second.col[1].xyz).dot(first.col[0].xyz) >= 0 else -1
            for segment, angle in enumerate(angles, 1):
                bone = rig.pose.bones['lefthand' + finger + str(segment)]
                bone.rotation_mode = 'QUATERNION'
                bone.rotation_quaternion = Quaternion((1, 0, 0), sign * math.radians(angle))
        bpy.context.view_layer.update()
        if key != 'neutral':
            thumb = rig.pose.bones['lefthandthumb1']
            axis = (rig.matrix_world @ thumb.matrix).to_3x3().inverted() @ Vector(basis[:, 2].tolist())
            abduction = int(key.rsplit('@', 1)[1]) if '@' in key else -35
            thumb.rotation_quaternion @= Quaternion(axis.normalized(), math.radians(abduction))
            bpy.context.view_layer.update()
        points, _, mapping = builder.bake(obj, np.eye(4), mapping)
        vertices, _, uvs, triangles = mapping
        if groups is None:
            groups = []
            for vertex in vertices:
                weights = obj.data.vertices[int(vertex)].groups
                name = obj.vertex_groups[max(weights, key=lambda g: g.weight).group].name if weights else ''
                groups.append(name)
        bone_points = {}
        for finger in builder.FINGERS:
            bones = [rig.pose.bones['lefthand' + finger + str(i)] for i in (1, 2, 3)]
            length = (bones[2].head - bones[1].head).length * .75
            world = [np.array(rig.matrix_world @ b.head) for b in bones]
            world.append(np.array(rig.matrix_world @ (bones[-1].head + bones[-1].matrix.col[1].xyz.normalized() * length)))
            bone_points[finger] = ((np.array(world) - wrist) @ basis).tolist()
        uvs = uvs.copy()
        uvs[:, 1] = 1 - uvs[:, 1]
        poses[key] = dict(points=(points - wrist) @ basis, triangles=triangles.reshape(-1, 3),
                          uvs=uvs, groups=groups, recipe=recipe, bone_points=bone_points)
    # Verify the independent-joint composition against an actual full rig
    # evaluation, instead of assuming skinning composition is exact.
    chosen = dict(index='firm', middle='closed', ring='wrap', pinky='wide', thumb='thumb@-50')
    expected = poses['medium']['points'].copy()
    for finger, profile in chosen.items():
        sample = poses[profile if profile.startswith('thumb@') else profile + ':' + finger]
        expected += sample['points'] - poses['medium']['points']
        first = rig.data.bones['lefthand' + finger + '1'].matrix_local
        second = rig.data.bones['lefthand' + finger + '2'].matrix_local
        sign = 1 if first.col[1].xyz.cross(second.col[1].xyz).dot(first.col[0].xyz) >= 0 else -1
        for segment, angle in enumerate(sample['recipe'][finger], 1):
            rig.pose.bones['lefthand' + finger + str(segment)].rotation_quaternion = Quaternion((1, 0, 0), sign * math.radians(angle))
    bpy.context.view_layer.update()
    thumb = rig.pose.bones['lefthandthumb1']
    axis = (rig.matrix_world @ thumb.matrix).to_3x3().inverted() @ Vector(basis[:, 2].tolist())
    thumb.rotation_quaternion @= Quaternion(axis.normalized(), math.radians(-50))
    bpy.context.view_layer.update()
    actual, _, _ = builder.bake(obj, np.eye(4), mapping)
    error = float(np.max(np.linalg.norm((actual - wrist) @ basis - expected, axis=1)))
    SKIN_VALIDATION['mixed_finger_pose_max_error_native_units'] = error
    if error > .0001:
        raise RuntimeError('Mixed finger pose does not reproduce the evaluated skeleton: ' + str(error))
    print('SKELETAL COMPOSITION ERROR', error, flush=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    return poses


def bvh_for(objects):
    vertices, faces = [], []
    for obj in objects:
        offset = len(vertices)
        vertices.extend(tuple(v.co) for v in obj.data.vertices)
        faces.extend(tuple(offset + i for i in poly.vertices) for poly in obj.data.polygons)
    return BVHTree.FromPolygons(vertices, faces, all_triangles=True, epsilon=.002)


def fit(spec, poses, objects):
    weapon_bvh = bvh_for(objects)
    base = poses['medium']
    centers = []
    for finger in ('index', 'middle', 'ring'):
        yz = np.array(base['bone_points'][finger])[:, 1:]
        centers.append(np.linalg.lstsq(np.c_[2 * yz, np.ones(4)], np.sum(yz * yz, axis=1), rcond=None)[0][:2])
    matrix = review.placement(spec, np.r_[0., np.median(centers, axis=0)])
    radius, y_min, y_max = spec['grip_geometry']
    center = np.array(spec['contact'])
    indices = {finger: np.array([i for i, group in enumerate(base['groups']) if group.startswith('lefthand' + finger)])
               for finger in builder.FINGERS}
    choices = {finger: [('medium', base)] + [(p, poses[p + ':' + finger]) for p in ('firm', 'closed', 'wrap', 'wide')]
               for finger in builder.FINGERS}
    choices['thumb'] += [(p, pose) for p, pose in poses.items() if p.startswith('thumb@')]
    def selected_pose(profile, finger):
        return poses[profile] if profile.startswith(('joint:', 'thumb@')) else poses[profile + ':' + finger]
    trials = []
    # All fitting remains offline. Each finger is selected independently from
    # real evaluated rotations; the production renderer never runs this search.
    for roll in (-60, -30, 0, 30, 60, 90, 120):
        rotation = np.array(Matrix.Rotation(math.radians(roll), 3, Vector(spec['radial'])))
        rotated = matrix.copy()
        rotated[:3, :3] = rotation @ matrix[:3, :3]
        rotated[:3, 3] = center + rotation @ (matrix[:3, 3] - center)
        for a in np.arange(-2.5, 2.51, .5):
            for b in np.arange(-2.5, 2.51, .5):
                candidate = rotated.copy()
                candidate[:3, 3] += rotated[:3, 1] * a + rotated[:3, 2] * b
                score, selected = 0., {}
                for finger in builder.FINGERS:
                    best_finger = None
                    for profile, pose in choices[finger]:
                        points = review.transformed(pose['points'][indices[finger]], candidate)
                        radial = np.linalg.norm(points[:, (0, 2)] - center[(0, 2),], axis=1) - radius
                        axial = np.maximum(y_min - points[:, 1], points[:, 1] - y_max)
                        inside = (radial < -.005) & (axial < 0)
                        gap = np.percentile(np.maximum(radial, axial), 10)
                        value = 100 * np.count_nonzero(inside) + float(gap * gap)
                        sample = points[::max(1, len(points) // 24)]
                        for point in sample:
                            nearest, normal, _, distance = weapon_bvh.find_nearest(Vector(point))
                            if normal.dot(Vector(point) - nearest) < -.01:
                                value += 100 + distance * distance
                        if best_finger is None or value < best_finger[0]:
                            best_finger = value, profile
                    score += best_finger[0]
                    selected[finger] = best_finger[1]
                trials.append((score, candidate, selected, (a, b, roll)))
    trials.sort(key=lambda row: row[0])
    def evaluate(matrix, selected, shift):
        local = base['points'].copy()
        recipe = dict(base['recipe'])
        for finger, profile in selected.items():
            if profile != 'medium':
                chosen = selected_pose(profile, finger)
                local += chosen['points'] - base['points']
                recipe[finger] = chosen['recipe'][finger]
        points = review.transformed(local, matrix)
        hand_bvh = BVHTree.FromPolygons(points.tolist(), base['triangles'].tolist(), all_triangles=True, epsilon=.002)
        pairs = weapon_bvh.overlap(hand_bvh)
        # Measure the actual fingertips/phalanges, not just the palm or sleeve.
        gaps = {}
        for finger in builder.FINGERS:
            sample = points[indices[finger]][::max(1, len(indices[finger]) // 80)]
            distances = [weapon_bvh.find_nearest(Vector(p))[3] for p in sample]
            gaps[finger] = float(np.percentile(distances, 10))
        score = len(pairs) * 1000 + sum(d * d for d in gaps.values())
        return score, local, recipe, dict(selected), matrix, len(pairs), gaps, shift
    best = min((evaluate(matrix, selected, shift) for _, matrix, selected, shift in trials[:100]), key=lambda row: row[0])
    # Refine against all actual weapon triangles, including the handle braces
    # which the analytic cylinder cannot represent. No joint or translation
    # fitting is added to the running game.
    dense_choices = {finger: choices[finger] + [(key, pose) for key, pose in poses.items() if key.startswith('joint:' + finger + ':')]
                     for finger in builder.FINGERS}
    for iteration in range(8):
        previous = best[0]
        for finger in builder.FINGERS:
            for profile, _ in dense_choices[finger]:
                selected = dict(best[3], **{finger: profile})
                candidate = evaluate(best[4], selected, best[7])
                if candidate[0] < best[0]:
                    best = candidate
        matrix = best[4].copy()
        for axis in range(3):
            for sign in (-1, 1):
                moved = matrix.copy()
                moved[:3, 3] += matrix[:3, axis] * sign * .25
                candidate = evaluate(moved, best[3], best[7])
                if candidate[0] < best[0]:
                    best = candidate
        if abs(best[0] - previous) < 1e-7:
            break
    _, local, recipe, selected, matrix, crossings, gaps, shift = best
    print('FIT', spec['stem'], selected, 'crossings', crossings, 'gaps', gaps, 'shift', shift, flush=True)
    points = review.transformed(local, matrix)
    pairs = weapon_bvh.overlap(BVHTree.FromPolygons(points.tolist(), base['triangles'].tolist(), all_triangles=True, epsilon=.002))
    from collections import Counter
    print('CROSSING GROUPS', Counter(base['groups'][int(base['triangles'][i][0])] for _, i in pairs), flush=True)
    pose = dict(base, points=local, recipe=recipe)
    if crossings or not np.isfinite(local).all() or not np.isfinite(matrix).all():
        raise RuntimeError('Final grip geometry validation failed')
    if not np.allclose(matrix[:3, :3].T @ matrix[:3, :3], np.eye(3), atol=1e-5):
        raise RuntimeError('Grip matrix must remain rigid')
    return pose, matrix, dict(profile=selected, joint_degrees=recipe,
                                    thumb_abduction_degrees=int(selected['thumb'].rsplit('@', 1)[1]) if '@' in selected['thumb'] else -35,
                                    hand_matrix=matrix.tolist(), crossing_triangle_pairs=crossings,
                                    finger_surface_gap_p10_native_units=gaps, fit_shift=shift)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--draft', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    OUT.mkdir(exist_ok=True)
    bpy.context.preferences.filepaths.save_version = 0
    prefs = bpy.context.preferences.addons['cycles'].preferences
    prefs.compute_device_type = 'CUDA'
    prefs.get_devices()
    for device in prefs.devices:
        device.use = device.type == 'CUDA'
    poses = native_poses()
    hand_material = review.texture_material('Praetor natif', review.HANDS / 'BaseColor.png', review.HANDS / 'Normal.png', review.HANDS / 'MetallicRoughness.png')
    audit = dict(status='OFFLINE_REVIEW_ONLY', units='native Cast units', weapons={}, validation=SKIN_VALIDATION)
    for key in ('gauss', 'chaingun'):
        spec = dict(review.SPECS[key])
        spec['contact'] = (1.677667, 32.5, 4.280817) if key == 'gauss' else (1.021606, 33.2, 7.996148)
        spec['grip_geometry'] = (1.87223, 28.76670, 40.13758) if key == 'gauss' else (1.74039, 24.553, 38.319599)
        spec['radial'] = (0, 1, 0) if key == 'gauss' else (0, -1, 0)
        spec['forward'] = (1, 0, 0)
        scene = bpy.data.scenes.new(spec['title'])
        bpy.context.window.scene = scene
        entry = {}
        objects, weapon_points = review.weapon(spec, scene.collection, entry)
        pose, matrix, result = fit(spec, poses, objects)
        hand = review.mesh_object('Main Praetor', pose['points'], pose['triangles'], pose['uvs'], hand_material, scene.collection)
        hand.matrix_world = Matrix(matrix.tolist())
        entry.update(result)
        entry.update(grip_axis=[0, 1, 0], selected_grip_point=list(spec['contact']),
                     cylinder_radius=spec['grip_geometry'][0], cylinder_axial_range=list(spec['grip_geometry'][1:]))
        audit['weapons'][key] = entry
        points = np.concatenate((weapon_points, review.transformed(pose['points'], matrix)))
        center, span = (points.min(0) + points.max(0)) / 2, float(np.ptp(points, axis=0).max())
        review.lighting(scene, center, span)
        review.render_view(scene, key, center, span, args.draft, spec['title'], 'Prise resserree autour de la poignee')
        review.render_view(scene, key + '-contact', np.array(spec['contact']), 42, args.draft, spec['title'], 'Doigts alignes sur le manche cylindrique', (.8, 1.2, -.4))
        review.render_view(scene, key + '-reverse', np.array(spec['contact']), 42, args.draft, spec['title'], 'Verification de la face opposee', (-.7, 1.3, .5))
    hud = module('hud_clearance', ROOT / 'KHARVOX/tools/preview_praetor_hud_clearance.py')
    audit['hud'] = hud.build_hud_review(review, draft=args.draft, render=True)
    (OUT / 'placements.json').write_text(json.dumps(audit, indent=2), encoding='utf-8')
    if not args.draft:
        review.contact_sheet(('gauss-contact', 'gauss-reverse', 'chaingun-contact', 'chaingun-reverse'), 2, 'comparaison-prises.png')
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'praetor-revision2.blend'))


if __name__ == '__main__':
    main()
