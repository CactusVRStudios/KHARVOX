"""Grenade authoring only: existing Praetor rig, Cast mesh, manual pose controls.

No game conversion, runtime writes, launcher invocation or projectile hooks.
Run through ATELIER_GRENADE.cmd; saved scenes never execute scripts implicitly.
"""
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
from mathutils.bvhtree import BVHTree

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/grenade-workshop/frag'
SCENE = OUT / 'atelier.blend'
POSE = OUT / 'pose.json'
HAND_SOURCE = ROOT / 'build/hud-left-workshop/HUD-Praetor-Gauche.blend'
CAST = next((ROOT / 'build/native-weapons-export').rglob('grenade_frag.cast'))
ANCHOR, CONTROL = 'GRENADE_anchor_left', 'GRENADE_tenue'


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'KHARVOX/tools' / filename)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


poseur = module('grenade_existing_poseur', 'manual_praetor_poseur.py')
builder = module('grenade_existing_hand_builder', 'build_praetor_hands.py')
review = module('grenade_existing_mesh_tools', 'preview_praetor_weapon_grips.py')


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def rows(matrix):
    return [list(row) for row in matrix]


def verify_sources():
    expected = json.loads((OUT / 'inputs-sha256.json').read_text(encoding='utf-8'))
    for name, digest in expected.items():
        if sha(name) != digest:
            raise ValueError('Source modifiée : ' + name)


def payload():
    bpy.context.view_layer.update()
    rig, hand = bpy.data.objects['RIG_left'], bpy.data.objects['PRAETOR_left']
    anchor, grenade = bpy.data.objects[ANCHOR], bpy.data.objects[CONTROL]
    if anchor.parent != rig or anchor.parent_bone != 'lefthand' or anchor.parent_type != 'BONE':
        raise ValueError('Ancrage grenade modifié ; conserver lefthand.')
    local = anchor.matrix_world.inverted() @ grenade.matrix_world
    if not np.allclose(np.array(local)[:3, :3].T @ np.array(local)[:3, :3], np.eye(3), atol=2e-4) or local.determinant() <= 0:
        raise ValueError('Déplacer/tourner la grenade avec G/R, sans modifier son échelle.')
    return dict(format='KHARVOX_GRENADE_AUTHORING', version=1,
        stage='BLENDER_ONLY_AWAITING_EXPLICIT_USER_VALIDATION', game_exported=False,
        equipment='frag_grenade', physical_hand='left', units='centimetres',
        matrix_layout='row_major_column_vectors', input_hashes=json.loads((OUT / 'inputs-sha256.json').read_text()),
        native_reference_sha256=sha(OUT / 'reference-native.json'),
        hand=dict(root_matrix_cm=rows(bpy.data.objects['MAIN_left'].matrix_world),
            rig_matrix_local=rows(rig.matrix_local), mesh_matrix_local=rows(hand.matrix_local),
            bones={b.name: rows(b.matrix_basis) for b in rig.pose.bones}),
        anchor=dict(name=ANCHOR, bone='lefthand', bone_to_anchor_bind=json.loads(anchor['bone_to_anchor_bind'])),
        grenade=dict(matrix_anchor_local_cm=rows(local), mesh_local_matrices={o.name: rows(o.matrix_local) for o in grenade.children}),
        note='Pose locale manuelle. Matériau Blender reconstruit, rendu natif et transformation finale de l équipement non validés en jeu.')


def restore(data):
    if data.get('format') != 'KHARVOX_GRENADE_AUTHORING' or data.get('version') != 1 or data.get('equipment') != 'frag_grenade':
        raise ValueError('Pose grenade incompatible.')
    if data['native_reference_sha256'] != sha(OUT / 'reference-native.json'):
        raise ValueError('Référence native modifiée.')
    rig = bpy.data.objects['RIG_left']
    if set(data['hand']['bones']) != set(rig.pose.bones.keys()):
        raise ValueError('Squelette incompatible.')
    raw = [data['hand'][k] for k in ('root_matrix_cm', 'rig_matrix_local', 'mesh_matrix_local')]
    raw += list(data['hand']['bones'].values()) + [data['grenade']['matrix_anchor_local_cm']]
    raw += list(data['grenade']['mesh_local_matrices'].values())
    if any(np.asarray(m).shape != (4, 4) or not np.isfinite(m).all() for m in raw):
        raise ValueError('Matrice de pose invalide.')
    bpy.data.objects['MAIN_left'].matrix_world = Matrix(data['hand']['root_matrix_cm'])
    rig.matrix_local = Matrix(data['hand']['rig_matrix_local'])
    bpy.data.objects['PRAETOR_left'].matrix_local = Matrix(data['hand']['mesh_matrix_local'])
    for name, matrix in data['hand']['bones'].items():
        rig.pose.bones[name].matrix_basis = Matrix(matrix)
    bpy.context.view_layer.update()
    bpy.data.objects[CONTROL].matrix_world = bpy.data.objects[ANCHOR].matrix_world @ Matrix(data['grenade']['matrix_anchor_local_cm'])
    for name, matrix in data['grenade']['mesh_local_matrices'].items():
        bpy.data.objects[name].matrix_local = Matrix(matrix)
    bpy.context.view_layer.update()


def geometry(obj):
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        points = [evaluated.matrix_world @ v.co for v in mesh.vertices]
        faces = [tuple(p.vertices) for p in mesh.polygons]
        return points, faces
    finally:
        evaluated.to_mesh_clear()


def audit():
    hand_points, hand_faces = geometry(bpy.data.objects['PRAETOR_left'])
    hand_tree = BVHTree.FromPolygons(hand_points, hand_faces)
    overlaps = 0
    for obj in bpy.data.objects[CONTROL].children:
        points, faces = geometry(obj)
        overlaps += len(hand_tree.overlap(BVHTree.FromPolygons(points, faces)))
    return dict(stage='BLENDER_ONLY', intersecting_triangle_pairs=overlaps,
        intersection_check='Triangle surfaces seulement ; ne détecte pas un volume entièrement contenu.',
        anchor_parent_bone=bpy.data.objects[ANCHOR].parent_bone,
        finger_controls=sum(not b.hide for b in bpy.data.objects['RIG_left'].data.bones),
        manual_pose_validated=False, runtime_changes=False)


def save_pose():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    verify_sources()
    report = audit()
    POSE.write_text(json.dumps(payload(), indent=2, ensure_ascii=False), encoding='utf-8')
    (OUT / 'diagnostic-initial.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=str(SCENE))
    return report


class KHARVOX_OT_grenade_action(bpy.types.Operator):
    bl_idname = 'kharvox.grenade_action'
    bl_label = 'Atelier grenade'
    action: bpy.props.StringProperty()
    def execute(self, context):
        try:
            if self.action == 'SAVE':
                report = save_pose()
                self.report({'WARNING'} if report['intersecting_triangle_pairs'] else {'INFO'},
                    'Pose enregistrée : ' + str(POSE) + '; intersections : ' + str(report['intersecting_triangle_pairs']))
            elif self.action == 'LOAD':
                verify_sources()
                restore(json.loads(POSE.read_text(encoding='utf-8')))
                self.report({'INFO'}, 'Pose rechargée, aucune intégration jeu.')
            elif self.action == 'CHECK':
                report = audit()
                (OUT / 'diagnostic-initial.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
                self.report({'INFO'}, 'Paires de triangles en intersection : ' + str(report['intersecting_triangle_pairs']))
            elif self.action == 'HAND':
                poseur.activate(context, 'left', tool='builtin.move')
            elif self.action == 'VIEW':
                for area in context.screen.areas:
                    if area.type == 'VIEW_3D': area.spaces.active.region_3d.view_perspective = 'CAMERA'
            else:
                if context.object and context.object.mode != 'OBJECT': bpy.ops.object.mode_set(mode='OBJECT')
                bpy.ops.object.select_all(action='DESELECT')
                obj = bpy.data.objects[ANCHOR if self.action == 'ANCHOR' else CONTROL]
                obj.select_set(True);context.view_layer.objects.active = obj
                if context.area and context.area.type == 'VIEW_3D':
                    bpy.ops.wm.tool_set_by_id(name='builtin.rotate' if self.action == 'ROTATE' else 'builtin.move')
        except Exception as error:
            self.report({'ERROR'}, str(error));return {'CANCELLED'}
        return {'FINISHED'}


class KHARVOX_PT_grenade(bpy.types.Panel):
    bl_label = 'Grenade / Praetor gauche'
    bl_idname = 'KHARVOX_PT_grenade'
    bl_space_type = 'VIEW_3D'; bl_region_type = 'UI'; bl_category = 'KHARVOX'
    def draw(self, context):
        layout = self.layout
        layout.label(text='Grenade fragmentaire du jeu', icon='INFO')
        row = layout.row(align=True)
        for action, label in [('MOVE', 'Déplacer grenade'), ('ROTATE', 'Tourner grenade')]:
            row.operator('kharvox.grenade_action', text=label).action = action
        layout.operator('kharvox.grenade_action', text='Déplacer la main entière').action = 'HAND'
        for finger, label in zip(poseur.FINGERS, poseur.LABELS):
            row = layout.row(align=True);row.label(text=label)
            for segment in (1, 2, 3):
                op = row.operator('kharvox.choose_joint', text=str(segment))
                op.side, op.bone, op.tool = 'left', 'lefthand' + finger + str(segment), 'builtin.rotate'
        layout.label(text='Anneaux : tourner ; G / R ; Ctrl+Z')
        layout.operator('kharvox.grenade_action', text='Voir le point d’attache').action = 'ANCHOR'
        layout.operator('kharvox.grenade_action', text='Vue de travail').action = 'VIEW'
        layout.operator('kharvox.grenade_action', text='Vérifier les intersections').action = 'CHECK'
        layout.operator('kharvox.grenade_action', text='Enregistrer la pose', icon='FILE_TICK').action = 'SAVE'
        layout.operator('kharvox.grenade_action', text='Recharger la pose').action = 'LOAD'
        layout.label(text='Sauvegarde : build/grenade-workshop/frag')
        layout.label(text='STOP : validation avant intégration jeu')
        layout.label(text='Matériaux Blender reconstruits, pas idTech 6')


def register():
    for cls in (poseur.KHARVOX_OT_choose, KHARVOX_OT_grenade_action, KHARVOX_PT_grenade):
        old = getattr(bpy.types, cls.__name__, None)
        if old: bpy.utils.unregister_class(old)
        bpy.utils.register_class(cls)
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                space = area.spaces.active
                space.show_region_ui = True
                space.shading.type = 'MATERIAL'
                space.overlay.show_relationship_lines = False


def build():
    if SCENE.exists() or POSE.exists(): raise ValueError('Atelier existant : ouvrir sans reconstruire.')
    verify_sources()
    reference = json.loads((OUT / 'reference-native.json').read_text())
    factor = reference['models']['grenade_frag']['native_compare']['factor']
    cm_per_native = 100 / reference['units_per_meter']
    bpy.ops.wm.open_mainfile(filepath=str(HAND_SOURCE), use_scripts=False)
    if bpy.context.object and bpy.context.object.mode != 'OBJECT': bpy.ops.object.mode_set(mode='OBJECT')
    hand = bpy.data.objects['PRAETOR_GAUCHE'];rig = bpy.data.objects['SQUELETTE_PRAETOR_GAUCHE']
    worlds = {obj: obj.matrix_world.copy() for obj in (hand, rig)}
    for obj in (hand, rig):obj.parent = None;obj.matrix_world = worlds[obj]
    for obj in list(bpy.context.scene.objects):
        if obj not in (hand, rig): bpy.data.objects.remove(obj, do_unlink=True)
    hand.name, rig.name = 'PRAETOR_left', 'RIG_left'
    builder.pose(rig, 'left', {finger: 'free' for finger in poseur.FINGERS})
    wrist, basis, _ = builder.landmarks(rig, 'left')
    root = bpy.data.objects.new('MAIN_left', None);bpy.context.scene.collection.objects.link(root)
    root.empty_display_type='CIRCLE';root.empty_display_size=3;root.location=Vector(wrist)
    root.lock_scale=(True,)*3
    for obj in (hand, rig):
        world=obj.matrix_world.copy();obj.parent=root;obj.matrix_world=world
    hand.hide_select=True;rig.hide_select=False;rig.show_in_front=True;rig.data.display_type='WIRE'
    for bone in rig.data.bones:
        visible=any(bone.name=='lefthand'+f+str(i) for f in poseur.FINGERS for i in (1,2,3))
        bone.hide=not visible
        pb=rig.pose.bones[bone.name]
        pb.lock_rotation=(False,)*3 if visible else (True,)*3
        pb.lock_rotation_w=not visible
        pb.lock_location=pb.lock_scale=(True,)*3
    bpy.context.view_layer.update()
    frame=(rig.matrix_world @ rig.pose.bones['lefthand'].matrix)
    normalized=frame.to_3x3().normalized().to_4x4();normalized.translation=frame.translation
    anchor=bpy.data.objects.new(ANCHOR,None);bpy.context.scene.collection.objects.link(anchor)
    anchor.parent=rig;anchor.parent_type='BONE';anchor.parent_bone='lefthand'
    bpy.context.view_layer.update();anchor.matrix_world=normalized
    anchor.empty_display_type='ARROWS';anchor.empty_display_size=2;anchor.show_name=True
    anchor.show_in_front=True;anchor.lock_location=anchor.lock_rotation=anchor.lock_scale=(True,)*3
    bpy.context.view_layer.update()
    anchor['bone_to_anchor_bind']=json.dumps(rows(frame.inverted() @ anchor.matrix_world))
    control=bpy.data.objects.new(CONTROL,None);bpy.context.scene.collection.objects.link(control)
    control.parent=anchor;control.empty_display_type='CUBE';control.empty_display_size=2
    control.lock_scale=(True,)*3
    model=next(m for rt in review.Cast.load(str(CAST)).Roots() for m in rt.ChildrenOfType(review.Model))
    all_points=[]
    for i, mesh in enumerate(model.Meshes()):
        points=np.array(mesh.VertexPositionBuffer()).reshape(-1,3)*factor*cm_per_native
        triangles=np.array(mesh.FaceBuffer()).reshape(-1,3)
        uv=np.array(mesh.VertexUVLayerBuffer(0)).reshape(-1,2);uv[:,1]=1-uv[:,1]
        material=review.texture_material('Grenade frag / textures DOOM',OUT/'textures/BaseColor.png',OUT/'textures/Normal.png',OUT/'textures/MetallicRoughness.png')
        obj=review.mesh_object('GRENADE_FRAG_'+str(i),points,triangles,uv,material,bpy.context.scene.collection)
        obj.parent=control;obj.matrix_local=Matrix.Identity(4);obj.hide_select=True
        all_points.append(points)
    points=np.concatenate(all_points);centre=(points.min(0)+points.max(0))*.5
    roots=np.array([rig.matrix_world @ rig.pose.bones['lefthand'+f+'1'].head for f in poseur.FINGERS[:4]])
    # For this LEFT Praetor rig radial x forward faces the dorsal armor.
    # The grenade and the working view belong on the opposite, palmar side.
    target=wrist*.4+roots.mean(0)*.6-basis[:,2]*((points.max(0)[1]-points.min(0)[1])*.5+1.)
    rotation=np.column_stack((-basis[:,0],basis[:,2],basis[:,1]))
    placement=Matrix.Identity(4)
    for row in range(3):
        for col in range(3):placement[row][col]=float(rotation[row,col])
    placement.translation=Vector(target-rotation@centre)
    control.matrix_world=placement
    scene=bpy.context.scene;scene.name='KHARVOX - grenade frag / pose manuelle'
    scene.unit_settings.system='METRIC';scene.unit_settings.scale_length=.01;scene.unit_settings.length_unit='CENTIMETERS'
    centre_view=(Vector(wrist)+Vector(target))*.5
    offset=-basis[:,0]*.45-basis[:,2]+basis[:,1]*.25
    cam=review.camera(scene,centre_view,46,offset=offset)
    scene.render.engine='BLENDER_EEVEE';scene.render.resolution_x=1100;scene.render.resolution_y=850;scene.render.resolution_percentage=100
    review.lighting(scene,centre_view,35)
    for obj in scene.objects:
        if obj.type in ('LIGHT','CAMERA'):obj.hide_select=True
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type=='VIEW_3D':
                area.spaces.active.region_3d.view_rotation=cam.rotation_euler.to_quaternion()
                area.spaces.active.region_3d.view_distance=50
                area.spaces.active.region_3d.view_location=centre_view
                area.spaces.active.clip_start=.01;area.spaces.active.clip_end=10000
    bpy.ops.object.select_all(action='DESELECT');control.select_set(True);bpy.context.view_layer.objects.active=control
    register();save_pose()


if __name__=='__main__':
    if '--build' in sys.argv:build()
    elif '--verify' in sys.argv:
        verify_sources();saved=json.loads(POSE.read_text());restore(saved);current=payload()
        assert np.allclose(current['grenade']['matrix_anchor_local_cm'],saved['grenade']['matrix_anchor_local_cm'],atol=2e-4)
        assert set(current['hand']['bones'])==set(saved['hand']['bones'])
        before=np.array(geometry(bpy.data.objects['PRAETOR_left'])[0])
        grenade_before=bpy.data.objects[CONTROL].matrix_world.copy()
        bpy.data.objects['MAIN_left'].location.x+=2
        bpy.context.view_layer.update()
        moved=np.array(geometry(bpy.data.objects['PRAETOR_left'])[0])
        assert np.allclose(moved-before,[2,0,0],atol=2e-4), 'Whole hand motion failed'
        assert np.allclose(np.array(bpy.data.objects[CONTROL].matrix_world.translation-grenade_before.translation),[2,0,0],atol=2e-4), 'Anchor did not follow hand'
        restore(saved)
        finger=bpy.data.objects['RIG_left'].pose.bones['lefthandindex2']
        from mathutils import Quaternion
        finger.rotation_quaternion @= Quaternion((1,0,0),.25)
        bpy.context.view_layer.update()
        finger_delta=float(np.linalg.norm(np.array(geometry(bpy.data.objects['PRAETOR_left'])[0])-before,axis=1).max())
        assert finger_delta>.01, 'Finger rig did not deform the mesh'
        assert np.allclose(np.array(bpy.data.objects[CONTROL].matrix_world),np.array(grenade_before),atol=2e-4), 'Finger unexpectedly moved grenade'
        restore(saved)
        assert np.allclose(np.array(geometry(bpy.data.objects['PRAETOR_left'])[0]),before,atol=2e-4), 'Pose did not restore'
        for name,matrix in saved['hand']['bones'].items():
            assert np.allclose(np.array(bpy.data.objects['RIG_left'].pose.bones[name].matrix_basis),matrix,atol=2e-4),name
        report=audit();report.update(scene_reopened=True,pose_roundtrip_verified=True,input_hashes_unchanged=True,
            whole_hand_translation_verified=True,grenade_follows_hand=True,independent_finger_deformation_cm=finger_delta,
            finger_does_not_move_grenade=True,original_pose_restored=True)
        (OUT/'verification.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
        print('GRENADE_VERIFIED '+json.dumps(report),flush=True)
    else:register()
