"""Isolated forearm HUD authoring. No runtime export or hand-pose writes."""
import sys
sys.dont_write_bytecode = True
import hashlib
import importlib.util
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/hud-left-workshop'
SCENE = OUT / 'HUD-Praetor-Gauche.blend'
SOURCE = ROOT / 'build/praetor-imported.blend'
RUNTIME = ROOT / 'artifacts/KHARVOX-VR-Valide'
ANCHOR = 'HUD_anchor_left'
CONTROL = 'HUD_gauche'
BONE = 'LeftForeArmRoll2'
PANELS = ('HUD_Vie_Armure', 'HUD_Munitions', 'HUD_Options')


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'KHARVOX/tools' / filename)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(matrix):
    return [list(row) for row in matrix]


def select(context, name, tool='builtin.move'):
    if context.object and context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    bpy.ops.object.select_all(action='DESELECT')
    obj = bpy.data.objects[name]
    obj.select_set(True)
    context.view_layer.objects.active = obj
    if context.area and context.area.type == 'VIEW_3D':
        bpy.ops.wm.tool_set_by_id(name=tool)
    return obj


def check_rigid(matrix):
    basis = np.array(matrix.to_3x3())
    if not np.isfinite(np.array(matrix)).all() or not np.allclose(basis.T @ basis, np.eye(3), atol=2e-4) or np.linalg.det(basis) < .999:
        raise ValueError('Le HUD doit garder son échelle 1 ; utiliser G/R pour le déplacer et le tourner.')


def payload():
    bpy.context.view_layer.update()
    anchor = bpy.data.objects[ANCHOR]
    rig = bpy.data.objects['SQUELETTE_PRAETOR_GAUCHE']
    if anchor.parent != rig or anchor.parent_type != 'BONE' or anchor.parent_bone != BONE:
        raise ValueError('Ancrage avant-bras modifié ; enregistrement refusé.')
    inverse = anchor.matrix_world.inverted()
    result = {}
    for name in (CONTROL, *PANELS):
        obj = bpy.data.objects[name]
        local = inverse @ obj.matrix_world
        check_rigid(local)
        quat = local.to_quaternion()
        result[name] = dict(matrix_anchor_local_cm=rows(local),
            position_cm=list(local.translation), quaternion_xyzw=[quat.x, quat.y, quat.z, quat.w])
        if name in PANELS:
            result[name].update(width_cm=obj['width_cm'], height_cm=obj['height_cm'],
                dimensions_provisional=bool(obj.get('dimensions_provisional', False)))
    return dict(format='KHARVOX_FOREARM_HUD_AUTHORING', version=1,
        stage='blender_only_awaiting_human_validation', runtime_ready=False,
        units='centimetres', matrix_layout='row_major_column_vectors', physical_hand='left',
        anchor=dict(name=ANCHOR, bone=BONE, rig=rig.name,
            bone_to_anchor_bind=json.loads(anchor['bone_to_anchor_bind']),
            bone_matrix_native_at_bind=rows(rig.pose.bones[BONE].matrix),
            frame_in_grip_xr_cm=rows(bpy.data.objects['AVANT_BRAS_GAUCHE'].matrix_world.inverted() @ anchor.matrix_world),
            native_to_grip_xr_cm=json.loads(rig['native_to_grip_xr_cm'])),
        transforms=result, input_hashes=json.loads(bpy.context.scene['input_hashes']),
        initial_runtime_orientation_verified=False,
        note='Positions configurées ; axes GUI natifs et dégagement dynamique non enregistrés par le jeu. Placement manuel à valider. Aucun offset contrôleur à réappliquer.')


class KHARVOX_OT_hud_left_select(bpy.types.Operator):
    bl_idname = 'kharvox.hud_left_select'
    bl_label = 'Sélectionner le HUD'
    target: bpy.props.StringProperty(default=CONTROL)
    rotate: bpy.props.BoolProperty(default=False)

    def execute(self, context):
        select(context, self.target, 'builtin.rotate' if self.rotate else 'builtin.move')
        return {'FINISHED'}


class KHARVOX_OT_hud_left_save(bpy.types.Operator):
    bl_idname = 'kharvox.hud_left_save'
    bl_label = 'Enregistrer la position HUD'

    def execute(self, context):
        try:
            if context.object and context.object.mode != 'OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
            data = payload()
            (OUT / 'hud-left.json').write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding='utf-8')
            bpy.ops.wm.save_as_mainfile(filepath=str(SCENE))
        except Exception as error:
            self.report({'ERROR'}, str(error))
            return {'CANCELLED'}
        self.report({'INFO'}, 'Position enregistrée : ' + str(OUT / 'hud-left.json'))
        return {'FINISHED'}


class KHARVOX_PT_hud_left(bpy.types.Panel):
    bl_label = 'HUD gauche'
    bl_idname = 'KHARVOX_PT_hud_left'
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = 'KHARVOX'

    def draw(self, context):
        layout = self.layout
        layout.label(text='Ancrage : ' + BONE, icon='BONE_DATA')
        layout.label(text='Croix orange : HUD_anchor_left')
        row = layout.row(align=True)
        row.operator('kharvox.hud_left_select', text='Déplacer le HUD').target = CONTROL
        op = row.operator('kharvox.hud_left_select', text='Tourner le HUD')
        op.target, op.rotate = CONTROL, True
        row = layout.row(align=True)
        for name, title in zip(PANELS, ('Vie / armure', 'Munitions', 'Options')):
            row.operator('kharvox.hud_left_select', text=title).target = name
        layout.operator('kharvox.hud_left_select', text='Voir le point d’ancrage').target = ANCHOR
        layout.operator('kharvox.hud_left_select', text='Tourner le bras (aperçu)').target = 'AVANT_BRAS_GAUCHE'
        active = context.object
        if active and active.name in (CONTROL, *PANELS):
            layout.prop(active, 'location', text='Position locale')
            layout.prop(active, 'rotation_euler', text='Rotation locale')
        layout.label(text='G : déplacer   R : tourner   Ctrl+Z : annuler')
        layout.label(text='Vie / munitions : 25 × 14,65 cm')
        layout.label(text='Options : largeur 20 cm ; hauteur provisoire')
        warning = layout.box()
        warning.label(text='Préposition initiale indicative', icon='INFO')
        warning.label(text='Axes GUI / dégagement du jeu non sauvegardés')
        warning.label(text='Pas d’intégration jeu avant ta validation')
        layout.operator('kharvox.hud_left_save', icon='FILE_TICK')
        layout.label(text='Export : build/hud-left-workshop/hud-left.json')


def register():
    for cls in (KHARVOX_OT_hud_left_select, KHARVOX_OT_hud_left_save, KHARVOX_PT_hud_left):
        previous = getattr(bpy.types, cls.__name__, None)
        if previous:
            bpy.utils.unregister_class(previous)
        bpy.utils.register_class(cls)
    if not bpy.app.background:
        def activate():
            select(bpy.context, CONTROL)
            for window in bpy.context.window_manager.windows:
                for area in window.screen.areas:
                    if area.type != 'VIEW_3D': continue
                    area.spaces.active.show_region_ui = True
                    for sidebar in area.regions:
                        if sidebar.type == 'UI': sidebar.active_panel_category = 'KHARVOX'
                    region = next((r for r in area.regions if r.type == 'WINDOW'), None)
                    if region:
                        with bpy.context.temp_override(window=window, area=area, region=region):
                            bpy.ops.wm.tool_set_by_id(name='builtin.move')
            return None
        bpy.app.timers.register(activate, first_interval=.5)


def empty(name, matrix, parent=None, kind='PLAIN_AXES', size=3):
    obj = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(obj)
    obj.empty_display_type, obj.empty_display_size = kind, size
    obj.parent = parent
    obj.matrix_world = matrix
    obj.show_name, obj.show_in_front = True, True
    return obj


def build():
    if SCENE.exists():
        raise ValueError('Scène existante : ouvrir ATELIER_HUD_GAUCHE.cmd ; aucune reconstruction automatique.')
    builder = module('hud_native_builder', 'build_praetor_hands.py')
    reader = module('hud_runtime_geometry', 'preview_praetor_hud_clearance.py')
    review = module('hud_existing_materials', 'preview_praetor_weapon_grips.py')
    inputs = [SOURCE, ROOT / 'build/praetor-hands/provenance.json',
        RUNTIME / 'offhand_hud.cfg', RUNTIME / 'hand_models.cfg',
        RUNTIME / 'hand_models_calibration_default.cfg',
        RUNTIME / 'assets/3D/DOOM_LEFT_HAND_FIST.glb',
        RUNTIME / 'assets/3D/DOOM_LEFT_HAND_FINGERS.bin']
    saved = RUNTIME / 'hand_models_calibration_saved.cfg'
    if saved.exists(): inputs.append(saved)
    hashes = {str(path): sha(path) for path in inputs}
    runtime_points, _, _, runtime_audit = reader.load_runtime_hand(RUNTIME, 'left', True)
    provenance = json.loads(inputs[1].read_text())
    align = Matrix(next(h for h in provenance['hands'] if h['physical_hand'] == 'left')['alignment']['FIST'])
    native_to_cm = Matrix(np.array(runtime_audit['raw_grip_to_baked_mesh_matrix_meters']).tolist()) @ align
    native_to_cm = Matrix.Diagonal((100., 100., 100., 1.)) @ native_to_cm
    bpy.ops.wm.open_mainfile(filepath=str(SOURCE))
    bpy.context.preferences.filepaths.save_version = 0
    scene = bpy.context.scene
    scene.name = 'KHARVOX - HUD gauche - préparation uniquement'
    scene.unit_settings.system, scene.unit_settings.scale_length = 'METRIC', .01
    scene.unit_settings.length_unit = 'CENTIMETERS'
    mesh = next(o for o in scene.objects if o.type == 'MESH' and o.get('kharvox_native_hands'))
    rig = next(m.object for m in mesh.modifiers if m.type == 'ARMATURE')
    for obj in list(scene.objects):
        if obj not in (mesh, rig): bpy.data.objects.remove(obj, do_unlink=True)
    if ANCHOR in scene.objects:
        raise ValueError('Un ancrage HUD existe déjà : ne pas en créer un second.')
    if BONE not in rig.pose.bones: raise ValueError('Os avant-bras natif absent : ' + BONE)
    builder.isolate(mesh, rig, 'left')
    builder.pose(rig, 'left', {finger: 'free' for finger in builder.FINGERS})
    mesh.parent = None
    rig.parent = None
    mesh.matrix_world = rig.matrix_world = native_to_cm
    mesh.name, rig.name = 'PRAETOR_GAUCHE', 'SQUELETTE_PRAETOR_GAUCHE'
    mesh.hide_select = True
    rig.hide_select = True
    rig.show_in_front = True
    rig.data.display_type = 'WIRE'
    for bone in rig.data.bones:
        bone.hide = not ('left' in bone.name.lower() and ('hand' in bone.name.lower() or 'arm' in bone.name.lower()))
        bone.color.palette = 'THEME03' if bone.name == BONE else 'THEME04'
        pb = rig.pose.bones[bone.name]
        pb.lock_location = pb.lock_rotation = pb.lock_scale = (True, True, True)
        pb.lock_rotation_w = True
    visible_joints(rig)
    textures = ROOT / 'build/praetor-hands'
    material = review.texture_material('Praetor existant / aperçu HUD', textures/'BaseColor.png', textures/'Normal.png', textures/'MetallicRoughness.png')
    mesh.data.materials.clear()
    mesh.data.materials.append(material)
    for polygon in mesh.data.polygons: polygon.material_index = 0
    preview = empty('AVANT_BRAS_GAUCHE', Matrix.Identity(4), size=6)
    preview.lock_scale = (True, True, True)
    for obj in (rig, mesh):
        world = obj.matrix_world.copy()
        obj.parent, obj.matrix_world = preview, world
    bpy.context.view_layer.update()
    bone_world = rig.matrix_world @ rig.pose.bones[BONE].matrix
    normalized = bone_world.to_3x3().normalized().to_4x4()
    normalized.translation = bone_world.translation
    anchor = empty(ANCHOR, normalized, kind='ARROWS', size=4)
    anchor.parent, anchor.parent_type, anchor.parent_bone = rig, 'BONE', BONE
    bpy.context.view_layer.update()
    anchor.matrix_world = normalized
    anchor.color = (1, .35, .02, 1)
    anchor.lock_location = anchor.lock_rotation = anchor.lock_scale = (True, True, True)
    bpy.context.view_layer.update()
    if not np.allclose(np.array(anchor.matrix_world), np.array(normalized), atol=1e-4):
        raise ValueError('Repère parent os instable ; scène non enregistrée.')
    anchor['bone_to_anchor_bind'] = json.dumps(rows(bone_world.inverted() @ anchor.matrix_world))
    rig['native_to_grip_xr_cm'] = json.dumps(rows(native_to_cm))
    cfg = (RUNTIME / 'offhand_hud.cfg').read_text(encoding='utf-8-sig').splitlines()
    calibrations = [list(map(float, row.split())) for row in cfg[1:7]]
    # Native GUI's in-plane rows are not serialized. This explicit provisional
    # frame is the same technical-envelope convention used by the HUD review.
    control = empty(CONTROL, anchor.matrix_world.copy(), anchor, kind='CUBE', size=2)
    control.lock_scale = (True, True, True)
    for index, name in enumerate(PANELS):
        values = calibrations[index*2]
        point = np.array(values[:3]);point[1] += (14, -14, 0)[index]
        center = reader.DOOM_TO_XR @ point
        basis = reader.hud_basis(values[3:6])
        matrix = Matrix.Identity(4)
        # Blender rectangle X/Y correspond to the provisional GUI pixel rows.
        for column, source_row in enumerate((1, 2, 0)):
            for row in range(3): matrix[row][column] = float(basis[source_row, row])
        matrix.translation = Vector(center)
        if index == 0:
            control.matrix_world = matrix
            bpy.context.view_layer.update()
        width, height = (20 if index==2 else 25)*values[6]/.4, (20 if index==2 else 25)*values[6]/.4*300/512
        data = bpy.data.meshes.new(name)
        data.from_pydata([(-width/2,-height/2,0),(width/2,-height/2,0),(width/2,height/2,0),(-width/2,height/2,0)], [], [(0,1,2,3)])
        obj = bpy.data.objects.new(name, data)
        scene.collection.objects.link(obj)
        obj.parent, obj.matrix_world = control, matrix
        obj.lock_scale = (True, True, True)
        obj.rotation_mode = 'XYZ'
        obj.show_wire, obj.show_all_edges, obj.show_name = True, True, True
        obj.color = ((.05,.65,1,.4),(.95,.45,.04,.4),(.6,.25,.9,.4))[index]
        mat = bpy.data.materials.new(name+' / gabarit sans graphisme')
        mat.diffuse_color = obj.color
        obj.data.materials.append(mat)
        obj['width_cm'], obj['height_cm'] = width, height
        obj['dimensions_provisional'] = index == 2
    scene['input_hashes'] = json.dumps(hashes)
    scene['hud_initial_orientation'] = 'PROVISOIRE : axes GUI natifs non sauvegardés ; pas de dégagement dynamique reproduit'
    register()
    bpy.context.view_layer.update()
    # No native meshes/poses are exported. Test only the new anchor-local data.
    initial = payload()
    original_local = np.array(initial['transforms'][CONTROL]['matrix_anchor_local_cm'])
    before = preview.matrix_world.copy()
    preview.rotation_euler = (.4, -.6, .8)
    bpy.context.view_layer.update()
    moved = payload()
    if not np.allclose(original_local, np.array(moved['transforms'][CONTROL]['matrix_anchor_local_cm']), atol=2e-4):
        raise ValueError('Le HUD ne suit pas rigidement le bras ; scène non enregistrée.')
    preview.matrix_world = before
    bpy.context.view_layer.update()
    for path in inputs:
        if sha(path) != hashes[str(path)]: raise ValueError('Source modifiée : '+str(path))
    # Compare the forearm extent against the actual relaxed runtime hand.
    evaluated = mesh.evaluated_get(bpy.context.evaluated_depsgraph_get())
    geometry = evaluated.to_mesh()
    points = np.array([mesh.matrix_world @ v.co for v in geometry.vertices])
    evaluated.to_mesh_clear()
    native_bounds = np.array((points.min(0), points.max(0)))
    runtime_bounds = np.array((runtime_points.min(0), runtime_points.max(0)))
    report = dict(anchor=ANCHOR, bone=BONE, anchor_count=sum(o.name==ANCHOR for o in scene.objects),
        source_files_unchanged=True, runtime_not_modified=True, arm_rotation_follows=True,
        scene_units='one Blender unit = one centimetre', panel_dimensions_cm=[[bpy.data.objects[n]['width_cm'],bpy.data.objects[n]['height_cm']] for n in PANELS],
        initial_orientation_provisional=True, runtime_dynamic_clearance_reproduced=False,
        rig_bounds_cm=native_bounds.tolist(), runtime_relaxed_bounds_cm=runtime_bounds.tolist(),
        bounds_max_difference_cm=float(np.max(np.abs(native_bounds-runtime_bounds))),
        exported_user_pose=False, runtime_export_enabled=False)
    center = Vector(((points.min(0)+points.max(0))*.5).tolist())
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                space = area.spaces.active
                space.show_region_ui = True
                space.shading.type, space.shading.color_type = 'SOLID', 'TEXTURE'
                space.overlay.show_floor = False
                space.region_3d.view_location = center
                space.region_3d.view_distance = 65
                space.region_3d.view_rotation = Vector((1,-1,1)).to_track_quat('Z','Y')
                space.region_3d.view_perspective = 'PERSP'
    select(bpy.context, CONTROL)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (OUT / 'LIRE-MOI.txt').write_text(
        'HUD GAUCHE - PREPARATION BLENDER UNIQUEMENT\n'
        'Ouvrir ATELIER_HUD_GAUCHE.cmd : N > KHARVOX > HUD gauche.\n'
        'Croix orange HUD_anchor_left : rattachee a LeftForeArmRoll2, os dominant de la protection.\n'
        'Deplacer/Tourner le HUD selectionne tout le groupe ; G/R et fleches/anneaux Blender.\n'
        'Vie/armure, Munitions, Options : ajustements individuels possibles.\n'
        'Alt+Z rend la geometrie transparente pour inspecter une intersection ; le desactiver pour juger le masquage.\n'
        'Tourner le bras (apercu) : R, le HUD suit le bras sans changer sa pose locale.\n'
        'Doigts et squelette verrouilles ; aucun fichier de pose existant modifie.\n'
        'Preposition = centres configures du jeu. Axes GUI natifs et degagement dynamique\n'
        'non sauvegardes : orientation initiale INDICATIVE, pas une reproduction certifiee du rendu.\n'
        'Vie et munitions : 25 x 14.6484375 cm. Options : largeur 20 cm ; hauteur 11.71875 cm PROVISOIRE.\n'
        'Ces rectangles sont des gabarits techniques, sans recreer les dessins du HUD.\n'
        'Enregistrer la position HUD : sauvegarde HUD-Praetor-Gauche.blend + hud-left.json ici.\n'
        'Export local en cm par rapport a HUD_anchor_left, matrices 4x4 et quaternions.\n'
        'Aucun offset controleur a ajouter. Ce JSON est preparatoire et nest pas charge par le jeu.\n'
        'Integration uniquement apres validation humaine.\n', encoding='utf-8')
    bpy.ops.wm.save_as_mainfile(filepath=str(SCENE))
    print('HUD_LEFT_READY '+json.dumps(report), flush=True)


def visible_joints(rig):
    # Reuse the native poseur's wire-circle display: Cast bone tails are only
    # .0025 native units long. Custom shapes change neither rest bones nor pose.
    data = bpy.data.meshes.new('HUD_repere_articulation')
    points = [(0, math.cos(i*math.tau/24)*.55, math.sin(i*math.tau/24)*.55) for i in range(24)]
    data.from_pydata(points, [(i,(i+1)%24) for i in range(24)], [])
    shape = bpy.data.objects.new('HUD_forme_articulation', data)
    for bone in rig.pose.bones:
        if not bone.bone.hide:
            bone.custom_shape = shape
            bone.use_custom_shape_bone_size = False


if __name__ == '__main__':
    if '--build' in sys.argv: build()
    else: register()
