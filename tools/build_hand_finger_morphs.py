"""Run with Blender --background --python this_file -- [--inspect].

Derive three finger morphs and wrist/grasp frames from KHARVOX's own rigs.
The original four GLBs and .blend sources are never changed. UVs establish
correspondence with the shipped baked meshes; reject a mismatched rig/export.
"""
import json
import pathlib
import struct
import sys

import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector
from mathutils.kdtree import KDTree

ROOT = pathlib.Path(__file__).resolve().parents[1]
ASSETS = ROOT / "assets" / "3D"


def glb(path):
    raw = path.read_bytes()
    size = struct.unpack_from("<I", raw, 12)[0]
    document = json.loads(raw[20:20 + size])
    binary = raw[28 + size:]
    def accessor(index):
        a = document["accessors"][index]
        v = document["bufferViews"][a["bufferView"]]
        width = {"VEC2": 2, "VEC3": 3}[a["type"]]
        assert a["componentType"] == 5126
        return np.ndarray((a["count"], width), dtype="<f4", buffer=binary,
            offset=v.get("byteOffset", 0) + a.get("byteOffset", 0),
            strides=(v.get("byteStride", width * 4), 4)).copy()
    scene = document["scenes"][document.get("scene", 0)]
    node = next(document["nodes"][i] for i in scene["nodes"] if "mesh" in document["nodes"][i])
    q = node.get("rotation", [0, 0, 0, 1])
    transform = Matrix.Translation(node.get("translation", [0, 0, 0])) @ Quaternion((q[3], *q[:3])).to_matrix().to_4x4()
    transform @= Matrix.Diagonal((*node.get("scale", [1, 1, 1]), 1))
    primitive = document["meshes"][node["mesh"]]["primitives"][0]
    p = accessor(primitive["attributes"]["POSITION"])
    matrix = np.array(transform, dtype=float)
    positions = p @ matrix[:3, :3].T + matrix[:3, 3]
    return raw, positions, accessor(primitive["attributes"]["TEXCOORD_0"])


def evaluated(obj):
    bpy.context.view_layer.update()
    item = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = item.to_mesh()
    transform = np.array(item.matrix_world, dtype=float)
    positions = np.array([v.co[:] for v in mesh.vertices]) @ transform[:3, :3].T + transform[:3, 3]
    normals = np.array([v.vector[:] for v in mesh.vertex_normals]) @ np.linalg.inv(transform[:3, :3])
    normals /= np.maximum(np.linalg.norm(normals, axis=1)[:, None], 1e-12)
    item.to_mesh_clear()
    return positions, normals


def correspond(obj, positions, uvs):
    source, normals = evaluated(obj)
    uv_map = {}
    layer = obj.data.uv_layers.active
    for loop in obj.data.loops:
        u, v = layer.data[loop.index].uv
        uv_map.setdefault((round(u, 5), round(1-v, 5)), set()).add(loop.vertex_index)
    pairs = [(next(iter(uv_map[key])), i) for i, uv in enumerate(uvs)
        if len(uv_map.get(key := tuple(round(float(v), 5) for v in uv), ())) == 1]
    if len(pairs) < 100:
        raise RuntimeError("Too few unique UV correspondences for " + obj.name)
    a = np.array([(*source[i], 1) for i, _ in pairs])
    b = np.array([positions[j] for _, j in pairs])
    keep = np.ones(len(a), dtype=bool)
    for _ in range(4):
        fit = np.linalg.lstsq(a[keep], b[keep], rcond=None)[0]
        errors = np.linalg.norm(a @ fit - b, axis=1)
        keep = errors <= max(float(np.quantile(errors, .85)), 1e-5)
    matrix = np.eye(4)
    matrix[:3, :] = fit.T
    transformed = source @ matrix[:3, :3].T + matrix[:3, 3]
    tree = KDTree(len(source))
    for i, p in enumerate(transformed):
        tree.insert(p, i)
    tree.balance()
    mapping = []
    maximum = 0.0
    for p in positions:
        _, index, error = tree.find(p)
        mapping.append(index)
        maximum = max(maximum, error)
    print("correspondence", obj.name, "uv_pairs", len(pairs), "max_distance", maximum, flush=True)
    if maximum > .002:
        raise RuntimeError("The rig does not match the baked hand: " + str(maximum))
    return matrix, np.array(mapping), source, normals


def rig_for(obj):
    return next(m.object for m in obj.modifiers if m.type == "ARMATURE" and m.object)


def hand_side(obj):
    sums = {"L": 0., "R": 0.}
    for v in obj.data.vertices:
        for g in v.groups:
            name = obj.vertex_groups[g.group].name
            if name.endswith((".L", ".R")):
                sums[name[-1]] += g.weight
    return max(sums, key=sums.get)


def fnv(raw):
    result = 14695981039346656037
    for value in raw:
        result = ((result ^ value) * 1099511628211) & 0xffffffffffffffff
    return result


def grasp_centre(rig, side, fit):
    roots, tips = [], []
    for finger in ["index", "middle", "ring", "pinky"]:
        root = rig.pose.bones["DEF-f_" + finger + ".01." + side]
        tip = rig.pose.bones["DEF-f_" + finger + ".03." + side]
        roots.append(np.array(rig.matrix_world @ root.head))
        tips.append(np.array(rig.matrix_world @ tip.tail))
    centre = (np.mean(roots, axis=0) + np.mean(tips, axis=0)) * .5
    return fit[:3, :3] @ centre + fit[:3, 3]


def build(left, inspect):
    hand = "LEFT" if left else "RIGHT"
    bpy.ops.wm.open_mainfile(filepath=str(ASSETS / ("HandLeft.blend" if left else "HandRight.blend")))
    prefix = "L" if left else "R"
    scene = bpy.data.scenes[prefix + "_Pose_1"]
    bpy.context.window.scene = scene
    obj = next(o for o in scene.objects if o.type == "MESH" and len(o.data.vertices) > 1000)
    rig, side = rig_for(obj), hand_side(obj)
    raw_fist, positions, uvs = glb(ASSETS / ("DOOM_" + hand + "_HAND_FIST.glb"))
    fit, mapping, closed, closed_normals = correspond(obj, positions, uvs)
    wrist = np.array(rig.matrix_world @ rig.pose.bones["DEF-hand." + side].matrix)
    fist_wrist = fit @ wrist
    fist_centre = grasp_centre(rig, side, fit)
    bones = [b for b in rig.pose.bones if b.name.startswith(("DEF-f_", "DEF-thumb.")) and b.name.endswith("." + side)]
    original = {b.name: b.matrix_basis.copy() for b in bones}
    closed_matrices = {b.name: b.matrix.copy() for b in rig.pose.bones}
    constraints = [(c, c.mute) for b in bones for c in b.constraints]
    morphs = []
    grasp_deltas = []
    for prefixes in [("DEF-f_index.",), ("DEF-f_middle.", "DEF-f_ring.", "DEF-f_pinky."), ("DEF-thumb.",)]:
        for c, muted in constraints:
            c.mute = muted
        for b in bones:
            b.matrix_basis = original[b.name]
        bpy.context.view_layer.update()
        selected = [b for b in bones if b.name.startswith(prefixes)]
        for b in selected:
            for c in b.constraints:
                c.mute = True
        basis = rig.pose.bones["DEF-hand." + side].matrix @ rig.data.bones["DEF-hand." + side].matrix_local.inverted()
        relaxed_globals = {b.name: basis @ b.bone.matrix_local for b in selected}
        for b in sorted(selected, key=lambda b: len(b.parent_recursive)):
            # Pose each phalanx on the actual rig. Interpolating far-apart
            # baked vertices flattens/shortens fingers instead of bending joints.
            parent_closed = closed_matrices[b.parent.name] if b.parent else Matrix.Identity(4)
            parent_open = relaxed_globals.get(b.parent.name, parent_closed) if b.parent else Matrix.Identity(4)
            closed_local = parent_closed.inverted() @ closed_matrices[b.name]
            open_local = parent_open.inverted() @ relaxed_globals[b.name]
            segment = b.name.split('.')[1]
            fraction = {"01": .40, "02": .65, "03": .40}.get(segment, .4)
            if b.name.startswith("DEF-thumb."):
                fraction = {"01": .45, "02": .40, "03": .30}.get(segment, .4)
            rotation = open_local.to_quaternion().slerp(closed_local.to_quaternion(), fraction)
            local = Matrix.LocRotScale(closed_local.to_translation(), rotation, closed_local.to_scale())
            parent_now = b.parent.matrix if b.parent else Matrix.Identity(4)
            b.matrix = parent_now @ local
            bpy.context.view_layer.update()
        opened, open_normals = evaluated(obj)
        grasp_deltas.append(grasp_centre(rig, side, fit) - fist_centre)
        delta = (opened - closed) @ fit[:3, :3].T
        # Transform the normal change as a direction, then normalize in shader.
        normal_delta = (open_normals - closed_normals) @ np.linalg.inv(fit[:3, :3])
        morph = np.concatenate((delta[mapping], normal_delta[mapping]), axis=1)
        print("morph", hand, prefixes, "max_delta", float(np.linalg.norm(morph[:, :3], axis=1).max()), flush=True)
        if not np.isfinite(morph).all() or np.linalg.norm(morph[:, :3], axis=1).max() < .02:
            raise RuntimeError("Finger opening did not deform the hand")
        morphs.append(morph)
    bpy.context.window.scene = bpy.data.scenes[prefix + "_Pose_2"]
    gun = next(o for o in bpy.context.scene.objects if o.type == "MESH" and len(o.data.vertices) > 1000)
    raw_gun, gun_positions, gun_uvs = glb(ASSETS / ("DOOM_" + hand + "_HAND_GUN.glb"))
    gun_fit, _, _, _ = correspond(gun, gun_positions, gun_uvs)
    gun_rig, gun_side = rig_for(gun), hand_side(gun)
    gun_wrist = gun_fit @ np.array(gun_rig.matrix_world @ gun_rig.pose.bones["DEF-hand." + gun_side].matrix)
    centre = grasp_centre(gun_rig, gun_side, gun_fit)
    print("grasp", hand, centre.tolist(), "rig_side", side, gun_side, flush=True)
    if not inspect:
        header = struct.pack("<8sIQQ32f15f", b"KHFING02", len(positions), fnv(raw_fist), fnv(raw_gun),
            *fist_wrist.T.flatten(), *gun_wrist.T.flatten(), *centre, *fist_centre, *np.array(grasp_deltas).flatten())
        output = ASSETS / ("DOOM_" + hand + "_HAND_FINGERS.bin")
        output.write_bytes(header + np.array(morphs, dtype="<f4").tobytes())
        print("wrote", output, output.stat().st_size, flush=True)


if __name__ == "__main__":
    for left in [True, False]:
        build(left, "--inspect" in sys.argv)
