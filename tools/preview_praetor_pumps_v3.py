"""Two-shotgun visual proposals. No runtime assets or executable are written.

The hand uses the accepted free-hand physical scale. The unit native weapon
basis is an EXPLICIT hypothesis, not a captured game frame. Authored finger
poses use the native skeleton, not an interpolation that shrinks finger bones.
"""
import sys
sys.dont_write_bytecode = True
import json
import math
import pathlib
import collections

import bpy
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from mathutils.bvhtree import BVHTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'KHARVOX/tools'))
import preview_praetor_hud_reading as reading
review = reading.review
OUT = ROOT / 'build/pump-review-v3'
review.OUT = OUT
WEAPON_CM_PER_UNIT = 100 / 39.3701


def bvh(points, triangles):
    return BVHTree.FromPolygons(points.tolist(), triangles.tolist(), all_triangles=True)


def support_poses():
    builder = reading.module('pump_pose_builder', 'build_praetor_hands.py')
    bpy.ops.wm.open_mainfile(filepath=str(ROOT/'build/praetor-imported.blend'))
    obj = next(o for o in bpy.context.scene.objects if o.type == 'MESH' and o.get('kharvox_native_hands'))
    rig = next(m.object for m in obj.modifiers if m.type == 'ARMATURE')
    wrist, basis, _ = builder.landmarks(rig, 'left')
    builder.isolate(obj, rig, 'left')
    mapping = None
    for base in ((0, 9, 5), (10, 32, 35), (20, 35, 30), (30, 50, 30), (40, 65, 30)):
        for thumb_angles, thumb_angle in (
                ((0,0,2), -90), ((0,0,2), -70), ((0,0,2), -50),
                ((0,0,2), -20), ((0,0,2), 10), ((0,0,2), 40),
                ((8,18,10), -90), ((8,18,10), -70), ((8,18,10), -50),
                ((8,18,10), -20), ((8,18,10), 10), ((8,18,10), 40),
                ((12,28,18), -90), ((12,28,18), -70), ((12,28,18), -50),
                ((12,28,18), -20), ((12,28,18), 10), ((12,28,18), 40)):
            for finger in builder.FINGERS:
                angles = thumb_angles if finger == 'thumb' else base
                first = rig.data.bones['lefthand'+finger+'1'].matrix_local
                second = rig.data.bones['lefthand'+finger+'2'].matrix_local
                sign = 1 if first.col[1].xyz.cross(second.col[1].xyz).dot(first.col[0].xyz) >= 0 else -1
                for segment, angle in enumerate(angles, 1):
                    bone = rig.pose.bones['lefthand'+finger+str(segment)]
                    bone.rotation_mode = 'QUATERNION'
                    bone.rotation_quaternion = Quaternion((1,0,0), sign*math.radians(angle))
            bpy.context.view_layer.update()
            thumb = rig.pose.bones['lefthandthumb1']
            axis = (rig.matrix_world @ thumb.matrix).to_3x3().inverted() @ Vector(basis[:,2].tolist())
            thumb.rotation_quaternion @= Quaternion(axis.normalized(), math.radians(thumb_angle))
            bpy.context.view_layer.update()
            points, _, mapping = builder.bake(obj, np.eye(4), mapping)
            vertices, _, uvs, triangles = mapping
            if not review.HAND_BONES:
                for vertex in vertices:
                    weights = obj.data.vertices[int(vertex)].groups
                    review.HAND_BONES.append(obj.vertex_groups[max(weights, key=lambda g:g.weight).group].name)
            uvs = uvs.copy()
            uvs[:,1] = 1-uvs[:,1]
            review.AUTHORED_HANDS[(*base, *thumb_angles, thumb_angle)] = (
                (points-wrist)@basis, triangles.reshape(-1,3), uvs, np.zeros(3))
    bpy.ops.wm.read_factory_settings(use_empty=True)


def fit(spec, objects, hand_cm_per_unit):
    vertices, faces = [], []
    for obj in objects:
        offset = len(vertices)
        vertices.extend(tuple(v.co * WEAPON_CM_PER_UNIT) for v in obj.data.vertices)
        faces.extend(tuple(offset + i for i in p.vertices) for p in obj.data.polygons)
    weapon = BVHTree.FromPolygons(vertices, faces, all_triangles=True)
    # Find the actual underside of the exported fore-end, not a guessed Z=0.
    x = spec['contact'][0] * WEAPON_CM_PER_UNIT
    contacts = []
    for roll in (-60,-30,0,30,60):
        r = math.radians(roll)
        direction = Vector((0,math.sin(r),-math.cos(r)))
        center = Vector((x,0,spec['contact'][2]*WEAPON_CM_PER_UNIT))
        hit, normal, _, _ = weapon.ray_cast(center+direction*150, -direction)
        if hit is not None:
            if normal.dot(direction) < 0:
                normal = -normal
            contacts.append((np.array(hit), np.array(normal), roll))
    if not contacts:
        raise RuntimeError('No real fore-end surface found')
    bone_names = np.array(review.HAND_BONES)
    fitted = []
    for closure, data in review.AUTHORED_HANDS.items():
        points, triangles, uvs, grasp = data
        physical = points * hand_cm_per_unit
        # Surface of the palm facing the weapon (anatomical -Z), restricted to
        # the hand bone and metacarpal area; exclude forearm and curled digits.
        palm_mask = ((bone_names == 'lefthand') | (bone_names == 'lefthandthumb1')) & (points[:, 1] > 0) & (points[:, 1] < 12)
        palm = physical[palm_mask]
        if len(palm) < 8:
            raise RuntimeError('Missing native palm contact region')
        anchor = np.median(palm, axis=0)
        anchor[2] = palm[:,2].min()
        for hit, normal, roll in contacts:
            radial = np.array((1.,0,0)) - normal*normal[0]
            radial = review.unit(radial)
            forward = review.unit(np.cross(normal,radial))
            rotation = np.column_stack((radial,forward,normal))
            oriented = physical @ rotation.T
            for gap in (.15,.5,1.,1.5,2.,3.):
                translation = hit + normal*gap - rotation@anchor
                placed = oriented + translation
                pairs = weapon.overlap(bvh(placed, triangles))
                palm_distances = [weapon.find_nearest(Vector(p))[3] for p in placed[palm_mask][::8]]
                closest_palm = min(d for d in palm_distances if d is not None)
                fingers = placed[np.char.find(bone_names.astype(str), 'handindex') >= 0]
                distances = [weapon.find_nearest(Vector(p))[3] for p in fingers[::25]]
                finger_gap = float(np.median([d for d in distances if d is not None]))
                # Prioritize contact, not just a nonintersecting hand far away.
                score = (len(pairs), max(0, closest_palm - 1.1), finger_gap + closest_palm*.5)
                matrix = np.eye(4)
                matrix[:3, :3], matrix[:3, 3] = rotation * hand_cm_per_unit, translation
                fitted.append((score, matrix, closure, data, closest_palm, gap, (0,roll),hit))
                if not pairs:
                    break
    score, matrix, closure, data, palm_gap, gap, yaw, hit = min(fitted, key=lambda r: r[0])
    # Refine the point on the real fore-end. A long rigid armored thumb and
    # palm web cannot be fitted by changing digit curl alone.
    refined = []
    seeds = []
    for curl in dict.fromkeys(row[2][:3] for row in fitted):
        seeds.extend(sorted((row for row in fitted if row[2][:3] == curl), key=lambda r:r[0])[:3])
    for seed in seeds:
        _, initial, recipe, sample, _, seed_gap, orientation, seed_hit = seed
        raw, tris, _, _ = sample
        palm_mask = ((bone_names == 'lefthand') | (bone_names == 'lefthandthumb1')) & (raw[:,1] > 0) & (raw[:,1] < 12)
        for dx in (-8,-4,0,4,8):
            for dy in (-8,-4,0,4,8):
                moved = initial.copy()
                moved[:3,3] += (dx,dy,0)
                placed = review.transformed(raw, moved)
                nearest = [weapon.find_nearest(Vector(p))[3] for p in placed[palm_mask][::8]]
                distance = min(d for d in nearest if d is not None)
                if distance > 1.1:
                    continue
                crossings = len(weapon.overlap(bvh(placed, tris)))
                finger_points = placed[np.char.find(bone_names.astype(str), 'handindex') >= 0]
                distances = [weapon.find_nearest(Vector(p))[3] for p in finger_points[::25]]
                finger_gap = float(np.median([d for d in distances if d is not None]))
                score_new = (crossings, max(0,distance-1.1), finger_gap+distance*.5+.002*(dx*dx+dy*dy))
                refined.append((score_new,moved,recipe,sample,distance,seed_gap,orientation,seed_hit))
    if refined:
        best = min(refined, key=lambda r:r[0])
        if best[0] < score:
            score, matrix, closure, data, palm_gap, gap, yaw, hit = best
    print('FIT', spec['stem'], score, closure, 'palm_gap_cm', palm_gap, 'yaw', yaw, flush=True)
    placed = review.transformed(data[0], matrix)
    pairs = weapon.overlap(bvh(placed, data[1]))
    print('CROSSINGS BY BONE', collections.Counter(review.HAND_BONES[int(data[1][i][0])] for _,i in pairs), flush=True)
    return matrix, data, dict(crossing_triangle_pairs=score[0], palm_distance_cm=palm_gap,
        contact_region='palm and thumb web', contact_surface_cm=hit.tolist(),
        underside_cm=hit.tolist(),
        finger_closure=list(closure), yaw_degrees=yaw, hand_matrix_cm=matrix.tolist(),
        status='CONTACT_PROPOSAL' if not score[0] and palm_gap <= 1.1 else 'NEEDS_FURTHER_FIT')


def render(scene, name, center, span, title, note, offset):
    # Existing lighting, meshes, textures and cameras, with fewer path samples.
    original_setup = review.setup
    original_label = review.label
    def setup(scene, draft):
        original_setup(scene, draft)
        scene.cycles.samples = 24
    review.setup = setup
    def overlay(scene, body, at, size=1, color=(.8,.85,.9)):
        return original_label(scene, body, (at[0],at[1],-.1), size, color)
    review.label = overlay
    try:
        review.render_view(scene, name, np.array(center), span, False, title, note, offset)
    finally:
        review.setup = original_setup
        review.label = original_label


def use_gpu():
    prefs = bpy.context.preferences.addons['cycles'].preferences
    prefs.compute_device_type = 'CUDA'
    prefs.get_devices()
    for device in prefs.devices:
        device.use = device.type == 'CUDA'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bpy.context.preferences.filepaths.save_version = 0
    use_gpu()
    free_points, free_triangles, free_uvs, source = reading.physical_hand()
    hand_scale = source['meters_per_native_unit'] * 100
    support_poses()
    material = review.texture_material('Praetor natif / taille physique', review.HANDS/'BaseColor.png',
        review.HANDS/'Normal.png', review.HANDS/'MetallicRoughness.png')
    audit = dict(status='VISUAL_PROPOSALS_ONLY_NO_RUNTIME_BUILD',
        hand_source=source, hand_cm_per_source_unit=hand_scale,
        weapon_cm_per_source_unit=WEAPON_CM_PER_UNIT,
        weapon_basis='UNMEASURED: unit native basis, default 39.3701 units/m',
        caveat='Not a reconstruction of an observed game frame; validate appearance first.', weapons={})
    selected = None
    for key in ('shotgun', 'super-shotgun'):
        spec = review.SPECS[key]
        scene = bpy.data.scenes.new(spec['title'])
        bpy.context.window.scene = scene
        entry = {}
        objects, weapon_points = review.weapon(spec, scene.collection, entry)
        matrix, data, fit_audit = fit(spec, objects, hand_scale)
        if fit_audit['status'] != 'CONTACT_PROPOSAL':
            raise RuntimeError('Unresolved contact: ' + json.dumps(fit_audit))
        for obj in objects:
            obj.scale = (WEAPON_CM_PER_UNIT,)*3
        positions, triangles, uvs, _ = data
        hand = review.mesh_object('Main de soutien / echelle libre conservee', positions,
            triangles, uvs, material, scene.collection)
        hand.matrix_world = Matrix(matrix.tolist())
        # Check rigid support placement preserves stable palm dimensions. Digit
        # articulation is allowed; no scaling depends on grip or weapon size.
        if not np.allclose(matrix[:3, :3].T @ matrix[:3, :3], np.eye(3)*hand_scale**2, atol=1e-8):
            raise RuntimeError('Support placement resized the hand')
        placed = review.transformed(positions, matrix)
        points = np.concatenate((weapon_points*WEAPON_CM_PER_UNIT, placed))
        center, span = (points.max(0)+points.min(0))/2, float(np.ptp(points, axis=0).max())
        review.lighting(scene, center, span)
        render(scene, key+'-v3', center, span, spec['title'],
            'Main a taille libre / echelle arme unitaire a confirmer en jeu', (-.6, 1.5, .55))
        contact = np.array(fit_audit['underside_cm'])
        render(scene, key+'-contact-v3', contact + np.array((0, 0, -5)), 57, spec['title'],
            'Contact sous le garde-main / proposition a valider', (-.4, -1.6, -.25))
        entry.update(fit_audit)
        audit['weapons'][key] = entry
        if selected is None:
            selected = data
    scene = bpy.data.scenes.new('Taille physique constante')
    bpy.context.window.scene = scene
    # Free reference is a rigid view of the real runtime mesh. Support mesh is
    # skeletal-pose geometry at the exact same physical scale, no weapon scale.
    review.mesh_object('LIBRE', free_points + (0, 13, 0), free_triangles, free_uvs, material, scene.collection)
    points, triangles, uvs, _ = selected
    rotation = np.array(((0,1,0),(-1,0,0),(0,0,1)))
    locked = points @ rotation.T * hand_scale + np.array((0,-24,0))
    review.mesh_object('VERROUILLEE', locked, triangles, uvs, material, scene.collection)
    review.lighting(scene, (-7,0,0), 95)
    render(scene, 'taille-constante', (-7,-3,0), 110, 'TAILLE CONSTANTE',
        'Haut : libre / Bas : soutien / meme echelle physique du gant', (0,-.1,2))
    font = bpy.data.fonts.load(r'C:\Windows\Fonts\bahnschrift.ttf')
    forearm = free_points[(free_points[:,0]>-29)&(free_points[:,0]<-4)]
    height = float(forearm[:,2].max()) + 3.5
    scene = bpy.data.scenes.new('HUD / correction A')
    bpy.context.window.scene = scene
    review.mesh_object('Praetor / main libre', free_points, free_triangles, free_uvs, material, scene.collection)
    reading.panel(scene, 'upright', (-22,0,height), 'VIE', '100', 'ARMURE 50', (.08,.65,1), font)
    reading.panel(scene, 'upright', (-8,0,height), 'MUNITIONS', '24 / 120', 'FUSIL A POMPE', (1,.55,.12), font)
    review.lighting(scene, (-7,0,3), 66)
    render(scene, 'hud-corrige', (-7,0,3), 66, 'HUD / CORRECTION DU CAS A',
        'Lecture retournee de 180 degres / reperes illustratifs, graphisme natif conserve en jeu', (0,-.75,1.65))
    render(scene, 'hud-corrige-profil', (-7,0,3), 66, 'HUD / DEGAGEMENT ARMURE',
        'Proposition : 3,5 cm au-dessus de l enveloppe de l avant-bras / reperes illustratifs', (0,-1.8,.18))
    audit['hud'] = dict(correction='A: rotate actual pixel axes 180 degrees, no mirror',
        illustrative_only=True, native_swf_artwork_unavailable=True,
        proposed_clearance_cm=3.5, panel_z_cm=height,
        forearm_max_z_cm=float(forearm[:,2].max()),
        live_native_gui_axes='not captured; this is an appearance proposal, not an exact native layout')
    (OUT/'propositions-v3.json').write_text(json.dumps(audit, indent=2), encoding='utf-8')
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'propositions-v3.blend'))
    print('PROPOSALS SAVED / NO EXECUTABLE BUILT', flush=True)


if __name__ == '__main__':
    if '--reverse-only' in sys.argv:
        use_gpu()
        bpy.ops.wm.open_mainfile(filepath=str(OUT/'propositions-v3.blend'))
        audit = json.loads((OUT/'propositions-v3.json').read_text())
        for key in ('shotgun','super-shotgun'):
            spec = review.SPECS[key]
            scene = bpy.data.scenes[spec['title']]
            center = np.array(audit['weapons'][key]['contact_surface_cm'])
            render(scene, key+'-contact-reverse-v3', center+(0,0,-5), 57,
                spec['title'], 'Angle oppose : paume / pouce / dessous du garde-main', (1.35,.8,-.1))
    else:
        main()
