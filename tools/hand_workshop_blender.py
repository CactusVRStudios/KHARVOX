"""Reuse the Praetor poseur and native v5 export mathematics for one new weapon."""
import sys
sys.dont_write_bytecode = True
import importlib.util
import json
import math
from pathlib import Path
import subprocess
import struct
import bpy
import numpy as np
from mathutils import Matrix, Vector, Quaternion

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


work = module('workshop_coordinator', ROOT / 'KHARVOX/tools/hand_workshop.py')
OUT = work.OUT
poseur = module('workshop_poseur', ROOT / 'KHARVOX/tools/manual_praetor_poseur.py')
builder = module('workshop_geometry', ROOT / 'KHARVOX/tools/build_praetor_hands.py')
base_payload, base_restore = poseur.payload, poseur.restore
poseur.OUT = OUT
poseur.pose_path = lambda: OUT / 'pose.json'


def model():
    sys.path.insert(0, str(ROOT / '.tools/cast/libraries/python'))
    from cast import Cast, Model
    return next(m for r in Cast.load(str(work.CAST)).Roots() for m in r.ChildrenOfType(Model))


def authoring_reference():
    """Cast authoring scene. No claim about this weapon's native game scale."""
    reference = ROOT / 'references/KHARVOX-Shotgun-Conversion-Native/pose-convertie.json'
    source = json.loads(reference.read_text(encoding='utf-8-sig'))
    bones = model().Skeleton().Bones()
    axes = []
    for bone in bones:
        q = bone.WorldRotation()
        axes.append(np.array(Quaternion((q[3], *q[:3])).to_matrix()).tolist())
    # Same exported-coordinate display units as the proven shotgun scene.
    # Identity here means unconverted Cast geometry, NOT a native scale estimate.
    return dict(coordinate_frame='cast_weapon_centimetres', export_verified=False,
        export_block_reason='Conversion native de '+work.TITLE+' non verifiee sur les donnees existantes. Pose Blender conservee ; seul cet export est bloque.',
        cast_to_native_scale=1., rotation=np.eye(3).tolist(), translation=[0., 0., 0.],
        native_cm_per_unit=source['reference_geometry_cm_per_unit'],
        display_units_source=str(reference), display_units_source_sha256=work.sha(reference),
        capture_sha256=work.sha(work.CAST), cast_sha256=work.sha(work.CAST),
        native_positions=[list(b.WorldPosition()) for b in bones], native_axes=axes,
        attachments=work.settings()['attachments'],
        note='Repere Cast non converti. Unites d affichage reprises du shotgun ; echelle native de cette arme non supposee.')


def calibration():
    path = OUT / 'conversion.json'
    if not path.is_file():
        raise ValueError('Verification de conversion absente ; seul l export est bloque.')
    data = json.loads(path.read_text(encoding='utf-8-sig'))
    if data.get('export_verified') is not True:
        raise ValueError(data.get('export_block_reason', 'Conversion non verifiee ; export bloque.'))
    if data.get('cast_sha256') != work.sha(work.CAST):
        raise ValueError('Le modele source a change ; export bloque.')
    return data


def payload():
    data = base_payload()
    data.update(weapon=work.WEAPON, coordinate_frame=bpy.context.scene.get('coordinate_frame', 'native_weapon_centimetres'),
        weapon_cm_per_unit=bpy.context.scene['native_cm_per_unit'],
        calibration_sha256=bpy.context.scene['calibration_sha256'],
        weapon_root_matrix=[list(r) for r in bpy.data.objects['ARME_NATIVE'].matrix_world],
        weapon_bones={b.name: [list(r) for r in b.matrix_basis]
                      for b in bpy.data.objects['SQUELETTE_ARME_NATIVE'].pose.bones},
        hand_attachment_joints=work.settings()['attachments'])
    return data


def restore(data):
    if data['weapon'] != work.WEAPON or data['calibration_sha256'] != bpy.context.scene['calibration_sha256']:
        raise ValueError('Cette pose appartient a une autre arme ou calibration.')
    bpy.data.objects['ARME_NATIVE'].matrix_world = Matrix(data['weapon_root_matrix'])
    for name, matrix in data['weapon_bones'].items():
        bpy.data.objects['SQUELETTE_ARME_NATIVE'].pose.bones[name].matrix_basis = Matrix(matrix)
    bpy.context.view_layer.update()
    copy = dict(data, weapon='shotgun')  # Existing poseur validates its original format.
    base_restore(copy)


def save(operator, context):
    if work.PROTECTED:
        operator.report({'ERROR'}, 'Pose validee protegee : consultation uniquement.')
        return {'CANCELLED'}
    if context.object and context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.update()
    (OUT / 'pose.json').write_text(json.dumps(payload(), indent=2), encoding='utf-8')
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'atelier.blend'))
    operator.report({'INFO'}, 'Pose et scene enregistrees dans ' + str(OUT))
    return {'FINISHED'}


class KHARVOX_OT_workshop_prepare(bpy.types.Operator):
    bl_idname = 'kharvox.workshop_prepare'
    bl_label = 'Préparer le test'

    def execute(self, context):
        if save(self, context) == {'CANCELLED'}:
            return {'CANCELLED'}
        try:
            calibration()
        except ValueError as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        log = open(OUT / 'prepare.log', 'w', encoding='utf-8')
        subprocess.Popen([str(work.BLENDER), '--background', '--factory-startup', '--python-exit-code', '1',
            '--python', str(Path(__file__)), '--', 'prepare', '--weapon', work.WEAPON], stdout=log, stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW)
        log.close()
        self.report({'INFO'}, 'Preparation en cours ; resultat dans prepare.log')
        return {'FINISHED'}


class KHARVOX_OT_workshop_launch(bpy.types.Operator):
    bl_idname = 'kharvox.workshop_launch'
    bl_label = 'Lancer le test'

    def execute(self, context):
        try:
            work.main('launch')
        except Exception as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        return {'FINISHED'}


poseur.payload, poseur.restore = payload, restore
poseur.KHARVOX_OT_save.execute = save
old_draw = poseur.KHARVOX_PT_pose.draw


def draw(self, context):
    old_draw(self, context)
    self.layout.operator('kharvox.workshop_prepare')
    self.layout.operator('kharvox.workshop_launch')


def register():
    poseur.KHARVOX_PT_pose.draw = draw
    poseur.KHARVOX_PT_pose.bl_label = 'Praetor / '+work.TITLE
    poseur.register()
    for cls in (KHARVOX_OT_workshop_prepare, KHARVOX_OT_workshop_launch):
        old = getattr(bpy.types, cls.__name__, None)
        if old:
            bpy.utils.unregister_class(old)
        bpy.utils.register_class(cls)


def build():
    if work.PROTECTED:
        raise ValueError('Scene validee protegee : aucune reconstruction.')
    c = authoring_reference()
    cfg = work.settings()
    if (OUT / 'atelier.blend').exists() or (OUT / 'pose.json').exists():
        raise ValueError('Une scene ou pose existe deja : ouvrir la scene sans la reconstruire.')
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / 'references/KHARVOX-Shotgun-Conversion-Native/Praetor-Shotgun-Conversion-Native.blend'))
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.scene.name = 'KHARVOX - '+work.TITLE+' - pose manuelle'
    bpy.context.preferences.filepaths.save_version = 0
    old_frames = {s: np.array(bpy.data.objects['MAIN_' + s].parent.matrix_world) for s in ('left', 'right')}
    hand_worlds = {s: np.array(bpy.data.objects['MAIN_' + s].matrix_world) for s in ('left', 'right')}
    for s in ('left', 'right'):
        obj = bpy.data.objects['MAIN_' + s]
        obj.parent = None
        obj.matrix_world = Matrix(hand_worlds[s])
    keep = {bpy.data.objects[n] for s in ('left', 'right') for n in ('MAIN_' + s, 'RIG_' + s, 'PRAETOR_' + s)}
    for obj in list(bpy.context.scene.objects):
        if obj not in keep:
            bpy.data.objects.remove(obj, do_unlink=True)
    review = module('workshop_weapon_mesh', ROOT / 'KHARVOX/tools/preview_praetor_weapon_grips.py')
    meshes, _ = review.weapon(cfg, bpy.context.scene.collection, {})
    source_model = model()
    bones = source_model.Skeleton().Bones()
    cm = c['native_cm_per_unit']
    root = bpy.data.objects.new('ARME_NATIVE', None)
    bpy.context.scene.collection.objects.link(root)
    armature = bpy.data.armatures.new('Squelette exporte '+work.TITLE)
    rig = bpy.data.objects.new('SQUELETTE_ARME_NATIVE', armature)
    bpy.context.scene.collection.objects.link(rig)
    rig.parent = root
    bpy.ops.object.select_all(action='DESELECT')
    rig.select_set(True)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode='EDIT')
    for i, bone in enumerate(bones):
        m = np.eye(4)
        m[:3, :3] = c['native_axes'][i]
        m[:3, 3] = np.array(c['native_positions'][i]) * cm
        edit = armature.edit_bones.new(bone.Name())
        edit.head, edit.tail = Vector(m[:3, 3]), Vector(m[:3, 3] + m[:3, 1]*2)
        edit.matrix = Matrix(m)
        if bone.ParentIndex() >= 0:
            edit.parent = armature.edit_bones[bones[bone.ParentIndex()].Name()]
    bpy.ops.object.mode_set(mode='OBJECT')
    rig.hide_select = True
    transform = np.eye(4)
    transform[:3, :3] = np.array(c['rotation']) * c['cast_to_native_scale'] * cm
    transform[:3, 3] = np.array(c['translation']) * cm
    for obj, index in zip(meshes, cfg['meshes']):
        obj.parent = rig
        obj.matrix_world = Matrix(transform)
        raw = source_model.Meshes()[index]
        width = raw.properties['mi'].values[0]
        ids = np.array(raw.VertexWeightBoneBuffer()).reshape(-1, width)
        weights = np.array(raw.VertexWeightValueBuffer()).reshape(-1, width)
        groups = [obj.vertex_groups.new(name=b.Name()) for b in bones]
        for vertex, (indices, values) in enumerate(zip(ids, weights)):
            for joint, weight in zip(indices, values):
                if weight > .000001:
                    groups[int(joint)].add([vertex], float(weight), 'REPLACE')
        obj.modifiers.new('Joints natifs', 'ARMATURE').object = rig
        obj.hide_select = True
    for side, item in cfg['attachments'].items():
        frame = bpy.data.objects.new('REPERE_' + side, None)
        bpy.context.scene.collection.objects.link(frame)
        constraint = frame.constraints.new('COPY_TRANSFORMS')
        constraint.target, constraint.subtarget = rig, item['joint']
        constraint.target_space = constraint.owner_space = 'WORLD'
        bpy.context.view_layer.update()
        obj = bpy.data.objects['MAIN_' + side]
        obj.parent = frame
        obj.matrix_parent_inverse = Matrix.Identity(4)
        obj.matrix_world = Matrix(np.array(frame.matrix_world) @ np.linalg.inv(old_frames[side]) @ hand_worlds[side])
    bpy.context.scene['coordinate_frame'] = c['coordinate_frame']
    bpy.context.scene['native_cm_per_unit'] = cm
    bpy.context.scene['calibration_sha256'] = c['capture_sha256']
    (OUT / 'conversion.json').write_text(json.dumps(c, indent=2))
    register()
    poseur.activate(bpy.context, 'left', tool='builtin.move')
    bpy.ops.file.pack_all()
    save(type('Reporter', (), {'report': lambda *args: None})(), bpy.context)


def export():
    if work.PROTECTED:
        raise ValueError('Arme validee : export protege.')
    c = calibration()
    pose = json.loads((OUT / 'pose.json').read_text())
    if pose['calibration_sha256'] != c['capture_sha256']:
        raise ValueError('Calibration modifiee depuis la pose : integration bloquee.')
    bpy.ops.wm.open_mainfile(filepath=str(OUT / 'atelier.blend'))
    before = {s: builder.bake(bpy.data.objects['PRAETOR_' + s], np.eye(4))[0] for s in ('left', 'right')}
    restore(pose)
    out = OUT / 'export'
    out.mkdir(exist_ok=True)
    exporter = module('workshop_glb', ROOT / 'KHARVOX/tools/export_reviewed_support_hands.py')
    support = (work.REFERENCE / 'support_hands.cfg').read_text().splitlines()
    primary = (work.REFERENCE / 'primary_hands.cfg').read_text().splitlines()
    row = next((line.split() for line in support if line.split()[1] == work.WEAPON), None)
    if row is None:
        # Carry the validated physical hand units / atlas identity. A new
        # weapon had no reviewed offhand-right profile in this baseline;
        # retain its existing left-handed fallback instead of borrowing one.
        row = next(line.split()[:49] for line in support if line.split()[1] == 'super_shotgun')
        row[0], row[1], row[3], row[5] = '4', work.WEAPON, '-', '0'
    if row[0] != '4' or len(row) != 49:
        raise ValueError('Profil de depart incompatible.')
    rows = {'left': row.copy(), 'right': row.copy()}
    bindings = {s: [(-1, np.eye(4)), (-1, np.eye(4))] for s in rows}
    report = dict(weapon=work.WEAPON, pose_sha256=work.sha(OUT / 'pose.json'), assets=[],
        scene_sha256=work.sha(OUT / 'atelier.blend'), calibration=c, game_validated=False,
        weapon_axes_native=work.SPEC.get('axes'), hands={})
    root_inverse = np.linalg.inv(np.array(bpy.data.objects['ARME_NATIVE'].matrix_world))
    rig = bpy.data.objects['SQUELETTE_ARME_NATIVE']
    for slot, side in enumerate(('left', 'right')):
        points, normals, mapping = builder.bake(bpy.data.objects['PRAETOR_' + side], root_inverse)
        actual = builder.bake(bpy.data.objects['PRAETOR_' + side], np.eye(4))[0]
        error = float(np.linalg.norm(actual - before[side], axis=1).max())
        if error > .002:
            raise ValueError('JSON et scene divergent : ' + side)
        joint = work.settings()['attachments'][side]['joint']
        bind = root_inverse @ np.array(rig.matrix_world) @ np.array(rig.pose.bones[joint].matrix)
        bind[:3, 3] /= c['native_cm_per_unit']
        if not np.allclose(bind[:3, :3].T @ bind[:3, :3], np.eye(3), atol=.0001):
            raise ValueError('Joint non rigide.')
        anchor = bind[:3, 3]
        fields = rows[side]
        meters = float(fields[12])
        transform = np.eye(4)
        transform[:3, :3] /= meters * 100
        transform[:3, 3] = (1 - c['native_cm_per_unit']/100/meters) * anchor
        vertices = points @ transform[:3, :3].T + transform[:3, 3]
        _, _, uv, indices = mapping
        name = 'assets/3D/' + ('SUPPORT_'+work.WEAPON.upper()+'_LEFT.glb' if slot == 0 else 'PRIMARY_'+work.WEAPON.upper()+'_RIGHT.glb')
        raw = exporter.geometry_glb(vars(builder), vertices, normals, uv, indices, side, dict(native_joint=joint))
        exporter.validate_glb(raw, vertices, indices.reshape(-1, 3))
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
        fields[0] = '5'
        fields[2 + slot] = name
        fields[4 + slot] = str(exporter.fnv64(raw))
        fields[6 + slot*3:9 + slot*3] = [format(v, '.12g') for v in anchor]
        bindings[side][slot] = (slot, np.linalg.inv(bind))
        header = (work.REFERENCE / ('assets/3D/DOOM_'+side.upper()+'_HAND_FINGERS.bin')).read_bytes()
        fist = np.array(struct.unpack_from('<16f', header, 28)).reshape(4, 4).T
        wrist = root_inverse @ np.array(bpy.data.objects['RIG_'+side].matrix_world) @ np.array(bpy.data.objects['RIG_'+side].pose.bones[side+'hand'].matrix)
        fist_to_asset = transform @ wrist @ np.linalg.inv(fist)
        if side == 'left':
            fields[17:33] = [format(v, '.12g') for v in fist_to_asset.T.flatten()]
        for index, inverse in bindings[side]:
            fields += [str(index)] + [format(v, '.12g') for v in inverse.T.flatten()]
        if len(fields) != 83:
            raise ValueError('Taille profil v5 invalide.')
        report['assets'].append(name)
        # Replay the same bind/live-joint and fixed-size matrix as the runtime.
        inverse = np.linalg.inv(bind)
        replay_error = 0.
        for angle in (0., .31):
            delta = np.eye(4)
            delta[:3, :3] = np.array(Quaternion((0, 0, 1), angle).to_matrix())
            delta[:3, 3] = (0., 0., 0.) if angle == 0 else (1., -.2, .4)
            live = delta @ bind
            attached = np.diag([c['native_cm_per_unit']/100]*3 + [1.]) @ live @ inverse
            fixed = attached.copy()
            fixed[:3, :3] = delta[:3, :3]*meters
            fixed[:3, 3] += (attached[:3, :3]-fixed[:3, :3])@anchor
            actual = vertices.astype(np.float32)@fixed[:3, :3].T + fixed[:3, 3]
            expected = (points@delta[:3, :3].T + delta[:3, 3]*c['native_cm_per_unit'])/100
            replay_error = max(replay_error, float(np.linalg.norm(actual-expected, axis=1).max()))
        if replay_error > .00002:
            raise ValueError('Raccord runtime incoherent : ' + side)
        report['hands'][side] = dict(joint=joint, asset=name, json_scene_error_cm=error,
            asset_sha256=work.sha(path), joint_replay_error_m=replay_error,
            fist_to_asset=fist_to_asset.tolist(), anchor_native=anchor.tolist())
    support = [line for line in support if line.split()[1] != work.WEAPON]
    support.append(' '.join(rows['left']))
    primary = [line for line in primary if line.split()[1] != work.WEAPON]
    primary.append(' '.join(rows['right']))
    for name, lines in (('support_hands.cfg', support), ('primary_hands.cfg', primary)):
        (out / name).write_text('\n'.join(lines) + '\n')
    (out / 'report.json').write_text(json.dumps(report, indent=2))


if __name__ == '__main__':
    action = sys.argv[sys.argv.index('--') + 1] if '--' in sys.argv else 'ui'
    if action == 'build':
        build()
    elif action == 'export':
        export()
    elif action == 'prepare':
        work.main('prepare')
    else:
        register()
