"""Bake accepted Blender support poses to static, weapon-native runtime GLBs.

Run in Blender --background --factory-startup --python-exit-code 1 --python FILE.
Only build/praetor-runtime-grips is written. Material image URIs are symbolic:
production loadAsset(..., physicalHandFist) supplies the already loaded atlas.
These geometry-only files do not carry or duplicate native texture payloads.
"""
import hashlib
import json
import pathlib
import struct
import sys

import bpy
import numpy as np
from mathutils.bvhtree import BVHTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/praetor-runtime-grips'
WEAPONS = (
    ('shotgun', 'shotgun', 'FUSIL A POMPE'),
    ('heavy-ar', 'heavy_assault_rifle', "FUSIL D'ASSAUT LOURD"),
    ('plasma', 'plasma_rifle', 'FUSIL A PLASMA'),
    ('rocket', 'rocket_launcher', 'LANCE-ROQUETTES'),
    ('super-shotgun', 'super_shotgun', 'SUPER SHOTGUN'),
    ('gauss', 'gauss_cannon', 'CANON GAUSS'),
    ('chaingun', 'chaingun', 'MITRAILLEUSE'),
    ('bfg', 'bfg', 'BFG 9000'),
    ('chainsaw', 'chainsaw', 'TRONCONNEUSE'),
)
MIRROR_PLANE_Y = {'gauss_cannon': 34.45214040, 'chaingun': 31.436299, 'chainsaw': 16.6}


def weapon_bvh(scene, hand, builder):
    points, faces = [], []
    count = 0
    for obj in scene.objects:
        if obj.type != 'MESH' or obj == hand:
            continue
        vertices, _, mapping = builder['bake'](obj, np.eye(4))
        triangles = mapping[3].reshape(-1, 3)
        points.extend(vertices.tolist())
        faces.extend((triangles+count).tolist())
        count += len(vertices)
    if not faces:
        raise RuntimeError('Reviewed scene has no original weapon geometry')
    return BVHTree.FromPolygons(points, faces, all_triangles=True, epsilon=.002)


def fit_right_clearance(points, triangles, weapon_tree, grip):
    def count(candidate):
        tree = BVHTree.FromPolygons(candidate.tolist(), triangles.tolist(), all_triangles=True, epsilon=.002)
        return len(weapon_tree.overlap(tree))
    initial = count(points)
    if initial == 0:
        return points, np.zeros(3), np.eye(3), 0., initial, 0
    # Small, offline-only translations along the existing handle/height axes.
    # Never reflect the weapon and never change the already accepted left mesh.
    candidates = [np.array((0., y/2, z/2)) for y in range(-6, 7) for z in range(-6, 7)]
    candidates.sort(key=lambda shift: (float(shift@shift), abs(shift[2]), abs(shift[1])))
    best = (initial, points, np.zeros(3), np.eye(3), 0.)
    for angle in (0., 15., -15., 30., -30.):
        cosine, sine = np.cos(np.deg2rad(angle)), np.sin(np.deg2rad(angle))
        rotation = np.array(((cosine, 0, sine), (0, 1, 0), (-sine, 0, cosine)))
        rotated = (points-grip) @ rotation.T+grip
        for shift in candidates:
            candidate = rotated+shift
            crossings = count(candidate)
            if crossings < best[0]:
                best = crossings, candidate, shift, rotation, angle
            if crossings == 0:
                return candidate, shift, rotation, angle, initial, 0
    crossings, candidate, shift, rotation, angle = best
    # Keep an inspectable offline result, but caller disables this model in cfg.
    return candidate, shift, rotation, angle, initial, crossings


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fnv64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def geometry_glb(builder, positions, normals, uvs, indices, name, provenance):
    # Reuse the existing GLB geometry/material contract. Replace its three
    # empty embedded image views with legal external URI image declarations.
    raw = builder['glb'](positions, normals, uvs, indices, name, (b'', b'', b''), provenance)
    length = struct.unpack_from('<I', raw, 12)[0]
    document = json.loads(raw[20:20+length])
    binary = raw[28+length:]
    if len(document['bufferViews']) != 7 or any(view['byteLength'] for view in document['bufferViews'][4:]):
        raise RuntimeError('Unexpected production GLB builder texture contract')
    document['bufferViews'] = document['bufferViews'][:4]
    document['images'] = [{'uri': name} for name in ('BaseColor.png', 'Normal.png', 'MetallicRoughness.png')]
    document['asset']['generator'] = 'KHARVOX reviewed static weapon-space support adapter'
    encoded = json.dumps(document, separators=(',', ':')).encode('utf-8')
    encoded += b' ' * (-len(encoded) % 4)
    result = (struct.pack('<III', 0x46546c67, 2, 28+len(encoded)+len(binary))
              + struct.pack('<II', len(encoded), 0x4e4f534a)+encoded
              + struct.pack('<II', len(binary), 0x004e4942)+binary)
    return result


def validate_glb(raw, expected_positions, expected_triangles):
    if struct.unpack_from('<III', raw) != (0x46546c67, 2, len(raw)):
        raise RuntimeError('Invalid GLB envelope')
    length = struct.unpack_from('<I', raw, 12)[0]
    document = json.loads(raw[20:20+length])
    binary = raw[28+length:]
    if len(document['bufferViews']) != 4 or any('bufferView' in image for image in document['images']):
        raise RuntimeError('Support GLB must contain geometry only, no texture payload')
    def accessor(index, columns, dtype):
        item = document['accessors'][index]
        view = document['bufferViews'][item['bufferView']]
        return np.frombuffer(binary, dtype=dtype, count=item['count']*columns,
            offset=view.get('byteOffset', 0)).reshape(-1, columns)
    positions = accessor(0, 3, '<f4')
    normals = accessor(1, 3, '<f4')
    uvs = accessor(2, 2, '<f4')
    triangles = accessor(3, 1, '<u4').reshape(-1, 3)
    if (not np.allclose(positions, expected_positions, atol=2e-5, rtol=2e-6)
            or not np.array_equal(triangles, expected_triangles)):
        raise RuntimeError('Export geometry differs from accepted Blender geometry')
    if (not np.isfinite(positions).all() or not np.isfinite(normals).all()
            or not np.isfinite(uvs).all() or triangles.max() >= len(positions)
            or np.max(np.abs(np.linalg.norm(normals, axis=1)-1)) > 1e-4):
        raise RuntimeError('Invalid exported attributes/indices/normals')
    if document['nodes'] != [{'mesh': 0, 'rotation': [0, 0, 0, 1], 'name': document['nodes'][0]['name']}]:
        raise RuntimeError('Support GLB node must be identity; placement is already baked')
    return dict(vertices=len(positions), triangles=len(triangles), bytes=len(raw),
                bounds_native_units=[positions.min(axis=0).tolist(), positions.max(axis=0).tolist()],
                max_normal_length_error=float(np.max(np.abs(np.linalg.norm(normals, axis=1)-1))))


def main():
    builder_path = ROOT / 'KHARVOX/tools/build_praetor_hands.py'
    builder = {'__file__': str(builder_path), '__name__': 'runtime_support_builder'}
    exec(compile(builder_path.read_text(encoding='utf-8'), str(builder_path), 'exec'), builder)
    source_sets = {
        'v1': (ROOT/'build/praetor-placement-review/praetor-weapons.blend',
               ROOT/'build/praetor-placement-review/placements.json'),
        'v2': (ROOT/'build/praetor-placement-review-v2/praetor-revision2.blend',
               ROOT/'build/praetor-placement-review-v2/placements.json')}
    source_info = {key: dict(blend=str(blend), blend_sha256=sha256(blend),
                            placements=str(placement), placements_sha256=sha256(placement))
                   for key, (blend, placement) in source_sets.items()}
    manifests = {key: json.loads(placement.read_text(encoding='utf-8'))['weapons']
                 for key, (_, placement) in source_sets.items()}
    (OUT/'assets/3D').mkdir(parents=True, exist_ok=True)
    rows, entries, messages = {}, {}, []
    def log(message):
        print(message, flush=True)
        messages.append(message)
    # Open each accepted blend once; no fitting/rigging or per-frame work.
    for source_key in ('v1', 'v2'):
        bpy.ops.wm.open_mainfile(filepath=str(source_sets[source_key][0]))
        for review_key, weapon, title in WEAPONS:
            if ('v2' if review_key in ('gauss', 'chaingun') else 'v1') != source_key:
                continue
            entry = manifests[source_key][review_key]
            source_cast = pathlib.Path(entry['source'])
            if sha256(source_cast) != entry['source_sha256']:
                raise RuntimeError('Accepted weapon source hash mismatch: '+weapon)
            if entry['crossing_triangle_pairs'] != 0:
                raise RuntimeError('Accepted pose contains recorded crossings: '+weapon)
            scene = bpy.data.scenes[title]
            bpy.context.window.scene = scene
            objects = [obj for obj in scene.objects if obj.type == 'MESH' and obj.name.startswith('Main Praetor')]
            if len(objects) != 1:
                raise RuntimeError('Expected one reviewed support mesh in scene '+title)
            obj = objects[0]
            expected_matrix = np.array(entry['hand_matrix'])
            matrix = np.array(obj.matrix_world)
            if not np.allclose(matrix, expected_matrix, atol=1e-5, rtol=1e-6):
                raise RuntimeError('Blender hand matrix differs from accepted JSON: '+weapon)
            # bake() applies obj.matrix_world, inverse-transpose normals and
            # per-corner splitting. corner_mapping converts Blender UV v to1-v.
            positions, normals, mapping = builder['bake'](obj, np.eye(4))
            _, _, uvs, indices = mapping
            triangles = indices.reshape(-1, 3)
            tree = weapon_bvh(scene, obj, builder)
            left_tree = BVHTree.FromPolygons(positions.tolist(), triangles.tolist(), all_triangles=True, epsilon=.002)
            left_crossings = len(tree.overlap(left_tree))
            if left_crossings:
                raise RuntimeError('Accepted left pose no longer matches recorded zero crossings: '+weapon)
            point_key = 'selected_grip_point' if source_key == 'v2' else 'actual_grasp_center'
            grip = np.array(entry[point_key], dtype=float)
            if grip.shape != (3,) or not np.isfinite(grip).all():
                raise RuntimeError('Invalid accepted native grip point: '+weapon)
            plane_y = MIRROR_PLANE_Y.get(weapon, 0.)
            right_grip = grip.copy()
            right_grip[1] = 2*plane_y-grip[1]
            right_shift = np.zeros(3)
            base_provenance = dict(source='accepted Blender Praetor support pose', weapon=weapon,
                coordinate_space='native weapon model units; object.matrix_world baked',
                source_set=source_key, **source_info[source_key], scene=scene.name, object=obj.name,
                weapon_cast=str(source_cast), weapon_cast_sha256=entry['source_sha256'],
                accepted_hand_matrix=expected_matrix.tolist(),
                uv_contract='Blender uv v -> glTF 1-v; production renderer consumes glTF uv directly',
                shared_runtime_material='physical-hand fist',
                symbolic_images='URI declarations only; production must inherit physical-hand fist textures',
                support_pose_locked=True, per_frame_skinning=False)
            exported = {}
            for side in ('left', 'right'):
                points, normal, faces = positions.copy(), normals.copy(), triangles.copy()
                chirality = 'native left Praetor mesh'
                if side == 'right':
                    points[:, 1] = 2*plane_y-points[:, 1]
                    normal[:, 1] *= -1
                    faces = faces[:, (0, 2, 1)].copy()
                    points, right_shift, rotation, right_angle, before_crossings, after_crossings = fit_right_clearance(points, faces, tree, right_grip)
                    normal = normal @ rotation.T
                    right_grip += right_shift
                    chirality = 'nativeleftmirrored: reflect weapon-space Y about existing handle plane, reverse triangle winding and normal Y; weapon itself is not mirrored'
                relative = 'assets/3D/SUPPORT_'+weapon.upper()+'_'+side.upper()+'.glb'
                provenance = dict(base_provenance, physical_support=side, chirality=chirality,
                    left_handed_limit='right support is a mirrored reviewed left pose, not separately fitted right-hand native animation')
                if side == 'right':
                    provenance.update(mirror_plane_weapon_y=plane_y,
                        offline_clearance_shift_native_units=right_shift.tolist(),
                        original_weapon_not_mirrored=True, initial_crossing_triangle_pairs=before_crossings,
                        final_crossing_triangle_pairs=after_crossings,
                        offline_rotation_about_native_handle_y_degrees=right_angle,
                        right_grip_native_units=right_grip.tolist())
                raw = geometry_glb(builder, points, normal, uvs, faces.reshape(-1), weapon+'_'+side, provenance)
                validation = validate_glb(raw, points, faces)
                destination = OUT/relative
                if side == 'left' and destination.exists():
                    if destination.read_bytes() != raw:
                        raise RuntimeError('Refusing to change previously accepted left asset: '+weapon)
                else:
                    destination.write_bytes(raw)
                exported[side] = dict(path=relative, fnv1a64=fnv64(raw), sha256=hashlib.sha256(raw).hexdigest(),
                    chirality=chirality, **validation)
                log(weapon+' '+side+' vertices='+str(validation['vertices'])+' triangles='+str(validation['triangles'])
                    +' bytes='+str(len(raw))+' FNV64='+str(exported[side]['fnv1a64']))
            right_enabled = after_crossings == 0
            right_path = exported['right']['path'] if right_enabled else '-'
            right_hash = exported['right']['fnv1a64'] if right_enabled else 0
            rows[weapon] = ('2 '+weapon+' '+exported['left']['path']+' '+right_path
                +' '+str(exported['left']['fnv1a64'])+' '+str(right_hash)
                +' '+' '.join(format(value, '.12g') for value in (*grip, *right_grip)))
            entries[weapon] = dict(**base_provenance, exported=exported,
                grip_source_key=point_key, canonical_right_handed_grip_native_units=grip.tolist(),
                mirrored_left_handed_grip_native_units=right_grip.tolist(),
                mirror_plane_weapon_y=plane_y, offline_right_clearance_shift_native_units=right_shift.tolist(),
                left_crossing_triangle_pairs=left_crossings, right_initial_crossing_triangle_pairs=before_crossings,
                right_final_crossing_triangle_pairs=after_crossings,
                right_support_asset_enabled=right_enabled, right_handle_rotation_y_degrees=right_angle,
                grip_mirroring='config v2 includes each physical support point; right Y=2*existing_handle_planeY-leftY, plus recorded offline shift')
            log(weapon+' canonical grip='+' '.join(format(value, '.12g') for value in grip)
                +' mirrored left-handed='+' '.join(format(value, '.12g') for value in right_grip))
            log(weapon+' RIGHT enabled='+str(right_enabled)+' crossings='+str(before_crossings)+'->'+str(after_crossings)
                +' rotationY='+str(right_angle)+' shiftYZ='+str(right_shift.tolist()))
    if set(entries) != {weapon for _, weapon, _ in WEAPONS}:
        raise RuntimeError('Not all nine weapon support poses were exported')
    (OUT/'support_hands.cfg').write_text('\n'.join(rows[weapon] for _, weapon, _ in WEAPONS)+'\n', encoding='utf-8')
    (OUT/'provenance.json').write_text(json.dumps(dict(format_version=2,
        status='static runtime support poses generated from accepted Blender review',
        config_format='2 weapon_key left_relative_glb right_relative_glb unsigned_decimal_left_fnv1a64 unsigned_decimal_right_fnv1a64 left_grip_x left_grip_y left_grip_z right_grip_x right_grip_y right_grip_z',
        native_units=True, root_node_identity=True, no_embedded_textures=True,
        features='no runtime mesh fitting, CPU skinning or per-frame file reads',
        right_support_limit='native left pose mirrored about the existing handle plane on the original non-mirrored weapon; small offline Y/Z clearance shifts recorded; headset validation required',
        weapons=entries), indent=2), encoding='utf-8')
    log('EXPORTED 9 reviewed poses /18 static geometry-only GLBs; cfg bindings and attribute round trips validated')
    (OUT/'export.log').write_text('\n'.join(messages)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
