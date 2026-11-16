"""Bounded, offline rigid contact fitting of the existing native shotgun scene.

Run with Blender --background --factory-startup --python-exit-code 1 --python FILE.
No VR/HUD/menu source, existing scene, finger pose or hand scale is changed.
"""
import sys
sys.dont_write_bytecode = True
import hashlib
import itertools
import json
import math
import pathlib
import re
import time

import bpy
import numpy as np
from mathutils import Euler, Matrix, Quaternion, Vector
from mathutils.bvhtree import BVHTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/shotgun-contact-prototype'
sys.path.insert(0, str(ROOT / 'KHARVOX/tools'))
import preview_praetor_pumps_v3 as pumps
review = pumps.review
builder = pumps.reading.module('contact_native_builder', 'build_praetor_hands.py')
SOURCE = ROOT / 'build/pump-review-v4/shotgun-deux-mains.blend'
NATIVE = ROOT / 'build/praetor-imported.blend'
CORRECTED = ROOT / 'build/pump-review-v4/propositions-v4.blend'
CORRECTIONS = ROOT / 'build/pump-review-v4/propositions-v4.json'
SIDES = ('left', 'right')
NAMES = ('PALM', 'INDEX_TIP', 'MIDDLE_TIP', 'RING_TIP', 'PINKY_TIP', 'THUMB_TIP')
DEADLINE = time.monotonic() + 240
EPSILON_CM = .002


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def transform(points, matrix):
    return points @ matrix[:3, :3].T + matrix[:3, 3]


def tree(points, faces):
    return BVHTree.FromPolygons(points.tolist(), faces.tolist(),
                               all_triangles=True, epsilon=EPSILON_CM)


def native_markers(side):
    bpy.ops.wm.open_mainfile(filepath=str(NATIVE))
    obj = next(o for o in bpy.context.scene.objects
               if o.type == 'MESH' and o.get('kharvox_native_hands'))
    rig = next(m.object for m in obj.modifiers if m.type == 'ARMATURE')
    wrist, basis, _ = builder.landmarks(rig, side)
    builder.isolate(obj, rig, side)
    builder.pose(rig, side, {f: 'gun' if side == 'left' else 'closed'
                             for f in builder.FINGERS})
    if side == 'left':
        # Reconstruct the saved pose only to locate bones on its unchanged mesh.
        recipe = json.loads(CORRECTIONS.read_text())['weapons']['shotgun']['finger_closure']
        for finger in builder.FINGERS:
            first = rig.data.bones['lefthand' + finger + '1'].matrix_local
            second = rig.data.bones['lefthand' + finger + '2'].matrix_local
            sign = 1 if first.col[1].xyz.cross(second.col[1].xyz).dot(first.col[0].xyz) >= 0 else -1
            angles = recipe[3:6] if finger == 'thumb' else recipe[:3]
            for segment, angle in enumerate(angles, 1):
                bone = rig.pose.bones['lefthand' + finger + str(segment)]
                bone.rotation_quaternion = Quaternion((1, 0, 0), sign * math.radians(angle))
        bpy.context.view_layer.update()
        thumb = rig.pose.bones['lefthandthumb1']
        axis = (rig.matrix_world @ thumb.matrix).to_3x3().inverted() @ Vector(basis[:, 2].tolist())
        thumb.rotation_quaternion @= Quaternion(axis.normalized(), math.radians(recipe[6]))
        bpy.context.view_layer.update()
    points, _, mapping = builder.bake(obj, np.eye(4))
    local = (points - wrist) @ basis
    groups = []
    for index in mapping[0]:
        weights = obj.data.vertices[int(index)].groups
        groups.append(obj.vertex_groups[max(weights, key=lambda g: g.weight).group].name)
    groups = np.array(groups)
    roots, tips = [], []
    for finger in builder.FINGERS:
        first = rig.pose.bones[side + 'hand' + finger + '1']
        last = rig.pose.bones[side + 'hand' + finger + '3']
        roots.append(np.array(rig.matrix_world @ first.head))
        direction = np.array(rig.matrix_world.to_3x3() @ last.matrix.col[1].xyz)
        direction = direction @ basis
        direction /= np.linalg.norm(direction)
        candidates = np.flatnonzero(groups == side + 'hand' + finger + '3')
        if not len(candidates):
            raise RuntimeError('Missing native distal phalanx: ' + side + finger)
        # Tip is on the actual skin, using the native skeleton's distal axis.
        tips.append(local[candidates[np.argmax(local[candidates] @ direction)]])
    centre = ((np.mean(roots[:4], axis=0) - wrist) @ basis) * .58
    palm_side = -1 if side == 'left' else 1
    origin = centre.copy()
    origin[2] += palm_side * 40
    direction = Vector((0, 0, -palm_side))
    hit = tree(local, mapping[3].reshape(-1, 3)).ray_cast(Vector(origin), direction, 80)[0]
    if hit is None:
        raise RuntimeError('Palm ray does not meet the native hand')
    return local, np.array([tuple(hit), *tips]), groups


def inside(point, weapon_tree):
    # Three parity rays; disagreement is explicitly reported as unknown.
    votes = []
    for raw in ((1, .231, .417), (.317, 1, .619), (.571, .193, 1)):
        direction = Vector(raw).normalized()
        origin = Vector(point)
        count = 0
        for _ in range(256):
            location, normal, index, distance = weapon_tree.ray_cast(origin, direction, 1000)
            if location is None:
                break
            if distance > .0001:
                count += 1
            origin = location + direction * .0002
        votes.append(count % 2 == 1)
    return votes[0] if all(v == votes[0] for v in votes) else None


def metrics(raw, faces, markers, matrix, weapon_tree):
    placed = transform(raw, matrix)
    contact = transform(markers, matrix)
    pairs = len(weapon_tree.overlap(tree(placed, faces)))
    gaps = [float(weapon_tree.find_nearest(Vector(p))[3]) for p in contact]
    containment = [inside(p, weapon_tree) for p in contact]
    return dict(crossing_triangle_pairs=pairs,
                gaps_cm=dict(zip(NAMES, gaps)),
                inside_weapon=dict(zip(NAMES, containment)),
                matrix_cm=matrix.tolist())


def contact_rejection_reasons(result):
    reasons = []
    if result['crossing_triangle_pairs']:
        reasons.append('Intersecting mesh triangles')
    if result['gaps_cm']['PALM'] > .75:
        reasons.append('Palm marker more than 7.5 mm from the weapon')
    if max(v for k, v in result['gaps_cm'].items() if k != 'PALM') > 3:
        reasons.append('At least one fingertip more than 3 cm from the weapon')
    if any(value is True for value in result['inside_weapon'].values()):
        reasons.append('A contact marker is inside the weapon')
    if any(value is None for value in result['inside_weapon'].values()):
        reasons.append('Volume classification ambiguous on exported weapon surfaces')
    return reasons


def finalize_report():
    report = json.loads((OUT / 'rapport.json').read_text())
    for entry in report['hands'].values():
        entry['rejection_reasons'] = contact_rejection_reasons(entry['after'])
        entry['contact_accepted'] = not entry['rejection_reasons']
    report['geometry_contact_accepted'] = all(e['contact_accepted'] for e in report['hands'].values())
    report['status'] = ('OFFLINE_CONTACT_ACCEPTED_GAME_VALIDATION_REQUIRED'
                        if report['geometry_contact_accepted'] else 'CONTACT_REJECTED_NO_RUNTIME_EXPORT')
    report['runtime_export_written'] = False
    report['quality_limits'] = dict(max_palm_gap_cm=.75, max_fingertip_gap_cm=3,
                                   crossing_triangle_pairs=0, ambiguous_volume_allowed=False)
    report['runtime_preserved_sha256'] = {
        name: digest(ROOT / 'build/pump-review-v3/runtime-fixture' / name)
        for name in ('KharvoxLayer.dll', 'support_hands.cfg', 'offhand_hud.cfg', 'KharvoxDesktop.exe')
        if (ROOT / 'build/pump-review-v3/runtime-fixture' / name).exists()}
    report['facts'] = [
        '12 reperes lies aux vrais maillages Praetor gauche/droit.',
        'Ajustement rigide : topologie, UV, echelle et pose des doigts conservees.',
        'Maillage gauche et transformation initiale repris de la scene corrigee V4.',
        'Aucun HUD, menu, code de rendu VR ou executable existant modifie.',
        ('Contact hors jeu accepte, validation casque requise.' if report['geometry_contact_accepted'] else
         'La recherche bornee ne trouve pas de prise a deux mains acceptable ; aucun export en jeu.')]
    report['hypotheses_not_proven'] = [
        'Les poses fixes pourraient necessiter une articulation des doigts pour entourer cette arme.',
        'Cet essai ne prouve pas que tout autre placement rigide soit impossible.',
        'Des surfaces exportees ouvertes, superposees ou non-manifold peuvent expliquer les tests de volume ambigus.']
    validation = {}
    runtime_log = OUT / 'ctest-runtime.log'
    if runtime_log.exists():
        match = re.search(r'(\d+)% tests passed, (\d+) tests failed out of (\d+)', runtime_log.read_text())
        if match:
            validation['existing_ctest'] = dict(percent=int(match[1]), failed=int(match[2]), total=int(match[3]))
    offline_log = OUT / 'ctest/Testing/Temporary/LastTest.log'
    if offline_log.exists():
        log = offline_log.read_text()
        match = re.search(r'Ran (\d+) tests in ([\d.]+)s', log)
        if match:
            validation['blender_regressions'] = dict(tests=int(match[1]), seconds=float(match[2]),
                                                     passed='\nOK\n' in log.replace('\r', ''))
    report['validation'] = validation
    (OUT / 'rapport.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    lines = ['# Prototype contacts / fusil a pompe standard', '',
             ('**Contact accepte hors jeu ; validation casque requise.**' if report['geometry_contact_accepted'] else
              '**Export refuse : la prise de l arme ne satisfait pas les controles de contact.**'), '',
             'Faits observes :', *['- ' + fact for fact in report['facts']], '',
             '| Main | Croisements avant / apres | Ecart paume | Ecart maximal doigt |',
             '|---|---:|---:|---:|']
    for side, entry in report['hands'].items():
        gaps = entry['after']['gaps_cm']
        lines.append(f"| {side} | {entry['before']['crossing_triangle_pairs']} / "
            f"{entry['after']['crossing_triangle_pairs']} | {gaps['PALM']:.2f} cm | "
            f"{max(v for k, v in gaps.items() if k != 'PALM'):.2f} cm |")
    lines += ['', 'Hypotheses / limites :',
              *['- ' + value for value in report['hypotheses_not_proven']],
              *['- ' + value for value in report['limitations']], '',
              'Reproduction :',
              '1. Lancer Blender en arriere-plan avec tools/prototype_shotgun_contacts.py.',
              '2. Inspecter prototype.blend : CONTACT_LEFT_* et CONTACT_RIGHT_* suivent leurs mains.',
              '3. Lire rapport.json avant tout export : le statut actuel interdit la livraison en jeu.',
              '4. Executer les tests CMake/CTest de tests/blender_contacts.', '',
              'Validation executee : ' + json.dumps(validation), '',
              'Une seule arme traitee. Aucun profil generalise. Aucune validation casque revendiquee.']
    (OUT / 'rapport.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(report['status'], flush=True)


def fit(raw, faces, markers, initial, weapon_tree):
    pivot = initial[:3, 3].copy()
    # Fixed topology, finger pose, scale and chirality; only one rigid transform.
    cache = {}
    def evaluate(parameters):
        key = tuple(np.round(parameters, 5))
        if key in cache:
            return cache[key]
        if np.linalg.norm(parameters[:3]) > 8 or np.linalg.norm(parameters[3:]) > 35:
            return (float('inf'), None)
        rotation = np.array(Euler(tuple(np.deg2rad(parameters[3:])), 'XYZ').to_matrix())
        matrix = initial.copy()
        matrix[:3, :3] = rotation @ initial[:3, :3]
        matrix[:3, 3] = pivot + parameters[:3]
        placed = transform(raw, matrix)
        points = transform(markers, matrix)
        pairs = len(weapon_tree.overlap(tree(placed, faces)))
        gaps = np.array([weapon_tree.find_nearest(Vector(p))[3] for p in points])
        # Collision-free candidates always outrank colliding ones in this range.
        contact_cost = 3 * gaps[0] ** 2 + np.mean(np.minimum(gaps[1:], 12) ** 2)
        movement_cost = .06 * np.sum(parameters[:3] ** 2) + .001 * np.sum(parameters[3:] ** 2)
        palm_penalty = 150 * max(0, gaps[0] - .5) ** 2
        score = 10000 * min(pairs, 10000) + palm_penalty + contact_cost + movement_cost
        cache[key] = score, matrix
        return score, matrix
    best = np.zeros(6)
    baseline = evaluate(best)[0]
    for translation in itertools.product((-4., 0., 4.), repeat=3):
        for axis, angle in ((0, 0), (0, -15), (0, 15), (1, -15),
                            (1, 15), (2, -15), (2, 15)):
            if time.monotonic() > DEADLINE:
                break
            candidate = np.array([*translation, 0., 0., 0.])
            candidate[3 + axis] = angle
            if evaluate(candidate)[0] < evaluate(best)[0]:
                best = candidate
    for translation_step, angle_step in ((2, 8), (1, 4), (.5, 2), (.25, 1), (.125, .5)):
        for _ in range(3):
            previous = best.copy()
            for axis in range(6):
                for sign in (-1, 1):
                    if time.monotonic() > DEADLINE:
                        break
                    candidate = best.copy()
                    candidate[axis] += sign * (translation_step if axis < 3 else angle_step)
                    if evaluate(candidate)[0] < evaluate(best)[0]:
                        best = candidate
            if np.array_equal(previous, best):
                break
    score, matrix = evaluate(best)
    return matrix, dict(evaluations=len(cache), translation_cm=best[:3].tolist(),
                        rotation_degrees_xyz=best[3:].tolist(), score_before=baseline,
                        score_after=score, time_limit_reached=time.monotonic() > DEADLINE)


def main():
    started = time.monotonic()
    OUT.mkdir(parents=True, exist_ok=True)
    bpy.context.preferences.filepaths.save_version = 0
    native = {side: native_markers(side) for side in SIDES}
    bpy.ops.wm.open_mainfile(filepath=str(CORRECTED))
    corrected_scene = bpy.data.scenes[review.SPECS['shotgun']['title']]
    bpy.context.window.scene = corrected_scene
    bpy.context.view_layer.update()
    corrected_hand = next(o for o in corrected_scene.objects
                          if o.type == 'MESH' and o.name.startswith('Soutien'))
    corrected_raw = np.array([v.co[:] for v in corrected_hand.data.vertices])
    corrected_faces = np.array([p.vertices[:] for p in corrected_hand.data.polygons])
    corrected_uvs = np.zeros((len(corrected_raw), 2))
    for loop in corrected_hand.data.loops:
        corrected_uvs[loop.vertex_index] = corrected_hand.data.uv_layers.active.data[loop.index].uv
    corrected_matrix = corrected_hand.matrix_world.copy()
    assert np.allclose(np.array(corrected_matrix),
        json.loads(CORRECTIONS.read_text())['weapons']['shotgun']['hand_matrix_cm'], atol=2e-5)
    bpy.ops.wm.open_mainfile(filepath=str(SOURCE))
    scene = bpy.data.scenes[review.SPECS['shotgun']['title']]
    bpy.context.window.scene = scene
    # Only the shotgun scene is kept in this new file; previous files are read-only.
    for other in list(bpy.data.scenes):
        if other != scene:
            bpy.data.scenes.remove(other)
    hands = {
        side: next(o for o in scene.objects if o.type == 'MESH'
                   and o.name.startswith('Vraie main ' + ('GAUCHE' if side == 'left' else 'DROITE')))
        for side in SIDES}
    old_left = hands['left']
    hands['left'] = review.mesh_object('Main GAUCHE / correction Blender conservee',
        corrected_raw, corrected_faces, corrected_uvs,
        old_left.data.materials[0], scene.collection)
    hands['left'].matrix_world = corrected_matrix
    bpy.data.objects.remove(old_left, do_unlink=True)
    bpy.context.view_layer.update()
    weapon_objects = [o for o in scene.objects if o.type == 'MESH'
                      and o.name.startswith('shotgun_')]
    if len(weapon_objects) != 2:
        raise RuntimeError('Expected only native shotgun base meshes 3 and 4')
    weapon_points, weapon_faces = [], []
    for obj in weapon_objects:
        obj.data.calc_loop_triangles()
        offset = len(weapon_points)
        weapon_points.extend(transform(np.array([v.co[:] for v in obj.data.vertices]),
                                       np.array(obj.matrix_world)).tolist())
        weapon_faces.extend(tuple(offset + v for v in t.vertices)
                            for t in obj.data.loop_triangles)
    weapon_tree = tree(np.array(weapon_points), np.array(weapon_faces))
    report = dict(status='BLENDER_PROTOTYPE_NOT_GAME_VALIDATED', weapon='shotgun',
                  source_scene=str(SOURCE), source_scene_sha256=digest(SOURCE),
                  native_hands_sha256=digest(NATIVE), units='centimetres',
                  preserved_left_scene=str(CORRECTED), preserved_left_sha256=digest(CORRECTED),
                  rigid_only=True, hud_and_menus_modified=False,
                  runtime_collision_api_found=False, hands={})
    before, after = {}, {}
    for side, obj in hands.items():
        raw = np.array([v.co[:] for v in obj.data.vertices])
        local, markers, groups = native[side]
        if raw.shape != local.shape or not np.allclose(raw, local, atol=2e-4):
            raise RuntimeError('Existing hand does not match its native anatomical pose: ' + side)
        obj.data.calc_loop_triangles()
        faces = np.array([t.vertices[:] for t in obj.data.loop_triangles])
        initial = np.array(obj.matrix_world)
        # One point on the palmar contact patch, excluding dorsal skin/forearm.
        # It is bound once to the mesh and never slides during rigid fitting.
        palm_sign = -1 if side == 'left' else 1
        candidates = np.flatnonzero((groups == side + 'hand') & (raw[:, 1] > 2)
                                   & (raw[:, 1] < 11) & (palm_sign * raw[:, 2] > 0))
        if not len(candidates):
            raise RuntimeError('No palmar skin candidates: ' + side)
        distances = [weapon_tree.find_nearest(Vector(p))[3]
                     for p in transform(raw[candidates], initial)]
        markers[0] = raw[candidates[int(np.argmin(distances))]]
        before[side] = initial
        baseline = metrics(raw, faces, markers, initial, weapon_tree)
        print('BEFORE', side, json.dumps(baseline), flush=True)
        fitted, search = fit(raw, faces, markers, initial, weapon_tree)
        result = metrics(raw, faces, markers, fitted, weapon_tree)
        # Regression guards: no geometry/UV/scale/chirality change; no regression.
        assert np.allclose(initial[:3, :3].T @ initial[:3, :3],
                           fitted[:3, :3].T @ fitted[:3, :3], atol=2e-5)
        assert np.linalg.det(fitted[:3, :3]) > 0
        assert result['crossing_triangle_pairs'] <= baseline['crossing_triangle_pairs']
        assert np.array_equal(raw, np.array([v.co[:] for v in obj.data.vertices]))
        after[side] = fitted
        entry = dict(before=baseline, after=result, search=search,
                     markers_local=dict(zip(NAMES, markers.tolist())),
                     mesh_sha256=hashlib.sha256(raw.tobytes()).hexdigest(),
                     vertices=len(raw), triangles=len(faces))
        entry['palm_point_selection'] = 'Nearest palmar skin patch in the preserved initial pose'
        entry['contact_accepted'] = not contact_rejection_reasons(result)
        report['hands'][side] = entry
        print('AFTER', side, json.dumps(result), 'SEARCH', search, flush=True)
        obj.matrix_world = Matrix(fitted.tolist())
        for name, point in zip(NAMES, markers):
            marker = bpy.data.objects.new('CONTACT_' + side.upper() + '_' + name, None)
            scene.collection.objects.link(marker)
            marker.parent = obj
            marker.location = point
            marker.empty_display_type = 'SPHERE'
            marker.empty_display_size = .28
            marker.show_in_front = True
            marker['anatomy'] = side + ':' + name
            marker['gap_cm'] = result['gaps_cm'][name]
    assert sum(o.name.startswith('CONTACT_') for o in scene.objects) == 12
    assert digest(SOURCE) == report['source_scene_sha256']
    assert digest(CORRECTED) == report['preserved_left_sha256']
    report['geometry_contact_accepted'] = all(e['contact_accepted'] for e in report['hands'].values())
    report['fit_seconds'] = round(time.monotonic() - started, 2)
    report['limitations'] = [
        'Six points guide placement; full triangle overlap is also checked.',
        'Parity tests are ambiguous for open or non-manifold weapon surfaces.',
        'Finger joint poses are fixed. Rigid fitting cannot guarantee every fingertip touches.',
        'No dynamic collision, recoil, animation or SFS/AER headset validation.',
        'The existing renderer supports weapon-local support-hand exports only; '
        'the dominant hand remains controller driven.']
    (OUT / 'rapport.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    finalize_report()
    pumps.use_gpu()
    review.setup(scene, True)
    scene.cycles.samples = 16
    scene.render.resolution_x, scene.render.resolution_y = 1440, 960
    review.camera(scene, (22, 0, -6), 120, (-.75, 1.5, .65))
    for name, poses in (('avant', before), ('apres', after)):
        for side, obj in hands.items():
            obj.matrix_world = Matrix(poses[side].tolist())
        scene.render.filepath = str(OUT / (name + '.png'))
        bpy.ops.render.render(write_still=True, scene=scene.name)
    dots = []
    for side, obj in hands.items():
        color = (.1, .8, .9) if side == 'left' else (1, .45, .1)
        material = review.plain_material('Contact ' + side, color, True)
        for name, point in report['hands'][side]['markers_local'].items():
            bpy.ops.mesh.primitive_uv_sphere_add(segments=8, ring_count=4, radius=.38,
                location=transform(np.array([point]), after[side])[0])
            dot = bpy.context.object
            dot.name = 'VISUAL_' + side + '_' + name
            dot.data.materials.append(material)
            dots.append(dot)
    scene.render.filepath = str(OUT / 'contacts.png')
    bpy.ops.render.render(write_still=True, scene=scene.name)
    for dot in dots:
        bpy.data.objects.remove(dot, do_unlink=True)
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'prototype.blend'))
    print('COMPLETE', time.monotonic() - started, 'seconds; originals preserved', flush=True)


if __name__ == '__main__':
    if '--report-only' in sys.argv:
        finalize_report()
    else:
        main()
