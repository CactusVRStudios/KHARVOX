"""Two diagnostic reading examples, using the actual calibrated Praetor hand.

This is NOT a native SWF screenshot or a proposed runtime HUD calibration.
No runtime source, package, executable or user setting is written.
"""
import sys
sys.dont_write_bytecode = True
import hashlib
import importlib.util
import json
import math
import pathlib

import bpy
import numpy as np
from mathutils import Matrix

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/pump-review-v3'
RUNTIME = ROOT / 'references/test-fixtures/reviewed-support'


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'KHARVOX/tools' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


review = module('geometry_review', 'preview_praetor_weapon_grips.py')
hud = module('runtime_hand_reader', 'preview_praetor_hud_clearance.py')
review.OUT = OUT


def physical_hand():
    points, triangles, uvs, audit = hud.load_runtime_hand(RUNTIME, 'left', True)
    sys.path.insert(0, str(ROOT / '.tools/cast/libraries/python'))
    from cast import Cast, Model
    cast_path = next((ROOT / 'build/native-hands-export').rglob('praetor.cast'))
    model = next(m for root in Cast.load(str(cast_path)).Roots() for m in root.ChildrenOfType(Model))
    bones = {b.Name(): np.array(b.WorldPosition(), dtype=float) for b in model.Skeleton().Bones()}
    wrist = bones['lefthand']
    roots = np.array([bones['lefthand' + finger + '1'] for finger in ('index', 'middle', 'ring', 'pinky')])
    forward = review.unit(roots.mean(0) - wrist)
    radial = roots[0] - roots[-1]
    radial = review.unit(radial - forward * np.dot(radial, forward))
    anatomical = np.column_stack((radial, forward, np.cross(radial, forward)))
    provenance = json.loads((ROOT / 'build/praetor-hands/provenance.json').read_text())
    alignment = np.array(provenance['hands'][0]['alignment']['FIST'])
    native_to_xr = np.array(audit['raw_grip_to_baked_mesh_matrix_meters']) @ alignment
    meters_per_unit = float(np.linalg.norm(native_to_xr[:3, 0]))
    scales = np.linalg.norm(native_to_xr[:3, :3], axis=0)
    if not np.allclose(scales, meters_per_unit, atol=1e-8):
        raise RuntimeError('The accepted free-hand reference must use a uniform scale')
    wrist_xr = native_to_xr[:3, :3] @ wrist + native_to_xr[:3, 3]
    axes_xr = native_to_xr[:3, :3] @ anatomical / meters_per_unit
    # This is a rigid change of viewing coordinates only: physical size is
    # taken directly from the production free-pose GLB + morph + calibration.
    local_cm = (points / 100 - wrist_xr) @ axes_xr * 100
    display_rotation = np.array(((0, 1, 0), (-1, 0, 0), (0, 0, 1)), dtype=float)
    display_cm = local_cm @ display_rotation.T
    before = np.linalg.norm(points[::53] - points[0], axis=1)
    after = np.linalg.norm(display_cm[::53] - display_cm[0], axis=1)
    if not np.allclose(before, after, atol=.0001):
        raise RuntimeError('Diagnostic view must not resize the runtime hand')
    audit.update(native_to_xr_matrix=native_to_xr.tolist(), meters_per_native_unit=meters_per_unit,
                 canonical_view='fingers +X; forearm -X; dorsal +Z; rigid view change only',
                 cast_sha256=hashlib.sha256(cast_path.read_bytes()).hexdigest())
    return display_cm, triangles, uvs, audit


def emission(name, color, strength=1.3):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    light = nodes.new('ShaderNodeEmission')
    light.inputs['Color'].default_value = (*color, 1)
    light.inputs['Strength'].default_value = strength
    output = nodes.new('ShaderNodeOutputMaterial')
    links.new(light.outputs[0], output.inputs['Surface'])
    return material


def line(scene, parent, name, points, material, closed=False, thickness=.045):
    curve = bpy.data.curves.new(name, 'CURVE')
    curve.dimensions, curve.bevel_depth, curve.resolution_u = '3D', thickness, 1
    spline = curve.splines.new('POLY')
    spline.points.add(len(points) - 1)
    for point, value in zip(spline.points, points):
        point.co = (*value, 1)
    spline.use_cyclic_u = closed
    obj = bpy.data.objects.new(name, curve)
    scene.collection.objects.link(obj)
    obj.parent = parent
    obj.data.materials.append(material)
    return obj


def text(scene, parent, body, position, size, material, font, align='LEFT'):
    curve = bpy.data.curves.new(body, 'FONT')
    curve.body, curve.size, curve.align_x = body, size, align
    curve.font = font
    curve.extrude = 0
    obj = bpy.data.objects.new(body, curve)
    scene.collection.objects.link(obj)
    obj.parent = parent
    obj.location = position
    curve.materials.append(material)
    return obj


def panel(scene, mode, center, label, value, detail, color, font):
    parent = bpy.data.objects.new('HUD diagnostic ' + label, None)
    scene.collection.objects.link(parent)
    matrix = np.eye(4)
    rotations = {'180': (-1, -1, 1), 'mirror': (-1, 1, 1), 'upright': (1, 1, 1)}
    matrix[:3, :3] = np.diag(rotations[mode])
    matrix[:3, 3] = center
    parent.matrix_world = Matrix(matrix.tolist())
    glow = emission(label + ' emission', color)
    white = emission(label + ' chiffres', (.85, .94, 1.))
    width, height = 12, 7
    corners = [(-width/2, -height/2, 0), (width/2, -height/2, 0),
               (width/2, height/2, 0), (-width/2, height/2, 0)]
    line(scene, parent, label + ' cadre', corners, glow, True)
    text(scene, parent, label, (-5.1, 2.1, .05), .95, glow, font)
    text(scene, parent, value, (-5.1, -.7, .05), 2.7, white, font)
    text(scene, parent, detail, (-5.1, -2.1, .05), .82, glow, font)
    line(scene, parent, label + ' barre', [(-5.1, -2.8, .05), (4.6, -2.8, .05)], glow, thickness=.10)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    bpy.context.preferences.filepaths.save_version = 0
    prefs = bpy.context.preferences.addons['cycles'].preferences
    prefs.compute_device_type = 'CUDA'
    prefs.get_devices()
    for device in prefs.devices:
        device.use = device.type == 'CUDA'
    bpy.ops.wm.read_factory_settings(use_empty=True)
    points, triangles, uvs, source = physical_hand()
    textures = ROOT / 'build/praetor-hands'
    material = review.texture_material('Praetor / textures existantes', textures/'BaseColor.png',
        textures/'Normal.png', textures/'MetallicRoughness.png')
    font = bpy.data.fonts.load(r'C:\Windows\Fonts\bahnschrift.ttf')
    forearm = points[(points[:, 0] > -29) & (points[:, 0] < -4)]
    if not len(forearm):
        raise RuntimeError('Missing forearm envelope for the reading example')
    height = float(forearm[:, 2].max()) + 2.
    source['illustrative_plane_height_cm'] = height
    source['forearm_clearance_cm'] = 2.
    source['hud_artwork'] = 'Illustrative labels only; native SWF artwork and actual native GUI axes are NOT reconstructed.'
    source['scope'] = 'Reading diagnosis only, not a runtime placement proposal.'
    for mode, filename, title in (('180', 'hud-a-rotation-180', 'A / TEXTE TOURNE DE 180 DEGRES'),
                                   ('mirror', 'hud-b-miroir', 'B / LETTRES EN MIROIR')):
        scene = bpy.data.scenes.new(title)
        bpy.context.window.scene = scene
        review.mesh_object('Notre main Praetor / taille libre', points, triangles, uvs, material, scene.collection)
        panel(scene, mode, (-22, 0, height), 'VIE', '100', 'ARMURE 50', (.08, .65, 1.), font)
        panel(scene, mode, (-8, 0, height), 'MUNITIONS', '24 / 120', 'FUSIL A POMPE', (1., .55, .12), font)
        center = np.array((-7., 0., 3.))
        span = 66.
        review.lighting(scene, center, span)
        review.setup(scene, False)
        scene.cycles.samples = 24
        review.camera(scene, center, span, (0, -.75, 1.65))
        bpy.context.view_layer.update()
        width = scene.camera.data.ortho_scale
        screen_height = width * scene.render.resolution_y / scene.render.resolution_x
        review.label(scene, title, (-width*.46, screen_height*.43, -span*.8), width*.030)
        review.label(scene, 'MEME MAIN PRAETOR / MEME TAILLE / MEME CAMERA',
            (-width*.46, screen_height*.36, -span*.8), width*.018, (.4, .85, .95))
        review.label(scene, 'REPERES DE LECTURE ILLUSTRATIFS - pas une capture du HUD natif',
            (-width*.46, -screen_height*.41, -span*.8), width*.016, (.8, .75, .58))
        review.label(scene, 'Lecture normale de reference : VIE 100  /  MUNITIONS 24 / 120',
            (-width*.46, -screen_height*.46, -span*.8), width*.014, (.7, .75, .8))
        scene.render.filepath = str(OUT / (filename + '.png'))
        bpy.ops.render.render(write_still=True, scene=scene.name)
    (OUT/'hud-lecture.json').write_text(json.dumps(source, indent=2), encoding='utf-8')
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'hud-lecture.blend'))
    print('TWO READING EXAMPLES SAVED', OUT, flush=True)


if __name__ == '__main__':
    main()
