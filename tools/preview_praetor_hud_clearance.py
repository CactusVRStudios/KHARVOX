"""Offline HUD envelope review in the production raw-controller coordinate frame.

Call build_hud_review(review, draft=False, render=True) from the weapon preview
module. ``review`` supplies mesh_object, plain_material, texture_material, lighting, render_view
and OUT. Only the two explicitly named HUD PNGs may be written by this module.
No runtime assets/configuration are changed. Native SWF artwork is not recreated.
"""
import hashlib
import json
import math
import pathlib
import struct

import bpy
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
ORIGINAL = ROOT.parent / 'KHARVOX-v1.4a'
PRAETOR = ROOT / 'artifacts/KHARVOX-VR-Valide'
OUT = ROOT / 'build/praetor-placement-review-v2'
# DOOM forward/left/up -> OpenXR right/up/back, in one raw Grip frame.
DOOM_TO_XR = np.array(((0, -1, 0), (0, 0, 1), (-1, 0, 0)), dtype=float)


def unit(vector):
    vector = np.asarray(vector, dtype=float)
    length = np.linalg.norm(vector)
    if length < 1e-12:
        raise ValueError('Undefined existing HUD placement direction')
    return vector / length


def quaternion_matrix(quaternion):
    x, y, z, w = quaternion
    return np.array(((1-2*(y*y+z*z), 2*(x*y-w*z), 2*(x*z+w*y)),
                     (2*(x*y+w*z), 1-2*(x*x+z*z), 2*(y*z-w*x)),
                     (2*(x*z-w*y), 2*(y*z+w*x), 1-2*(x*x+y*y))))


def rotation_degrees(degrees):
    # Same quaternion construction/order as HandPoseMath.h::rotationDegrees.
    hx, hy, hz = np.asarray(degrees) * math.pi / 360
    sx, sy, sz = np.sin((hx, hy, hz))
    cx, cy, cz = np.cos((hx, hy, hz))
    return quaternion_matrix((sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz,
                              cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz))


def key_values(path):
    result = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if '=' in line:
            key, value = line.split('=', 1)
            result[key.strip()] = value.strip()
    return result


def transformed(points, matrix):
    return points @ matrix[:3, :3].T + matrix[:3, 3]


def load_runtime_hand(runtime, physical, relaxed):
    """GLB node baking + optional production morphs + accepted free-hand matrix."""
    config = key_values(runtime / 'hand_models.cfg')
    calibration = dict(config)
    for name in ('hand_models_calibration_default.cfg', 'hand_models_calibration_saved.cfg'):
        if (runtime / name).exists():
            calibration.update(key_values(runtime / name))
    path = runtime / config[physical + '_fist']
    raw = path.read_bytes()
    json_length = struct.unpack_from('<I', raw, 12)[0]
    document = json.loads(raw[20:20+json_length])
    binary = raw[28+json_length:]
    nodes = document.get('nodes', [])
    parents = {child: index for index, node in enumerate(nodes) for child in node.get('children', [])}

    def node_matrix(index):
        node = nodes[index]
        if 'matrix' in node:
            matrix = np.array(node['matrix'], dtype=float).reshape(4, 4).T
        else:
            matrix = np.eye(4)
            matrix[:3, :3] = quaternion_matrix(node.get('rotation', (0, 0, 0, 1))) @ np.diag(node.get('scale', (1, 1, 1)))
            matrix[:3, 3] = node.get('translation', (0, 0, 0))
        return node_matrix(parents[index]) @ matrix if index in parents else matrix

    scene_roots = document['scenes'][document.get('scene', 0)].get('nodes', [])
    def in_scene(index):
        while index in parents:
            index = parents[index]
        return index in scene_roots
    index = next(i for i, node in enumerate(nodes) if 'mesh' in node and in_scene(i))
    primitive = document['meshes'][nodes[index]['mesh']]['primitives'][0]
    if 'skin' in nodes[index]:
        raise ValueError('Review requires already-baked runtime GLBs, not an unbaked skin')

    def accessor(number, components):
        item = document['accessors'][number]
        view = document['bufferViews'][item['bufferView']]
        dtype = {5121: 'u1', 5123: '<u2', 5125: '<u4', 5126: '<f4'}[item['componentType']]
        offset = view.get('byteOffset', 0) + item.get('byteOffset', 0)
        stride = view.get('byteStride', np.dtype(dtype).itemsize * components)
        return np.ndarray((item['count'], components), dtype=dtype, buffer=binary,
                          offset=offset, strides=(stride, np.dtype(dtype).itemsize)).copy()

    positions = transformed(accessor(primitive['attributes']['POSITION'], 3), node_matrix(index))
    triangles = accessor(primitive['indices'], 1).reshape(-1, 3)
    uvs = accessor(primitive['attributes']['TEXCOORD_0'], 2)
    uvs[:, 1] = 1 - uvs[:, 1]
    morph_path = path.with_name('DOOM_' + physical.upper() + '_HAND_FINGERS.bin')
    morph_applied = False
    if relaxed and morph_path.exists():
        morph = morph_path.read_bytes()
        expected_hash = 14695981039346656037
        for value in raw:
            expected_hash = ((expected_hash ^ value) * 1099511628211) & ((1 << 64)-1)
        if (morph[:8] != b'KHFING02' or len(morph) != 216 + len(positions)*18*4
                or struct.unpack_from('<I', morph, 8)[0] != len(positions)
                or struct.unpack_from('<Q', morph, 12)[0] != expected_hash):
            raise ValueError('Morph does not match the production Fist GLB')
        deltas = np.frombuffer(morph, dtype='<f4', offset=216).reshape(3, len(positions), 6)
        # Zero trigger/squeeze -> openness=(1,1,1), exactly handDrawPush.
        positions += deltas[:, :, :3].sum(axis=0)
        morph_applied = True
    matrix = np.eye(4)
    scale = float(calibration[physical + '_scale'])
    mirror = config.get(physical + '_fist_mirror_x') == '1'
    matrix[:3, :3] = rotation_degrees([float(v) for v in calibration[physical + '_rotation'].split()]) @ np.diag((-scale if mirror else scale, scale, scale))
    matrix[:3, 3] = [float(v) for v in calibration[physical + '_position'].split()]
    positions = transformed(positions, matrix) * 100  # all review geometry in cm
    if not np.isfinite(positions).all():
        raise ValueError('Non-finite production hand coordinates')
    return positions, triangles, uvs, dict(path=str(path), sha256=hashlib.sha256(raw).hexdigest(),
        free_morph_applied=morph_applied, source_node_matrix=node_matrix(index).tolist(),
        raw_grip_to_baked_mesh_matrix_meters=matrix.tolist(),
        bounds_cm=[positions.min(axis=0).tolist(), positions.max(axis=0).tolist()])


def hud_basis(degrees):
    # Exact row-based intrinsic rotation in OffhandHudPolicy.h, not Euler XYZ.
    local = np.eye(3)
    for angle, (x, y) in zip(degrees, ((0, 2), (0, 1), (1, 2))):
        cosine, sine = math.cos(math.radians(angle)), math.sin(math.radians(angle))
        u, v = local[x].copy(), local[y].copy()
        local[x], local[y] = cosine*u+sine*v, -sine*u+cosine*v
    return local @ DOOM_TO_XR.T


def rectangle_depth_intervals(points, triangles, center, basis, width, height):
    """Clip triangles to the complete assumed canvas footprint, in cm.

    Each clipped polygon gives the normal-distance interval in which its
    triangle intersects the translated rectangle. No mesh vertices are sampled
    as a substitute for triangle intersections.
    """
    normal, axis_x, axis_y = np.asarray(basis)
    local = (points-center) @ np.column_stack((axis_x, axis_y, normal))
    faces = local[triangles]
    w, h = width/2, height/2
    keep = ((faces[:, :, 0].min(1) <= w) & (faces[:, :, 0].max(1) >= -w)
            & (faces[:, :, 1].min(1) <= h) & (faces[:, :, 1].max(1) >= -h))
    intervals = []
    for triangle in faces[keep]:
        polygon = list(triangle)
        for axis, sign, bound in ((0, 1, w), (0, -1, w), (1, 1, h), (1, -1, h)):
            clipped = []
            if not polygon:
                break
            for i, end in enumerate(polygon):
                start = polygon[i-1]
                a, b = bound-sign*start[axis], bound-sign*end[axis]
                if (a >= 0) != (b >= 0):
                    clipped.append(start+(end-start)*a/(a-b))
                if b >= 0:
                    clipped.append(end)
            polygon = clipped
        if polygon:
            depths = np.array(polygon)[:, 2]
            intervals.append((float(depths.min()), float(depths.max())))
    return intervals


def minimum_positive_normal_shift(intervals, margin=.2):
    # Enlarge each forbidden interval by 2mm; leave the first connected union
    # containing the original position. This is the first admissible +N shift.
    distance = 0.
    for lower, upper in sorted((a-margin, b+margin) for a, b in intervals):
        if lower <= distance <= upper:
            distance = upper+1e-5
    return distance


def clearance_audit(original=ORIGINAL, praetor=PRAETOR):
    original, praetor = pathlib.Path(original), pathlib.Path(praetor)
    cfg_path = original / 'offhand_hud.cfg'
    cfg = cfg_path.read_bytes()
    if cfg != (praetor / 'offhand_hud.cfg').read_bytes():
        raise ValueError('Original and Praetor HUD calibrations differ; review before proposing offsets')
    words = cfg.decode('utf-8').split()
    if words[:2] != ['3', '1'] or len(words) != 44:
        raise ValueError('Expected enabled v3 HUD calibration with six complete rows')
    rows = np.array([float(value) for value in words[2:]]).reshape(6, 7)
    geometry, hands, surfaces = {}, {}, []
    for physical in ('left', 'right'):
        geometry[physical] = {}
        for label, root, relaxed in (('original', original, False), ('praetor', praetor, True)):
            points, triangles, uvs, audit = load_runtime_hand(root, physical, relaxed)
            geometry[physical][label] = (points, triangles, uvs)
            hands[physical + '_' + label] = audit
    for index, row in enumerate(rows):
        surface, left_handed = index // 2, bool(index % 2)
        physical = 'right' if left_handed else 'left'
        center_doom = row[:3].copy()
        center_doom[1] += 14 if surface == 0 else -14 if surface == 1 else 0
        center = DOOM_TO_XR @ center_doom
        direction, distance = unit(center), np.linalg.norm(center)
        width = (20 if surface == 2 else 25) * row[6] / .4
        # For ProgMeter .20m is content width, but exact content aspect is live.
        # 512/300 below is its native canvas technical envelope, not content art.
        height = width * 300 / 512
        radius = math.hypot(width, height) / 2
        old_extent = float(np.max(geometry[physical]['original'][0] @ direction))
        new_extent = float(np.max(geometry[physical]['praetor'][0] @ direction))
        offset = max(0., new_extent-old_extent)
        before, after = distance-radius-new_extent, distance+offset-radius-new_extent
        surfaces.append(dict(slot=index, surface=('Life', 'Ammo', 'ProgMeter')[surface],
            left_handed=left_handed, physical_hand=physical, original_row=row.tolist(),
            original_center_raw_xr_cm=center.tolist(), controller_hud_basis_rows_xr=hud_basis(row[3:6]).tolist(),
            width_cm=width, height_cm=height, orientation_independent_radius_cm=radius,
            offset_direction_raw_xr=direction.tolist(), proposed_offset_cm=offset,
            proposed_center_raw_xr_cm=(center+direction*offset).tolist(),
            proposed_cfg_centimeters=(row[:3]+DOOM_TO_XR.T @ (direction*offset)).tolist(),
            original_directional_extent_cm=old_extent, praetor_directional_extent_cm=new_extent,
            original_separating_bound_cm=distance-radius-old_extent,
            praetor_before_bound_cm=before, praetor_after_bound_cm=after,
            conservative_canvas_clearance_proven=bool(surface != 2 and after > 0),
            proof='positive bound separates every orientation of the full canvas from every mesh vertex; negative bound is inconclusive, not a measured intersection'))
        record = surfaces[-1]
        record['orientation_independent_reference_only'] = {
            key: record[key] for key in ('proposed_offset_cm', 'proposed_center_raw_xr_cm',
                'original_separating_bound_cm', 'praetor_before_bound_cm', 'praetor_after_bound_cm',
                'conservative_canvas_clearance_proven')}
        for key in ('original_directional_extent_cm', 'praetor_directional_extent_cm',
                    'original_separating_bound_cm', 'praetor_before_bound_cm', 'praetor_after_bound_cm',
                    'conservative_canvas_clearance_proven'):
            record.pop(key)
        basis = hud_basis(row[3:6])
        old_intervals = rectangle_depth_intervals(*geometry[physical]['original'][:2], center, basis, width, height)
        new_intervals = rectangle_depth_intervals(*geometry[physical]['praetor'][:2], center, basis, width, height)
        offset = minimum_positive_normal_shift(new_intervals)
        proposed = center+basis[0]*offset
        record.update(plane_axis_hypothesis='canvas X=controllerHUD row1; canvas Y=row2; normal=row0; native GUI localAxisDirections unknown',
            original_canvas_intersecting_triangles=sum(a <= 0 <= b for a, b in old_intervals),
            praetor_before_canvas_intersecting_triangles=sum(a <= 0 <= b for a, b in new_intervals),
            praetor_after_canvas_intersecting_triangles=sum(a <= offset <= b for a, b in new_intervals),
            normal_margin_cm=.2, proposed_offset_cm=offset, offset_direction_raw_xr=basis[0].tolist(),
            proposed_center_raw_xr_cm=proposed.tolist(),
            proposed_cfg_centimeters=(row[:3]+DOOM_TO_XR.T @ (basis[0]*offset)).tolist(),
            native_axes_and_opaque_pixels_validated=False,
            proof='no triangle crosses the complete assumed rectangle after the minimum positive-normal translation; this is conservative full-canvas geometry, not native opaque HUD pixel clearance')
    audit = dict(status='technical envelopes only; original native SWF artwork and in-plane axes are not recreated',
        units='centimeters in identity OpenXR raw Grip frame', calibration_path=str(cfg_path),
        calibration_sha256=hashlib.sha256(cfg).hexdigest(), original_runtime=str(original), praetor_runtime=str(praetor),
        method='preserve original centers, angles and scales as reference; propose only minimum +normal translation for full-canvas triangle clearance under explicitly assumed GUI in-plane axes, with 2mm normal margin',
        hands=hands, surfaces=surfaces,
        limitations=['HUD native localAxisDirections is captured from the live game and is not present in cfg; rectangle orientation is explicitly hypothetical',
                     'Full native canvases include transparent pixels: intersection with the original model does not prove visible original HUD clipping',
                     'Measured conservative shifts can be 8-15cm; these are not a validated small compensation and are not applied to the game',
                     'ProgMeter content aspect is live: its 512x300 envelope is only a technical canvas reference; no content-clearance proof is asserted',
                     'Neutral free-hand comparison only; locked weapon support moves the hand independently of the raw-controller HUD anchor',
                     'Original static Fist endpoint has no authored neutral morph file; Praetor uses all production free-pose deltas',
                     'Offsets are offline proposals, not applied runtime changes; animations and game visibility require headset validation'])
    return audit, geometry


def curve(review, scene, name, points, color, closed=False, thickness=.07):
    data = bpy.data.curves.new(name, 'CURVE')
    data.dimensions, data.bevel_depth, data.resolution_u = '3D', thickness, 1
    spline = data.splines.new('POLY')
    spline.points.add(len(points)-1)
    for point, value in zip(spline.points, points):
        point.co = (*value, 1)
    spline.use_cyclic_u = closed
    obj = bpy.data.objects.new(name, data)
    scene.collection.objects.link(obj)
    data.materials.append(review.plain_material(name, color, True))
    return obj


def panel_corners(center, basis, width, height):
    return np.array([center+basis[1]*x*width/2+basis[2]*y*height/2
                     for x, y in ((-1, -1), (1, -1), (1, 1), (-1, 1))])


def build_hud_review(review, draft=False, render=True, original=ORIGINAL, praetor=PRAETOR):
    """Return JSON-ready audit; create only the two announced images if render."""
    audit, geometry = clearance_audit(original, praetor)
    if not render:
        return audit
    if pathlib.Path(review.OUT).resolve() != OUT.resolve():
        raise ValueError('HUD preview may only write its announced v2 output directory')
    OUT.mkdir(parents=True, exist_ok=True)
    scene = bpy.data.scenes.new('HUD original - plans sous hypothese GUI')
    bpy.context.window.scene = scene
    textures = ROOT / 'build/praetor-hands'
    new_material = review.texture_material('Praetor natif - materiau de controle',
        textures/'BaseColor.png', textures/'Normal.png', textures/'MetallicRoughness.png')
    colors = ((.18, .8, .95), (1., .55, .12), (.65, .42, 1.))
    all_points = []
    points, triangles, uvs = geometry['left']['praetor']
    review.mesh_object('Main gauche Praetor - pose libre runtime', points, triangles, uvs, new_material, scene.collection)
    all_points.append(points)
    for surface in (row for row in audit['surfaces'] if row['slot'] in (0, 2)):
        old_center = np.array(surface['original_center_raw_xr_cm'])
        center = np.array(surface['proposed_center_raw_xr_cm'])
        basis = np.array(surface['controller_hud_basis_rows_xr'])
        old_corners = panel_corners(old_center, basis, surface['width_cm'], surface['height_cm'])
        new_corners = panel_corners(center, basis, surface['width_cm'], surface['height_cm'])
        curve(review, scene, surface['surface']+' - canevas original', old_corners, (.5, .53, .57), True, .08)
        curve(review, scene, surface['surface']+' - translation normale proposee', new_corners,
              colors[surface['slot']//2], True, .12)
        curve(review, scene, surface['surface']+' - seule translation', (old_center, center), (1, .9, .25), thickness=.10)
        for axis in (1, 2):
            curve(review, scene, surface['surface']+' - centre original',
                  (old_center-basis[axis]*.7, old_center+basis[axis]*.7), (.6, .63, .67), thickness=.10)
            curve(review, scene, surface['surface']+' - centre propose',
                  (center-basis[axis]*.7, center+basis[axis]*.7), colors[surface['slot']//2], thickness=.10)
        all_points.extend((old_corners, new_corners))
    cloud = np.concatenate(all_points)
    center = (cloud.min(axis=0)+cloud.max(axis=0))/2
    span = float(np.max(np.ptp(cloud, axis=0)))*1.1
    review.lighting(scene, center, span)
    note = 'Canevas techniques / axes GUI supposes / graphismes natifs conserves en jeu'
    review.render_view(scene, 'hud-praetor', center, span, draft, 'HUD / ORIGINE GRIS - PROPOSITION COULEUR', note, (-2, .25, .5))
    review.render_view(scene, 'hud-praetor-profil', center, span, draft, 'HUD / DEGAGEMENT MESURE',
                       'Trait jaune: translation seule (+10.8 / +12.6 cm), conservative canevas entier; non validee en jeu.', (.2, .1, 2))
    audit['images'] = [str(OUT / name) for name in ('hud-praetor.png', 'hud-praetor-profil.png')]
    return audit
