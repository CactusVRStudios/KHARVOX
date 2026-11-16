"""Real Blender geometry review; never changes runtime assets or calibration.

blender --background --factory-startup --python this_file -- --only shotgun gauss
Cast/GLB positions are kept in native export units. This is a contact proposal,
not a reconstruction of an observed DOOM animated render frame.
"""
import argparse
import collections
import hashlib
import importlib.util
import json
import math
import pathlib
import struct
import sys

import bpy
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from mathutils.bvhtree import BVHTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXPORT = ROOT / 'build/native-weapons-export'
HANDS = ROOT / 'build/praetor-hands'
OUT = ROOT / 'build/praetor-placement-review'
sys.path.insert(0, str(ROOT / '.tools/cast/libraries/python'))
from cast import Cast, Model, File
AUTHORED_HANDS = {}
HAND_BONES = []

# Reviewed base meshes only: alternative weapon mods coexist in Cast exports.
# Points are proposals in the exported weapon frame, not controller grab zones.
SPECS = {
    'shotgun': dict(stem='shotgun', title='FUSIL A POMPE', meshes=(3, 4),
                    contact=(22, 0, 0), kind='under', note='Soutien sous la pompe'),
    'super-shotgun': dict(stem='double_barrel', title='SUPER SHOTGUN', meshes=(1,),
                          contact=(25, 0, 1), kind='under', note='Soutien sous les deux canons'),
    'gauss': dict(stem='gauss_rifle', title='CANON GAUSS', meshes=(1, 2),
                  contact=(6.5, 40.5, 4.5), kind='side', radial=(1, 0, 0), forward=(0, -1, 0), note='Prise sur la poignee laterale'),
    'chaingun': dict(stem='chaingun', title='MITRAILLEUSE', meshes=(0, 2),
                     contact=(4.5, 39.8, 8), kind='side', radial=(-1, 0, 0), forward=(0, -1, 0), note='Prise sur la poignee laterale'),
    'heavy-ar': dict(stem='heavy_ar', title="FUSIL D'ASSAUT LOURD", meshes=(0,),
                     contact=(20, 0, -4), kind='under', note='Paume sous le garde-main'),
    'plasma': dict(stem='plasmarifle', title='FUSIL A PLASMA', meshes=(1, 2),
                   contact=(18, 0, -15), kind='under', note='Soutien sous le corps de l arme'),
    'rocket': dict(stem='rocketlauncher', title='LANCE-ROQUETTES', meshes=(4,),
                   contact=(15, 0, -14), kind='under', note='Soutien sous le lanceur'),
    'bfg': dict(stem='bfg', title='BFG 9000', meshes=(4,),
                contact=(16, 0, -34), kind='under', note='Soutien sous le chassis'),
    'chainsaw': dict(stem='chainsaw', title='TRONCONNEUSE', meshes=(0, 1),
                     contact=(10, 13.6, 3), kind='side', radial=(-.7, 0, -.7), forward=(0, -1, 0), note='Prise sur la poignee avant'),
}


def unit(value):
    value = np.asarray(value, dtype=float)
    return value / np.linalg.norm(value)


def transformed(points, matrix):
    return np.c_[points, np.ones(len(points))] @ np.asarray(matrix).T[:, :3]


def array_accessor(document, binary, index, width, dtype='<f4'):
    acc = document['accessors'][index]
    view = document['bufferViews'][acc['bufferView']]
    return np.frombuffer(binary, dtype=dtype, count=acc['count'] * width,
                         offset=view.get('byteOffset', 0) + acc.get('byteOffset', 0)).reshape(-1, width).copy()


def mesh_object(name, positions, triangles, uvs, material, collection):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(positions.tolist(), [], triangles.tolist())
    mesh.update()
    if uvs is not None:
        layer = mesh.uv_layers.new(name='UVMap')
        for polygon in mesh.polygons:
            polygon.use_smooth = True
            for loop in polygon.loop_indices:
                layer.data[loop].uv = tuple(uvs[mesh.loops[loop].vertex_index])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    mesh.materials.append(material)
    return obj


def plain_material(name, color, emission=False):
    material = bpy.data.materials.new(name)
    material.diffuse_color = (*color, 1)
    material.use_nodes = True
    shader = material.node_tree.nodes.get('Principled BSDF')
    shader.inputs['Base Color'].default_value = (*color, 1)
    shader.inputs['Roughness'].default_value = .62
    if emission:
        shader.inputs['Emission Color'].default_value = (*color, 1)
        shader.inputs['Emission Strength'].default_value = .8
    return material


def texture_material(name, albedo, normal=None, roughness=None):
    material = plain_material(name, (.35, .35, .35))
    nodes, links = material.node_tree.nodes, material.node_tree.links
    shader = nodes.get('Principled BSDF')
    texture = nodes.new('ShaderNodeTexImage')
    texture.image = bpy.data.images.load(str(albedo), check_existing=True)
    links.new(texture.outputs['Color'], shader.inputs['Base Color'])
    if normal:
        texture = nodes.new('ShaderNodeTexImage')
        texture.image = bpy.data.images.load(str(normal), check_existing=True)
        texture.image.colorspace_settings.name = 'Non-Color'
        mapped = nodes.new('ShaderNodeNormalMap')
        links.new(texture.outputs['Color'], mapped.inputs['Color'])
        links.new(mapped.outputs['Normal'], shader.inputs['Normal'])
    if roughness:
        texture = nodes.new('ShaderNodeTexImage')
        texture.image = bpy.data.images.load(str(roughness), check_existing=True)
        texture.image.colorspace_settings.name = 'Non-Color'
        split = nodes.new('ShaderNodeSeparateColor')
        links.new(texture.outputs['Color'], split.inputs[0])
        links.new(split.outputs['Green'], shader.inputs['Roughness'])
    return material


def load_hand(closure=(.25, .35, .3)):
    if AUTHORED_HANDS:
        return AUTHORED_HANDS[closure if isinstance(closure, str) else tuple(closure)]
    glb_path = HANDS / 'DOOM_LEFT_HAND_FIST.glb'
    raw = glb_path.read_bytes()
    size = struct.unpack_from('<I', raw, 12)[0]
    document, binary = json.loads(raw[20:20 + size]), raw[28 + size:]
    positions = array_accessor(document, binary, 0, 3)
    uvs = array_accessor(document, binary, 2, 2)
    uvs[:, 1] = 1 - uvs[:, 1]
    triangles = array_accessor(document, binary, 3, 1, '<u4').reshape(-1, 3)
    morph = (HANDS / 'DOOM_LEFT_HAND_FINGERS.bin').read_bytes()
    weights = 1 - np.array(closure)
    deltas = np.frombuffer(morph, dtype='<f4', offset=216).reshape(3, len(positions), 6)
    positions += np.einsum('s,svc->vc', weights, deltas[:, :, :3])
    grasp = np.array(struct.unpack_from('<3f', morph, 168))
    grasp += weights @ np.array(struct.unpack_from('<9f', morph, 180)).reshape(3, 3)
    alignment = np.array(json.loads((HANDS / 'provenance.json').read_text())['hands'][0]['alignment']['FIST'])
    native = transformed(positions, np.linalg.inv(alignment))
    native_grasp = transformed(grasp[None], np.linalg.inv(alignment))[0]
    source = next((ROOT / 'build/native-hands-export').rglob('praetor.cast'))
    model = next(m for root in Cast.load(str(source)).Roots() for m in root.ChildrenOfType(Model))
    bones = {b.Name(): np.array(b.WorldPosition()) for b in model.Skeleton().Bones()}
    wrist = bones['lefthand']
    roots = np.array([bones['lefthand' + f + '1'] for f in ('index', 'middle', 'ring', 'pinky')])
    forward = unit(roots.mean(0) - wrist)
    radial = roots[0] - roots[3]
    radial = unit(radial - forward * np.dot(radial, forward))
    basis = np.column_stack((radial, forward, np.cross(radial, forward)))
    return (native - wrist) @ basis, triangles, uvs, (native_grasp - wrist) @ basis


def author_hand_poses():
    # Reuse the actual native skin and bone rotations. Interpolating posed
    # vertices directly shrinks curved fingers and is unsuitable for fitting
    # a static grip. Here rotations are interpolated before skin evaluation.
    module_spec = importlib.util.spec_from_file_location('praetor_builder', ROOT / 'KHARVOX/tools/build_praetor_hands.py')
    builder = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(builder)
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / 'build/praetor-imported.blend'))
    obj = next(o for o in bpy.context.scene.objects if o.type == 'MESH' and o.get('kharvox_native_hands'))
    rig = next(m.object for m in obj.modifiers if m.type == 'ARMATURE')
    side = 'left'
    native = builder.landmarks(rig, side)
    wrist, basis, _ = native
    builder.isolate(obj, rig, side)
    rotations = {}
    for state in ('free', 'gun'):
        builder.pose(rig, side, {finger: state for finger in builder.FINGERS})
        rotations[state] = {b.name: b.rotation_quaternion.copy() for b in rig.pose.bones}
    mapping = None
    for closure in ((.55, .55, .55), (.9, .9, .8), (.6, .6, .5), (.4, .4, .4), (.2, .2, .3), (.05, .05, .1), (0, 0, 0)):
        for index, group in enumerate(builder.GROUPS):
            for finger in group:
                for segment in (1, 2, 3):
                    name = side + 'hand' + finger + str(segment)
                    # Shorter digits need a wider opening around a thick grip.
                    amount = closure[index] * {'pinky': 0., 'ring': .45, 'middle': .75}.get(finger, 1.)
                    rig.pose.bones[name].rotation_quaternion = rotations['free'][name].slerp(rotations['gun'][name], amount)
        bpy.context.view_layer.update()
        # Splay the thumb along the grip axis instead of through its centre.
        # This is an authored static support pose; free fingers stay unchanged.
        if any(closure):
            thumb = rig.pose.bones[side + 'handthumb1']
            local_axis = (rig.matrix_world @ thumb.matrix).to_3x3().inverted() @ Vector(basis[:, 2].tolist())
            thumb.rotation_quaternion = thumb.rotation_quaternion @ Quaternion(local_axis.normalized(), math.radians(-35))
            bpy.context.view_layer.update()
        positions, _, mapping = builder.bake(obj, np.eye(4), mapping)
        if not HAND_BONES:
            for vertex in mapping[0]:
                groups = obj.data.vertices[int(vertex)].groups
                HAND_BONES.append(obj.vertex_groups[max(groups, key=lambda g: g.weight).group].name if groups else 'unweighted')
        _, _, uvs, triangles = mapping
        uvs = uvs.copy()
        uvs[:, 1] = 1 - uvs[:, 1]
        centers = []
        for finger in ('index', 'middle', 'ring'):
            bones = [rig.pose.bones[side + 'hand' + finger + str(segment)] for segment in (1, 2, 3)]
            points = [np.array(rig.matrix_world @ b.head) for b in bones]
            length = (bones[2].head - bones[1].head).length * .75
            points.append(np.array(rig.matrix_world @ (bones[2].head + bones[2].matrix.col[1].xyz.normalized() * length)))
            local = (np.array(points) - wrist) @ basis
            yz = local[:, 1:]
            solution = np.linalg.lstsq(np.c_[2 * yz, np.ones(len(yz))], np.sum(yz ** 2, axis=1), rcond=None)[0]
            centers.append(solution[:2])
        grasp = np.r_[0., np.median(centers, axis=0)]
        AUTHORED_HANDS[closure] = ((positions - wrist) @ basis, triangles.reshape(-1, 3), uvs, grasp)
        print('AUTHORED GRASP', closure, grasp.tolist(), flush=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)


def placement(spec, grasp):
    if spec['kind'] == 'side':
        radial = unit(spec.get('radial', (0, -1, 0)))
        requested_forward = np.array(spec.get('forward', (1, 0, 0)))
        forward = requested_forward - radial * np.dot(requested_forward, radial)
        forward = unit(forward)
    else:
        radial = unit((math.cos(math.radians(15)), math.sin(math.radians(15)), 0))
        forward = unit((math.sin(math.radians(15)), -math.cos(math.radians(15)), -.1))
    basis = np.column_stack((radial, forward, np.cross(radial, forward)))
    matrix = np.eye(4)
    matrix[:3, :3] = basis
    matrix[:3, 3] = np.array(spec['contact']) - basis @ grasp
    return matrix


def weapon(spec, collection, audit):
    path = next(EXPORT.rglob(spec['stem'] + '.cast'))
    model = next(m for root in Cast.load(str(path)).Roots() for m in root.ChildrenOfType(Model))
    meshes = model.Meshes()
    objects, all_points = [], []
    audit.update(source=str(path), source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), meshes=[])
    for index in spec['meshes']:
        mesh = meshes[index]
        positions = np.array(mesh.VertexPositionBuffer()).reshape(-1, 3)
        triangles = np.array(mesh.FaceBuffer()).reshape(-1, 3)
        uvs = np.array(mesh.VertexUVLayerBuffer(0)).reshape(-1, 2)
        uvs[:, 1] = 1 - uvs[:, 1]
        slots = mesh.Material().Slots()
        texture_path = path.parent / pathlib.Path(slots['albedo'].Path().replace('\\', '/'))
        if not texture_path.is_file():
            raise RuntimeError('Missing exported texture: ' + str(texture_path))
        material = texture_material(mesh.Material().Name(), texture_path)
        objects.append(mesh_object(spec['stem'] + '_' + str(index), positions, triangles, uvs, material, collection))
        all_points.append(positions)
        audit['meshes'].append(dict(index=index, material=mesh.Material().Name(), vertices=len(positions)))
    return objects, np.concatenate(all_points)


def camera(scene, center, span, offset=(-.45, 1.6, .75)):
    data = bpy.data.cameras.new('Vue controle')
    obj = bpy.data.objects.new(data.name, data)
    scene.collection.objects.link(obj)
    target = Vector(center)
    obj.location = target + Vector(offset) * span
    obj.rotation_euler = (target - obj.location).to_track_quat('-Z', 'Y').to_euler()
    data.type = 'ORTHO'
    data.ortho_scale = span * 1.25
    data.clip_start = .01
    data.clip_end = 10000
    scene.camera = obj
    return obj


def label(scene, body, at, size=1, color=(.8, .85, .9)):
    data = bpy.data.curves.new('Legende', 'FONT')
    data.body, data.size = body, size
    data.space_line = 1.15
    data.align_y = 'TOP_BASELINE'
    obj = bpy.data.objects.new(body[:30], data)
    scene.collection.objects.link(obj)
    obj.matrix_world = scene.camera.matrix_world @ Matrix.Translation(Vector(at))
    obj.data.materials.append(plain_material('Legende', color, True))
    return obj


def lighting(scene, center, span):
    scene.world = bpy.data.worlds.new('Studio de controle')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs[0].default_value = (.045, .055, .07, 1)
    scene.world.node_tree.nodes['Background'].inputs[1].default_value = .45
    for offset, power, size in (((-.3, .8, 1.3), 95, 1.2), ((.8, -.7, .5), 55, 1), ((-1, -.4, .2), 25, .8)):
        data = bpy.data.lights.new('Lumiere studio', 'AREA')
        data.energy, data.size = power * span * span, span * size
        obj = bpy.data.objects.new(data.name, data)
        scene.collection.objects.link(obj)
        obj.location = Vector(center) + Vector(offset) * span
        obj.rotation_euler = (Vector(center) - obj.location).to_track_quat('-Z', 'Y').to_euler()


def setup(scene, draft):
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 16 if draft else 48
    scene.cycles.use_denoising = True
    scene.cycles.device = 'GPU'
    scene.render.resolution_x = 1440 if draft else 1920
    scene.render.resolution_y = 1000 if draft else 1280
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.view_settings.view_transform = 'AgX'
    scene.unit_settings.system = 'METRIC'
    scene.unit_settings.scale_length = .01


def render_view(scene, name, center, span, draft, title, note, offset=(-.45, 1.6, .75)):
    bpy.context.window.scene = scene
    setup(scene, draft)
    camera(scene, center, span, offset)
    bpy.context.view_layer.update()
    width = scene.camera.data.ortho_scale
    height = width * scene.render.resolution_y / scene.render.resolution_x
    labels = [label(scene, 'PRAETOR / ' + title, (-width * .46, height * .43, -span * .8), width * .026),
              label(scene, note, (-width * .46, height * .36, -span * .8), width * .018, (.35, .85, .9)),
              label(scene, 'PROPOSITION BLENDER - a valider / eclairage de controle',
                    (-width * .46, -height * .43, -span * .8), width * .014, (.65, .7, .75))]
    scene.render.filepath = str(OUT / (name + '.png'))
    bpy.ops.render.render(write_still=True, scene=scene.name)
    for obj in labels:
        bpy.data.objects.remove(obj, do_unlink=True)


def hud_panel(scene, name, center, axis_x, axis_y, color):
    width, height = 25, 25 / (512 / 300)
    x, y, c = np.array(axis_x), np.array(axis_y), np.array(center)
    positions = np.array([c + x * a * width / 2 + y * b * height / 2 for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1))])
    # Outline only: preserve visibility of the native forearm behind the proxy.
    curve = bpy.data.curves.new(name, 'CURVE')
    curve.dimensions, curve.bevel_depth, curve.resolution_u = '3D', .09, 1
    spline = curve.splines.new('POLY')
    spline.points.add(3)
    for point, value in zip(spline.points, positions):
        point.co = (*value, 1)
    spline.use_cyclic_u = True
    obj = bpy.data.objects.new(name, curve)
    scene.collection.objects.link(obj)
    curve.materials.append(plain_material(name, color, True))
    text = bpy.data.curves.new(name + ' libelle', 'FONT')
    text.body, text.size, text.align_x = name, 1.7, 'CENTER'
    label_obj = bpy.data.objects.new(text.name, text)
    scene.collection.objects.link(label_obj)
    basis = np.column_stack((x, y, np.cross(x, y)))
    matrix = np.eye(4)
    matrix[:3, :3], matrix[:3, 3] = basis, c + y * 4
    label_obj.matrix_world = Matrix(matrix.tolist())
    text.materials.append(plain_material(name + ' couleur', color, True))
    return obj


def contact_sheet(keys, columns, filename):
    # Assemble the actual rendered PNGs as emissive image planes in Blender.
    # No synthetic replacement imagery and no game/postprocessing claim.
    rows = math.ceil(len(keys) / columns)
    scene = bpy.data.scenes.new(filename)
    scene.render.engine = 'CYCLES'
    scene.cycles.samples, scene.cycles.use_denoising, scene.cycles.device = 1, False, 'GPU'
    scene.view_settings.view_transform = 'Standard'
    scene.render.resolution_x, scene.render.resolution_y = columns * 960, rows * 640
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    for index, key in enumerate(keys):
        x, y = (index % columns - (columns - 1) / 2) * 960, ((rows - 1) / 2 - index // columns) * 640
        material = bpy.data.materials.new('Rendu ' + key)
        material.use_nodes = True
        nodes, links = material.node_tree.nodes, material.node_tree.links
        nodes.clear()
        image_node = nodes.new('ShaderNodeTexImage')
        image_node.image = bpy.data.images.load(str(OUT / (key + '.png')), check_existing=False)
        emission, output = nodes.new('ShaderNodeEmission'), nodes.new('ShaderNodeOutputMaterial')
        links.new(image_node.outputs['Color'], emission.inputs['Color'])
        links.new(emission.outputs[0], output.inputs['Surface'])
        positions = np.array(((x - 480, y - 320, 0), (x + 480, y - 320, 0),
                              (x + 480, y + 320, 0), (x - 480, y + 320, 0)))
        mesh_object(key, positions, np.array(((0, 1, 2), (0, 2, 3))),
                    np.array(((0, 0), (1, 0), (1, 1), (0, 1))), material, scene.collection)
    camera(scene, np.zeros(3), columns * 960 / 1.25, (0, 0, 2))
    scene.render.filepath = str(OUT / filename)
    bpy.ops.render.render(write_still=True, scene=scene.name)


def fit_clearance(spec, grasp, positions, triangles, objects):
    vertices, faces = [], []
    for obj in objects:
        offset = len(vertices)
        vertices.extend(tuple(v.co) for v in obj.data.vertices)
        faces.extend(tuple(offset + i for i in poly.vertices) for poly in obj.data.polygons)
    weapon_bvh = BVHTree.FromPolygons(vertices, faces, all_triangles=True, epsilon=.005)
    initial = placement(spec, grasp)
    forward, normal = initial[:3, 1], initial[:3, 2]
    candidates = [(a, b) for a in (0, -.5, .5, -1, 1, -2, 2, -3, 3)
                  for b in np.arange(-2, 7.01, .5)]
    candidates.sort(key=lambda v: v[0] ** 2 + v[1] ** 2)
    best = None
    for a, b in candidates:
        shift = forward * a + normal * b
        candidate = dict(spec, contact=(np.array(spec['contact']) + shift).tolist())
        matrix = placement(candidate, grasp)
        points = transformed(positions, matrix)
        hand_bvh = BVHTree.FromPolygons(points.tolist(), triangles.tolist(), all_triangles=True, epsilon=.005)
        pairs = weapon_bvh.overlap(hand_bvh)
        crossings = len(pairs)
        if a == 0 and b == 0:
            print('INITIAL CROSSINGS', spec['stem'], collections.Counter(HAND_BONES[int(triangles[index][0])] for _, index in pairs), flush=True)
        if best is None or crossings < best[0]:
            best = crossings, shift.tolist(), matrix
        if crossings == 0:
            break
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', nargs='*', default=list(SPECS))
    parser.add_argument('--draft', action='store_true')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else [])
    bpy.context.preferences.filepaths.save_version = 0
    cycles = bpy.context.preferences.addons['cycles'].preferences
    cycles.compute_device_type = 'CUDA'
    cycles.get_devices()
    for device in cycles.devices:
        device.use = device.type == 'CUDA'
    print('GPU DEVICES', [(d.name, d.type, d.use) for d in cycles.devices], flush=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    author_hand_poses()
    hand_material = texture_material('Praetor natif', HANDS / 'BaseColor.png', HANDS / 'Normal.png', HANDS / 'MetallicRoughness.png')
    audit = dict(status='PROPOSALS_ONLY_NOT_APPLIED', units='native Cast export units',
                 note='Native model contact study; no claim to reproduce a live animated engine frame.', weapons={})
    for key, spec in SPECS.items():
        scene = bpy.data.scenes.new(spec['title'])
        bpy.context.window.scene = scene
        entry = {}
        objects, weapon_points = weapon(spec, scene.collection, entry)
        poses = ((.9, .9, .8), (.6, .6, .5), (.4, .4, .4), (.2, .2, .3), (.05, .05, .1)) if spec['kind'] == 'side' else ((.55, .55, .55),)
        fitted = []
        for closure in poses:
            data = load_hand(closure)
            result = fit_clearance(spec, data[3], data[0], data[1], objects)
            fitted.append((result, closure, data))
        (crossings, shift, hand_matrix), closure, data = min(fitted, key=lambda row: (row[0][0], np.linalg.norm(row[0][1])))
        hand_positions, hand_triangles, hand_uvs, grasp = data
        if not np.isfinite(hand_positions).all() or hand_triangles.min() < 0 or hand_triangles.max() >= len(hand_positions):
            raise RuntimeError('Invalid authored hand geometry')
        if not np.allclose(hand_matrix[:3, :3].T @ hand_matrix[:3, :3], np.eye(3), atol=1e-6):
            raise RuntimeError('Non-rigid hand placement')
        print('CONTACT FIT', key, 'crossing_triangle_pairs', crossings, 'shift', shift, flush=True)
        hand = mesh_object('Main Praetor de soutien', hand_positions, hand_triangles, hand_uvs, hand_material, scene.collection)
        hand.matrix_world = Matrix(hand_matrix.tolist())
        points = np.concatenate((weapon_points, transformed(hand_positions, hand_matrix)))
        center, span = (points.max(0) + points.min(0)) * .5, float(np.ptp(points, axis=0).max())
        lighting(scene, center, span)
        entry.update(contact_proposal=list(spec['contact']), hand_matrix=hand_matrix.tolist(), selected_hand='left',
                     native_hand_bounds=[hand_positions.min(0).tolist(), hand_positions.max(0).tolist()],
                     fixed_finger_closure=list(closure) if not isinstance(closure, str) else None,
                     authored_profile=closure if isinstance(closure, str) else 'skeletal_interpolation',
                     crossing_triangle_pairs=crossings, fit_shift=shift,
                     thumb_abduction_degrees=-35, actual_grasp_center=transformed(grasp[None], hand_matrix)[0].tolist())
        audit['weapons'][key] = entry
        if key in args.only:
            render_view(scene, key, center, span, args.draft, spec['title'], spec['note'])
            if key in ('shotgun', 'super-shotgun', 'gauss', 'chaingun'):
                render_view(scene, key + '-contact', np.array(spec['contact']), 46, args.draft,
                            spec['title'], 'Gros plan du contact paume / doigts', (-.3, -1.6, -.15) if spec['kind'] == 'under' else (1.4, .3, -.5))
    # HUD review uses the real native hand in its neutral anatomical frame.
    # Coloured rectangles are metric canvas proxies, not invented game content.
    scene = bpy.data.scenes.new('HUD Praetor - gabarits')
    bpy.context.window.scene = scene
    hand_positions, hand_triangles, hand_uvs, grasp = load_hand((0, 0, 0))
    hand = mesh_object('Avant-bras Praetor', hand_positions, hand_triangles, hand_uvs, hand_material, scene.collection)
    # Wrist at origin; fingers +Y, thumb +X, dorsal surface +Z for left hand.
    normal = unit((.15, -.1, 1))
    axis_x = unit((1, 0, -.15))
    axis_y = unit(np.cross(normal, axis_x))
    projected = hand_positions @ normal
    plane = float(projected.max()) + 1.0
    if plane - float(projected.max()) < .999:
        raise RuntimeError('HUD proxy penetrates native hand envelope')
    base = np.array((0, -11, 0), dtype=float)
    base += normal * (plane - np.dot(base, normal))
    centers = [base - axis_x * 13.5, base + axis_x * 13.5]
    for name, color, center in zip(('VIE', 'MUNITIONS'), ((.08, .65, .85), (.95, .5, .08)), centers):
        hud_panel(scene, name, center, axis_x, axis_y, color)
    lighting(scene, (0, -6, 0), 70)
    audit['hud'] = dict(status='canvas proxies, not actual SWF', canvas_width=25,
                        canvas_aspect=512 / 300, clearance_native_units=1,
                        panel_normal=normal.tolist(), centers=[c.tolist() for c in centers])
    if 'hud' in args.only or (not args.draft and set(SPECS).issubset(args.only)):
        render_view(scene, 'hud-praetor', np.array((0, -5, 2)), 70, args.draft,
                    'HUD / AVANT-BRAS', 'Gabarits vie et munitions - separation du volume', (-.2, -.4, 1.6))
        render_view(scene, 'hud-praetor-profil', np.array((0, -5, 0)), 70, args.draft,
                    'HUD / DEGAGEMENT', 'Gabarits uniquement - pas les graphismes finaux du jeu', (1.5, -.3, .12))
    OUT.mkdir(exist_ok=True)
    audit['all_selected_mesh_crossings_zero'] = all(value['crossing_triangle_pairs'] == 0 for value in audit['weapons'].values())
    (OUT / 'placements.json').write_text(json.dumps(audit, indent=2), encoding='utf-8')
    if not args.draft:
        contact_sheet(('shotgun-contact', 'super-shotgun-contact', 'gauss-contact', 'chaingun-contact'), 2, 'apercu-4-prises.png')
        contact_sheet(list(SPECS), 3, 'apercu-9-armes.png')
    bpy.context.window.scene = bpy.data.scenes['FUSIL A POMPE']
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'praetor-weapons.blend'))
    print('REVIEW SAVED', OUT, flush=True)


if __name__ == '__main__':
    main()
