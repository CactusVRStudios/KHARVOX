"""Native Praetor finger posing UI. --build creates the isolated shotgun scene.

Open the supplied CMD to register the panel without installing an addon or
enabling automatic execution in Blender preferences. No runtime files touched.
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
from mathutils import Matrix, Quaternion, Vector

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'references/KHARVOX-Poseur-Shotgun'
NATIVE = ROOT / 'build/praetor-imported.blend'
REVIEW = ROOT / 'build/pump-review-v4/propositions-v4.blend'
AUDIT = ROOT / 'build/pump-review-v4/propositions-v4.json'
FINGERS = ('index', 'middle', 'ring', 'pinky', 'thumb')
LABELS = ('Index', 'Majeur', 'Annulaire', 'Auriculaire', 'Pouce')


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'KHARVOX/tools' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def pose_path():
    return OUT / 'shotgun_pose.json'


def payload():
    return dict(format='KHARVOX_MANUAL_PRAETOR_POSE', version=2, weapon='shotgun',
        units='centimetres', native_source_sha256=hashlib.sha256(NATIVE.read_bytes()).hexdigest(),
        weapon_cm_per_unit=json.loads(AUDIT.read_text())['weapon_cm_per_source_unit'],
        hands={side: dict(root_matrix_cm=[list(row) for row in bpy.data.objects['MAIN_' + side].matrix_world],
            rig_matrix_local=[list(row) for row in bpy.data.objects['RIG_' + side].matrix_local],
            mesh_matrix_local=[list(row) for row in bpy.data.objects['PRAETOR_' + side].matrix_local],
            bones={bone.name: [list(row) for row in bone.matrix_basis]
                   for bone in bpy.data.objects['RIG_' + side].pose.bones})
            for side in ('left', 'right')})


def restore(data):
    if (data.get('format') != 'KHARVOX_MANUAL_PRAETOR_POSE' or data.get('version') not in (1, 2)
            or data.get('weapon') != 'shotgun' or data.get('units') != 'centimetres'):
        raise ValueError('Ce fichier ne contient pas une pose shotgun compatible')
    if data['native_source_sha256'] != hashlib.sha256(NATIVE.read_bytes()).hexdigest():
        raise ValueError('La source Praetor a change ; pose non chargee')
    # Validate the entire payload before changing the scene.
    for side in ('left', 'right'):
        entry = data['hands'][side]
        rig = bpy.data.objects['RIG_' + side]
        if set(entry['bones']) != set(rig.pose.bones.keys()):
            raise ValueError('Squelette incompatible')
        for raw in (entry['root_matrix_cm'], entry['rig_matrix_local'], *entry['bones'].values()):
            values = np.asarray(raw)
            if values.shape != (4, 4) or not np.isfinite(values).all():
                raise ValueError('Matrice de pose invalide')
        if data['version'] == 2:
            values = np.asarray(entry['mesh_matrix_local'])
            if values.shape != (4, 4) or not np.isfinite(values).all():
                raise ValueError('Matrice du maillage invalide')
    for side in ('left', 'right'):
        entry = data['hands'][side]
        bpy.data.objects['MAIN_' + side].matrix_world = Matrix(entry['root_matrix_cm'])
        rig = bpy.data.objects['RIG_' + side]
        mesh = bpy.data.objects['PRAETOR_' + side]
        mesh_from_rig = rig.matrix_local.inverted() @ mesh.matrix_local
        rig.matrix_local = Matrix(entry['rig_matrix_local'])
        mesh.matrix_local = (Matrix(entry['mesh_matrix_local']) if data['version'] == 2
                             else rig.matrix_local @ mesh_from_rig)
        for name, raw in entry['bones'].items():
            rig.pose.bones[name].matrix_basis = Matrix(raw)
    bpy.context.view_layer.update()


def repair_hand_handles(force=False):
    """Move only the control origins; retain the complete posed hand in world space."""
    for side in ('left', 'right'):
        root = bpy.data.objects.get('MAIN_' + side)
        rig = bpy.data.objects.get('RIG_' + side)
        mesh = bpy.data.objects.get('PRAETOR_' + side)
        if not all((root, rig, mesh)):
            continue
        bpy.context.view_layer.update()
        if force or root.get('kharvox_wrist_handle_version') != 2:
            worlds = [(child, child.matrix_world.copy()) for child in root.children]
            wrist = rig.matrix_world @ rig.pose.bones[side + 'hand'].head
            matrix = root.matrix_world.copy()
            matrix.translation = wrist
            root.matrix_world = matrix
            bpy.context.view_layer.update()
            for child, world in worlds:
                child.matrix_world = world
            root['kharvox_wrist_handle_version'] = 2
        root.hide_select = False
        root.hide_set(False)
        root.lock_location = (False, False, False)
        root.lock_rotation = (False, False, False)
        root.show_in_front = True
        root.show_name = True
        root.empty_display_type = 'SPHERE'
        root.empty_display_size = 3
    bpy.context.view_layer.update()


def activate(context, side, bone='', tool='builtin.rotate'):
    if context.object and context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT')
    obj = bpy.data.objects[('RIG_' if bone else 'MAIN_') + side]
    obj.hide_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj
    if bone:
        bpy.ops.object.mode_set(mode='POSE')
        for item in obj.pose.bones:
            if hasattr(item, 'select'):
                item.select = False
            else:
                item.bone.select = False
        obj.data.bones.active = obj.data.bones[bone]
        selected = obj.pose.bones[bone]
        if hasattr(selected, 'select'):
            selected.select = True
        else:
            selected.bone.select = True
    context.scene.transform_orientation_slots[0].type = 'LOCAL' if bone else 'GLOBAL'
    for area in context.screen.areas:
        if area.type == 'VIEW_3D':
            space = area.spaces.active
            space.show_gizmo = True
            space.show_gizmo_tool = True
            space.show_gizmo_object_translate = not bone and tool == 'builtin.move'
            space.show_gizmo_object_rotate = tool == 'builtin.rotate'
            space.overlay.show_extras = True
            space.overlay.show_overlays = True
            with context.temp_override(area=area):
                bpy.ops.wm.tool_set_by_id(name=tool)


class KHARVOX_OT_choose(bpy.types.Operator):
    bl_idname = 'kharvox.choose_joint'
    bl_label = 'Selectionner le controle'
    bl_options = {'REGISTER', 'UNDO'}
    side: bpy.props.StringProperty(default='left')
    bone: bpy.props.StringProperty(default='')
    tool: bpy.props.StringProperty(default='builtin.rotate')

    def execute(self, context):
        activate(context, self.side, self.bone, self.tool)
        return {'FINISHED'}


class KHARVOX_OT_save(bpy.types.Operator):
    bl_idname = 'kharvox.save_manual_pose'
    bl_label = 'Enregistrer la pose'

    def execute(self, context):
        bpy.context.view_layer.update()
        pose_path().write_text(json.dumps(payload(), indent=2), encoding='utf-8')
        bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'Praetor-Shotgun.blend'))
        self.report({'INFO'}, 'Pose et scene enregistrees dans KHARVOX-Poseur-Shotgun')
        return {'FINISHED'}


class KHARVOX_OT_load(bpy.types.Operator):
    bl_idname = 'kharvox.load_manual_pose'
    bl_label = 'Recharger la pose'
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        try:
            restore(json.loads(pose_path().read_text(encoding='utf-8')))
            repair_hand_handles(force=True)
        except (OSError, ValueError, KeyError) as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        self.report({'INFO'}, 'Pose rechargee')
        return {'FINISHED'}


class KHARVOX_PT_pose(bpy.types.Panel):
    bl_label = 'Praetor / Fusil a pompe'
    bl_idname = 'KHARVOX_PT_manual_pose'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'KHARVOX'

    def draw(self, context):
        layout = self.layout
        layout.label(text='Main entiere : Deplacer, puis tirer une fleche.')
        layout.label(text='Cliquer une articulation, puis tirer un anneau.')
        layout.label(text='R = rotation souris ; Ctrl+Z = annuler.')
        for side, label in (('left', 'Main gauche'), ('right', 'Main droite')):
            box = layout.box()
            box.label(text=label)
            row = box.row(align=True)
            for text, tool in (('Deplacer', 'builtin.move'), ('Tourner', 'builtin.rotate')):
                op = row.operator('kharvox.choose_joint', text=text)
                op.side, op.tool = side, tool
            box.prop(bpy.data.objects['MAIN_' + side], 'location', text='Position')
            for finger, label in zip(FINGERS, LABELS):
                row = box.row(align=True)
                row.label(text=label)
                for segment in (1, 2, 3):
                    op = row.operator('kharvox.choose_joint', text=str(segment))
                    op.side, op.bone = side, side + 'hand' + finger + str(segment)
        layout.operator('kharvox.save_manual_pose', icon='FILE_TICK')
        layout.operator('kharvox.load_manual_pose', icon='FILE_REFRESH')
        layout.label(text='JSON conserve pour integration KHARVOX ulterieure.')


CLASSES = (KHARVOX_OT_choose, KHARVOX_OT_save, KHARVOX_OT_load, KHARVOX_PT_pose)


def register():
    for cls in CLASSES:
        old = getattr(bpy.types, cls.__name__, None)
        if old:
            bpy.utils.unregister_class(old)
        bpy.utils.register_class(cls)
    repair_hand_handles()
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                area.spaces.active.show_region_ui = True


def repair_saved_scene():
    """Migrate an existing local scene/JSON without rebuilding the user's pose."""
    bpy.context.preferences.filepaths.save_version = 0
    register()
    current = payload()
    if pose_path().exists():
        restore(json.loads(pose_path().read_text(encoding='utf-8')))
        repair_hand_handles(force=True)
        pose_path().write_text(json.dumps(payload(), indent=2), encoding='utf-8')
        restore(current)
    def world_vertices(side):
        obj = bpy.data.objects['PRAETOR_' + side].evaluated_get(bpy.context.evaluated_depsgraph_get())
        mesh = obj.to_mesh()
        try:
            points = np.array([v.co[:] for v in mesh.vertices])
            matrix = np.array(obj.matrix_world)
            return points @ matrix[:3, :3].T + matrix[:3, 3]
        finally:
            obj.to_mesh_clear()
    checks = {}
    for side, other in (('left', 'right'), ('right', 'left')):
        before = world_vertices(side)
        other_before = world_vertices(other)
        root = bpy.data.objects['MAIN_' + side]
        rig = bpy.data.objects['RIG_' + side]
        distance = (root.matrix_world.translation -
                    rig.matrix_world @ rig.pose.bones[side + 'hand'].head).length
        assert distance < .002, 'Control origin is not at the wrist'
        result = bpy.ops.kharvox.choose_joint(side=side, tool='builtin.move')
        assert result == {'FINISHED'} and bpy.context.object == root and root.mode == 'OBJECT'
        root.location += Vector((2, -1, .5))
        bpy.context.view_layer.update()
        assert np.allclose(world_vertices(side) - before, (2, -1, .5), atol=.001)
        assert np.allclose(world_vertices(other), other_before, atol=.001)
        restore(current)
        checks[side + '_whole_hand_moves_independently'] = True
        checks[side + '_handle_wrist_gap_cm'] = distance
    saved_json = json.loads(pose_path().read_text())
    restore(saved_json)
    positions = {side: world_vertices(side) for side in ('left', 'right')}
    restore(payload())
    assert all(np.allclose(positions[s], world_vertices(s), atol=.001) for s in positions)
    restore(current)
    checks['json_v2_restores_mesh_and_rig'] = True
    bpy.ops.kharvox.choose_joint(side='left', tool='builtin.move')
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'Praetor-Shotgun.blend'))
    validation_path = OUT / 'validation.json'
    validation = json.loads(validation_path.read_text())
    validation['checks'].update(checks)
    validation_path.write_text(json.dumps(validation, indent=2), encoding='utf-8')
    (OUT / 'LIRE-MOI.txt').write_text(
        'KHARVOX - POSEUR PRAETOR / SHOTGUN\n\n'
        'Ouvrir avec OUVRIR_POSEUR.cmd pour afficher N > KHARVOX.\n'
        'La scene demarre en mode Objet, main gauche selectionnee et outil Deplacer.\n'
        'Pour rapprocher une main : bouton Deplacer sous Main gauche/droite, puis\n'
        'tirer une fleche coloree a son poignet, ou G avec la souris dans la vue 3D.\n'
        'Les champs Position du panneau permettent aussi de deplacer chaque main.\n'
        'Les spheres MAIN_left et MAIN_right sont selectionnables aux poignets,\n'
        'meme si le fichier .blend est ouvert directement sans le panneau.\n'
        'Doigts : choisir une phalange 1/2/3, puis tourner les anneaux ou utiliser R.\n'
        'Ctrl+Z annule. Enregistrer la pose sauvegarde le .blend et shotgun_pose.json.\n'
        'Recharger restaure le JSON. Les JSON V1 restent compatibles, le V2 conserve\n'
        'aussi le repere du maillage pour fiabiliser le changement des poignees.\n'
        'Les sauvegardes avant correction portent le suffixe avant-deplacement.\n'
        'Aucune modification du jeu, HUD, menus ou executable. Integration jeu ulterieure.\n', encoding='utf-8')
    print('HAND MOVE REGRESSIONS', json.dumps(checks), flush=True)


def build():
    builder = load_module('manual_native_builder', 'build_praetor_hands.py')
    review = load_module('manual_native_weapon', 'preview_praetor_weapon_grips.py')
    audit = json.loads(AUDIT.read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.open_mainfile(filepath=str(NATIVE))
    scene = bpy.context.scene
    scene.name = 'KHARVOX - Pose manuelle shotgun'
    template = next(o for o in scene.objects if o.type == 'MESH' and o.get('kharvox_native_hands'))
    template_rig = next(m.object for m in template.modifiers if m.type == 'ARMATURE')
    original_objects = list(scene.objects)
    material = review.texture_material('Praetor / textures existantes',
        ROOT / 'build/praetor-hands/BaseColor.png', ROOT / 'build/praetor-hands/Normal.png',
        ROOT / 'build/praetor-hands/MetallicRoughness.png')
    # Reuse the corrected support transform, not the unsuccessful contact fit.
    left_matrix = np.array(audit['weapons']['shotgun']['hand_matrix_cm'])
    right_matrix = None
    with bpy.data.libraries.load(str(ROOT / 'build/pump-review-v4/shotgun-deux-mains.blend'), link=False) as (source, target):
        target.objects = [name for name in source.objects if name.startswith('Vraie main DROITE')]
    donor_right = target.objects[0]
    scene.collection.objects.link(donor_right)
    bpy.context.view_layer.update()
    right_matrix = np.array(donor_right.matrix_world)
    bpy.data.objects.remove(donor_right, do_unlink=True)
    validation = dict(weapon='shotgun', no_runtime_changes=True, hands={}, checks={})
    for side, placement in (('left', left_matrix), ('right', right_matrix)):
        rig = template_rig.copy()
        rig.data = template_rig.data.copy()
        rig.name = 'RIG_' + side
        scene.collection.objects.link(rig)
        obj = template.copy()
        obj.data = template.data.copy()
        obj.name = 'PRAETOR_' + side
        scene.collection.objects.link(obj)
        obj.parent = None
        obj.matrix_world = template.matrix_world.copy()
        rig.parent = None
        rig.matrix_world = template_rig.matrix_world.copy()
        for modifier in obj.modifiers:
            if modifier.type == 'ARMATURE':
                modifier.object = rig
        bpy.context.view_layer.update()
        wrist, basis, _ = builder.landmarks(rig, side)
        builder.isolate(obj, rig, side)
        builder.pose(rig, side, {f: 'closed' for f in FINGERS})
        if side == 'left':
            recipe = audit['weapons']['shotgun']['finger_closure']
            for finger in FINGERS:
                first = rig.data.bones[side + 'hand' + finger + '1'].matrix_local
                second = rig.data.bones[side + 'hand' + finger + '2'].matrix_local
                sign = 1 if first.col[1].xyz.cross(second.col[1].xyz).dot(first.col[0].xyz) >= 0 else -1
                for segment, angle in enumerate(recipe[3:6] if finger == 'thumb' else recipe[:3], 1):
                    rig.pose.bones[side + 'hand' + finger + str(segment)].rotation_quaternion = Quaternion((1, 0, 0), sign * math.radians(angle))
            bpy.context.view_layer.update()
            thumb = rig.pose.bones['lefthandthumb1']
            axis = (rig.matrix_world @ thumb.matrix).to_3x3().inverted() @ Vector(basis[:, 2].tolist())
            thumb.rotation_quaternion @= Quaternion(axis.normalized(), math.radians(recipe[6]))
        anatomy = np.eye(4)
        anatomy[:3, :3], anatomy[:3, 3] = basis, wrist
        root = bpy.data.objects.new('MAIN_' + side, None)
        scene.collection.objects.link(root)
        root.empty_display_type = 'CUBE'
        root.empty_display_size = 5
        root.matrix_world = Matrix((placement @ np.linalg.inv(anatomy)).tolist())
        root.lock_scale = (True, True, True)
        for child in (rig, obj):
            original = child.matrix_world.copy()
            child.parent = root
            child.matrix_parent_inverse = Matrix.Identity(4)
            child.matrix_basis = original
        obj.data.materials.clear()
        obj.data.materials.append(material)
        for polygon in obj.data.polygons:
            polygon.material_index = 0
            polygon.use_smooth = True
        obj.hide_select = True
        rig.show_in_front = True
        rig.data.display_type = 'WIRE'
        for bone in rig.data.bones:
            visible = any(bone.name == side + 'hand' + finger + str(segment)
                          for finger in FINGERS for segment in (1, 2, 3))
            bone.hide = not visible
            if visible:
                bone.color.palette = 'THEME04' if side == 'left' else 'THEME03'
                control = rig.pose.bones[bone.name]
                control.lock_location = (True, True, True)
                control.lock_scale = (True, True, True)
        # Native display tails are tiny; explicit wire circles make selection usable.
        points = [(0, math.cos(i * math.tau / 32) * .8, math.sin(i * math.tau / 32) * .8) for i in range(32)]
        mesh = bpy.data.meshes.new('Cercle articulation ' + side)
        mesh.from_pydata(points, [(i, (i + 1) % 32) for i in range(32)], [])
        shape = bpy.data.objects.new('FORME_' + side, mesh)
        # Custom shapes need not be linked into the scene.
        for bone in rig.pose.bones:
            if not bone.bone.hide:
                bone.custom_shape = shape
                bone.use_custom_shape_bone_size = False
        validation['hands'][side] = dict(native_finger_joints=sum(not b.hide for b in rig.data.bones),
            vertices=len(obj.data.vertices), armature_modifier=True)
    for obj in original_objects:
        bpy.data.objects.remove(obj, do_unlink=True)
    weapon_audit = {}
    weapon_objects, weapon_points = review.weapon(review.SPECS['shotgun'], scene.collection, weapon_audit)
    for obj in weapon_objects:
        obj.scale = (audit['weapon_cm_per_source_unit'],) * 3
        obj.hide_select = True
    bpy.context.view_layer.update()
    register()
    # Independently check the articulated left hand against the saved correction.
    with bpy.data.libraries.load(str(REVIEW), link=False) as (source, target):
        target.scenes = ['FUSIL A POMPE']
    saved_scene = target.scenes[0]
    reference = next(o for o in saved_scene.objects if o.type == 'MESH' and o.name.startswith('Soutien'))
    reference_raw = np.array([v.co[:] for v in reference.data.vertices])
    expected = reference_raw @ left_matrix[:3, :3].T + left_matrix[:3, 3]
    actual, _, _ = builder.bake(bpy.data.objects['PRAETOR_left'], np.eye(4))
    assert actual.shape == expected.shape
    error = float(np.linalg.norm(actual - expected, axis=1).max())
    assert error < .002, 'Saved left correction mismatch: ' + str(error)
    validation['checks']['left_saved_correction_max_error_cm'] = error
    bpy.data.scenes.remove(saved_scene)
    initial = payload()
    # Test native skinning and lossless pose reload without any collision code.
    for side in ('left', 'right'):
        obj, rig = bpy.data.objects['PRAETOR_' + side], bpy.data.objects['RIG_' + side]
        original, _, _ = builder.bake(obj, np.eye(4))
        joint = rig.pose.bones[side + 'handindex2']
        joint.rotation_quaternion @= Quaternion((1, 0, 0), .3)
        bpy.context.view_layer.update()
        moved, _, _ = builder.bake(obj, np.eye(4))
        assert np.max(np.linalg.norm(moved - original, axis=1)) > .05
        restore(initial)
        restored, _, _ = builder.bake(obj, np.eye(4))
        assert np.allclose(original, restored, atol=2e-4)
        validation['checks'][side + '_finger_moves_skin_and_restores'] = True
        assert validation['hands'][side]['native_finger_joints'] == 15
    pose_path().write_text(json.dumps(initial, indent=2), encoding='utf-8')
    restore(json.loads(pose_path().read_text()))
    validation['checks']['pose_json_round_trip'] = True
    validation['checks']['native_joint_controls'] = 30
    # Test panel operators, selecting a native joint with the mouse's rotation tool.
    result = bpy.ops.kharvox.choose_joint(side='left', bone='lefthandindex2')
    assert result == {'FINISHED'} and bpy.context.object.mode == 'POSE'
    validation['checks']['selection_operator'] = True
    bpy.ops.kharvox.choose_joint(side='left', tool='builtin.move')
    scene.unit_settings.system, scene.unit_settings.scale_length = 'METRIC', .01
    scene.tool_settings.transform_pivot_point = 'INDIVIDUAL_ORIGINS'
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                space = area.spaces.active
                space.shading.type = 'MATERIAL'
                space.show_gizmo = True
                space.region_3d.view_location = (20, 0, -7)
                space.region_3d.view_distance = 140
                space.region_3d.view_rotation = Vector((-.7, 1.5, .6)).to_track_quat('Z', 'Y')
                space.region_3d.view_perspective = 'PERSP'
                space.clip_end = 5000
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT / 'Praetor-Shotgun.blend'))
    validation['status'] = 'MANUAL_POSE_TOOL_READY_NO_GAME_INTEGRATION'
    (OUT / 'validation.json').write_text(json.dumps(validation, indent=2), encoding='utf-8')
    blender = ROOT / '.tools/blender-5.2.3/blender-5.2.3-windows-x64/blender.exe'
    command = '@echo off\r\nstart "KHARVOX - Poseur Praetor" "' + str(blender) + '" "' + str(OUT / 'Praetor-Shotgun.blend') + '" --python "' + str(pathlib.Path(__file__).resolve()) + '"\r\n'
    (OUT / 'OUVRIR_POSEUR.cmd').write_bytes(command.encode('utf-8'))
    (OUT / 'LIRE-MOI.txt').write_text(
        'KHARVOX / POSEUR MANUEL PRAETOR - FUSIL A POMPE\n\n'
        '1. Double-cliquer OUVRIR_POSEUR.cmd. Le panneau est dans N > KHARVOX.\n'
        '2. Deplacer/Tourner choisit la main entiere. Tirer les fleches/anneaux a la souris.\n'
        '3. Pour les doigts, cliquer une poignee coloree ou le bouton 1/2/3 de la phalange.\n'
        '   Tirer le cercle rouge pour plier la phalange sur son axe local X.\n'
        '   R permet aussi la rotation a la souris ; Ctrl+Z annule.\n'
        '4. Enregistrer la pose sauvegarde la scene .blend ET shotgun_pose.json.\n'
        '5. Recharger la pose relit le JSON. Copier ce JSON pour conserver plusieurs versions.\n\n'
        'Les squelettes, poids de skinning et textures natifs sont reutilises.\n'
        'La main gauche reprend exactement la pose et la transformation corrigees V4.\n'
        'La main droite reprend la scene existante : son placement reste a regler manuellement.\n'
        'Aucune collision, optimisation automatique ou installation addon.\n'
        'Le JSON conserve les transformations et articulations pour un futur adaptateur KHARVOX.\n'
        'Il ne peut pas encore etre charge directement par le jeu. HUD/menu/executable inchanges.\n'
        'Ouvrir via le CMD pour disposer du panneau, ou executer manual_praetor_poseur.py dans Blender.\n', encoding='utf-8')
    print(json.dumps(validation), flush=True)


if __name__ == '__main__':
    if '--build' in sys.argv:
        build()
    elif '--repair' in sys.argv:
        repair_saved_scene()
    else:
        register()
