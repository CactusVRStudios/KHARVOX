"""Floating wrist HUD authoring in a separate Blender copy. No game export."""
import sys
sys.dont_write_bytecode = True
import hashlib
import importlib.util
import json
import math
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector, Quaternion

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/hud-floating-preview'
SCENE = OUT / 'HUD-Suspendu-Praetor.blend'
POSE = OUT / 'hud-floating.json'
SOURCE = ROOT / 'build/hud-context-preview/HUD-Contextuel-Praetor.blend'
SOURCE_POSE = SOURCE.with_name('hud-context.json')
spec = importlib.util.spec_from_file_location('floating_existing_context', ROOT/'KHARVOX/tools/hud_context_preview.py')
context_hud = importlib.util.module_from_spec(spec)
spec.loader.exec_module(context_hud)
base = context_hud.base
context_hud.OUT = base.OUT = OUT
context_hud.SCENE = base.SCENE = SCENE


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def link_update():
    obj = bpy.data.objects.get('LIAISON_HUD_SUSPENDU')
    if not obj:
        return
    local = bpy.data.objects[base.ANCHOR].matrix_world.inverted() @ bpy.data.objects[base.CONTROL].matrix_world
    obj.data.splines[0].points[1].co = (*local.translation, 1)


def update(self, context):
    scene = context.scene
    if 'floating_base' not in scene or scene.get('floating_busy'):
        return
    initial = Matrix(json.loads(scene['floating_base']))
    center = Vector(scene['floating_center'])
    up = Vector(scene['floating_height_axis'])
    outward = Vector(scene['floating_outward_axis'])
    axis = Vector(scene['floating_tilt_axis'])
    rotation = Quaternion(axis, math.radians(self.tilt)).to_matrix().to_4x4()
    transform = Matrix.Translation(center + self.height*up + self.distance*outward) @ rotation @ Matrix.Translation(-center)
    bpy.data.objects[base.CONTROL].matrix_world = bpy.data.objects[base.ANCHOR].matrix_world @ transform @ initial
    context.view_layer.update()
    link_update()


def volume_update(self, context):
    obj = bpy.data.objects.get('VOLUME_ARMURE_GAUCHE')
    if obj:
        obj.hide_set(not self.volume)


class KHARVOX_FloatingSettings(bpy.types.PropertyGroup):
    height: bpy.props.FloatProperty(name='Hauteur au-dessus du bras (cm)', description='Direction radiale vers le haut dans la vue de consultation initiale ; reste solidaire du bras', min=-15, max=60, default=40, update=update)
    distance: bpy.props.FloatProperty(name='Déport hors de l’armure (cm)', description='Direction extérieure initiale de la face HUD, vers le casque témoin', min=-10, max=30, default=4, update=update)
    tilt: bpy.props.FloatProperty(name='Inclinaison vers le regard (°)', min=-45, max=45, default=5, update=update)
    volume: bpy.props.BoolProperty(name='Armure réelle en fil de fer', default=False, update=volume_update)


def clipped_area(triangle, half_width, half_height):
    polygon = list(triangle)
    for coordinate, sign, limit in ((0,1,half_width),(0,-1,half_width),(1,1,half_height),(1,-1,half_height)):
        clipped = []
        if not polygon:
            return 0.
        for index, end in enumerate(polygon):
            start = polygon[index-1]
            a, b = limit-sign*start[coordinate], limit-sign*end[coordinate]
            if (a >= 0) != (b >= 0):
                clipped.append(start+(end-start)*a/(a-b))
            if b >= 0:
                clipped.append(end)
        polygon = clipped
    if len(polygon) < 3:
        return 0.
    p = np.array(polygon)
    return abs(float(np.sum(p[:,0]*np.roll(p[:,1],-1)-p[:,1]*np.roll(p[:,0],-1))))*.5


def silhouette_overlaps(points, triangles, eye, panel):
    world = np.array(panel.matrix_world)
    center, axes = world[:3,3], world[:3,:3]
    directions = points-eye
    denominator = directions @ axes[:,2]
    numerator = float((center-eye) @ axes[:,2])
    if np.any(denominator*numerator <= 1e-7):
        return dict(triangles=None, reason='Armure traversant le plan de projection : contrôle conservateur indéterminé')
    projected = eye + directions*(numerator/denominator)[:,None]
    xy = (projected-center) @ axes[:,:2]
    faces = xy[triangles]
    half_width, half_height = panel['width_cm']/2, panel['height_cm']/2
    candidate = ((faces[:,:,0].min(1) <= half_width) & (faces[:,:,0].max(1) >= -half_width)
        & (faces[:,:,1].min(1) <= half_height) & (faces[:,:,1].max(1) >= -half_height))
    intersections = sum(clipped_area(t,half_width,half_height)>1e-6 for t in faces[candidate])
    return dict(triangles=intersections)


def audit():
    scene = bpy.context.scene
    values = scene.kharvox_context
    original_roll = values.roll
    details = []
    try:
        for angle in (-150,-135,-120,-105,-90,-75,-60,-45,-30,-15,0,15,30,45,60,75,90,105,120,135,150):
            values.roll = angle
            bpy.context.view_layer.update()
            points, triangles = context_hud.geometry()
            geometry = context_hud.geometry_audit()
            metrics = context_hud.gesture_metrics(scene)
            in_consultation = (metrics['facing'] >= math.cos(math.radians(values.face_off))
                and metrics['fov'] >= math.cos(math.radians(values.fov_off))
                and values.near_cm-3 <= metrics['distance_cm'] <= values.far_cm+5)
            rows = []
            if in_consultation:
                for eye in ('OEIL_GAUCHE','OEIL_DROIT'):
                    origin = np.array(bpy.data.objects[eye].matrix_world.translation)
                    for name in base.PANELS:
                        rows.append(dict(eye=eye,panel=name,**silhouette_overlaps(points,triangles,origin,bpy.data.objects[name])))
            details.append(dict(roll_degrees=angle,inside_gesture_exit_envelope=in_consultation,
                intersections_3d=sum(p['intersections'] for p in geometry['panels']),
                projections=rows,metrics=metrics))
    finally:
        values.roll = original_roll
        bpy.context.view_layer.update()
    source = json.loads(SOURCE_POSE.read_text(encoding='utf-8-sig'))
    current = base.payload()
    dimensions_unchanged = all(current['transforms'][name][key] == source['transforms'][name][key]
        for name in base.PANELS for key in ('width_cm','height_cm','dimensions_provisional'))
    anchor_unchanged = np.allclose(current['anchor']['frame_in_grip_xr_cm'],source['anchor']['frame_in_grip_xr_cm'],atol=2e-4)
    return dict(stage='BLENDER_ONLY_AWAITING_HUMAN_VALIDATION',source_scene=str(SOURCE),source_pose=str(SOURCE_POSE),
        dimensions_unchanged=dimensions_unchanged,anchor_unchanged=bool(anchor_unchanged),
        anchor_count=sum(o.name==base.ANCHOR for o in scene.objects),anchor_bone=bpy.data.objects[base.ANCHOR].parent_bone,
        adjustments=dict(height_cm=scene.kharvox_floating.height,outward_cm=scene.kharvox_floating.distance,tilt_degrees=scene.kharvox_floating.tilt),
        physical_intersections=sum(row['intersections_3d'] for row in details),
        consultation_projected_overlaps=sum(item['triangles'] or 0 for row in details for item in row['projections']),
        indeterminate_projections=sum(item['triangles'] is None for row in details for item in row['projections']),
        consultation_angles=[row['roll_degrees'] for row in details if row['inside_gesture_exit_envelope']],sweep=details,
        appearance='Maquette existante conservée : chiffres fixes, police et icônes reconstitués ; pas de SWF natif.',
        limitations=['Casque témoin, pas une mesure de ta posture réelle.',
            'Rotation rigide du bras autour de son axe longitudinal ; pas une simulation de l’anatomie du coude.',
            'Recouvrement projeté contrôlé dans les deux yeux aux angles testés où le geste peut rester actif ; les postures intermédiaires et autres mouvements restent à valider.',
            'Aucune promesse de correction du rendu natif en jeu à cette étape.'],
        game_exported=False,game_compiled=False,graphics_pipeline_changed=False)


def write_pose(report):
    data = base.payload()
    data.update(stage='blender_floating_preview_awaiting_human_validation',runtime_ready=False,
        gesture_preview={key:getattr(bpy.context.scene.kharvox_context,key) for key in
            ('face_on','face_off','fov_on','fov_off','near_cm','far_cm','on_ms','off_ms','fade_in_ms','fade_out_ms')},
        floating_adjustments=report['adjustments'],geometry_check=report,
        appearance=report['appearance'],source_pose_sha256=sha(SOURCE_POSE))
    POSE.write_text(json.dumps(data,indent=2,ensure_ascii=False),encoding='utf-8')


class KHARVOX_OT_floating_action(bpy.types.Operator):
    bl_idname = 'kharvox.floating_action'
    bl_label = 'HUD suspendu'
    action: bpy.props.StringProperty()
    def execute(self, context):
        try:
            if self.action == 'BASE':
                scene = context.scene
                scene['floating_base'] = json.dumps(base.rows(bpy.data.objects[base.ANCHOR].matrix_world.inverted() @ bpy.data.objects[base.CONTROL].matrix_world))
                scene['floating_busy'] = True
                values = scene.kharvox_floating
                values.height = values.distance = values.tilt = 0
                scene['floating_busy'] = False
                self.report({'INFO'},'Placement actuel conservé comme base des curseurs')
                return {'FINISHED'}
            report = audit()
            (OUT/'diagnostic.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
            if self.action == 'SAVE':
                if not report['dimensions_unchanged'] or not report['anchor_unchanged']:
                    raise ValueError('Dimensions ou ancrage modifiés ; enregistrement refusé')
                write_pose(report)
                bpy.context.preferences.filepaths.save_version = 0
                bpy.ops.wm.save_as_mainfile(filepath=str(SCENE))
                if report['physical_intersections'] or report['consultation_projected_overlaps'] or report['indeterminate_projections']:
                    self.report({'WARNING'},'Pose enregistrée ; dégagement incomplet : consulter diagnostic.json avant validation')
                else:
                    self.report({'INFO'},'Position enregistrée : '+str(POSE))
            else:
                self.report({'INFO'},'Intersections 3D : %d ; triangles projetés sur HUD : %d' % (report['physical_intersections'],report['consultation_projected_overlaps']))
        except Exception as error:
            self.report({'ERROR'},str(error));return {'CANCELLED'}
        return {'FINISHED'}


def draw(self, context):
    layout = self.layout
    layout.label(text='HUD suspendu — préparation Blender',icon='INFO')
    layout.label(text='Ancrage existant : LeftForeArmRoll2')
    row = layout.row(align=True)
    row.operator('kharvox.hud_left_select',text='Déplacer HUD').target=base.CONTROL
    op=row.operator('kharvox.hud_left_select',text='Incliner HUD');op.target=base.CONTROL;op.rotate=True
    for key in ('height','distance','tilt'):
        layout.prop(context.scene.kharvox_floating,key)
    layout.label(text='G / R pour affiner librement')
    layout.operator('kharvox.floating_action',text='Utiliser ce placement comme base').action='BASE'
    layout.operator('kharvox.hud_left_select',text='Voir HUD_anchor_left').target=base.ANCHOR
    layout.prop(context.scene.kharvox_floating,'volume')
    row = layout.row(align=True)
    for action,label in (('EYE_CENTER','Vue joueur'),('EYE_LEFT','Œil G'),('EYE_RIGHT','Œil D')):
        row.operator('kharvox.context_action',text=label).action=action
    layout.prop(context.scene.kharvox_context,'inspection')
    layout.prop(context.scene.kharvox_context,'roll')
    row = layout.row(align=True)
    for action,label in (('SUPINATION','Consulter'),('PRONATION','Retourner le poignet')):
        row.operator('kharvox.context_action',text=label).action=action
    layout.label(text='Supination : seuils et fondus existants conservés')
    layout.operator('kharvox.floating_action',text='Vérifier bras + deux yeux').action='CHECK'
    layout.operator('kharvox.floating_action',text='Enregistrer la position HUD',icon='FILE_TICK').action='SAVE'
    layout.label(text='Export local : hud-floating.json')
    layout.label(text='STOP : validation avant conversion / jeu')
    layout.label(text='Polices / icônes : maquette existante')


original_tick = context_hud.tick
def tick():
    link_update()
    return original_tick()


def register():
    context_hud.tick = tick
    context_hud.register()
    for cls in (KHARVOX_FloatingSettings,KHARVOX_OT_floating_action):
        old = getattr(bpy.types,cls.__name__,None)
        if old:
            bpy.utils.unregister_class(old)
        bpy.utils.register_class(cls)
    bpy.types.Scene.kharvox_floating = bpy.props.PointerProperty(type=KHARVOX_FloatingSettings)
    base.KHARVOX_PT_hud_left.draw = draw


def build():
    if SCENE.exists() or POSE.exists():
        raise ValueError('Copie déjà créée : ouvrir le CMD pour préserver la pose enregistrée')
    sealed = {str(path):sha(path) for path in (SOURCE,SOURCE_POSE,ROOT/'artifacts/KHARVOX-VR-Valide/KharvoxLayer.dll',ROOT/'build/hud-scale-audit/test/KharvoxLayer.dll')}
    bpy.ops.wm.open_mainfile(filepath=str(SOURCE),use_scripts=False)
    scene = bpy.context.scene
    rig = bpy.data.objects['SQUELETTE_PRAETOR_GAUCHE']
    bones = {bone.name:base.rows(bone.matrix_basis) for bone in rig.pose.bones}
    source_panels = {name:base.rows(bpy.data.objects[base.CONTROL].matrix_world.inverted() @ bpy.data.objects[name].matrix_world) for name in base.PANELS}
    register()
    scene.kharvox_context.roll = 0
    scene.kharvox_context.inspection = True
    scene['hud_context_alpha'] = 1.
    bpy.context.view_layer.update()
    anchor = bpy.data.objects[base.ANCHOR]
    control = bpy.data.objects[base.CONTROL]
    head = bpy.data.objects['CASQUE_APERCU']
    life = bpy.data.objects['HUD_Vie_Armure']
    inverse = anchor.matrix_world.inverted()
    normal = life.matrix_world.col[2].xyz.normalized()
    arm_axis = Vector(scene['context_axis']).normalized()
    upward = head.matrix_world.col[1].xyz.normalized()
    upward = (upward-arm_axis*upward.dot(arm_axis)).normalized()
    center = sum((bpy.data.objects[name].matrix_world.translation for name in base.PANELS),Vector())/3
    toward_eye = (head.matrix_world.translation-(center+upward*40+normal*4)).normalized()
    tilt_axis = normal.cross(toward_eye)
    if tilt_axis.length<1e-5:
        tilt_axis = life.matrix_world.col[0].xyz
    scene['floating_base'] = json.dumps(base.rows(inverse @ control.matrix_world))
    scene['floating_center'] = list(inverse @ center)
    scene['floating_height_axis'] = list(inverse.to_3x3() @ upward)
    scene['floating_outward_axis'] = list(inverse.to_3x3() @ normal)
    scene['floating_tilt_axis'] = list((inverse.to_3x3() @ tilt_axis).normalized())
    update(scene.kharvox_floating,bpy.context)
    # Reframe only the preview cameras. Their location/IPD stay unchanged.
    target = (anchor.matrix_world.translation+bpy.data.objects['HUD_Vie_Armure'].matrix_world.translation)*.5
    forward = (target-head.matrix_world.translation).normalized()
    right = forward.cross(upward).normalized()
    up = right.cross(forward).normalized()
    camera_world = head.matrix_world.copy()
    for column,axis in enumerate((right,up,-forward)):
        for row in range(3):
            camera_world[row][column] = axis[row]
    head.matrix_world = camera_world
    head.data.angle = math.radians(75)
    for eye_name in ('OEIL_GAUCHE','OEIL_DROIT'):
        bpy.data.objects[eye_name].data.angle = head.data.angle
    scene.camera = head
    scene.render.resolution_x,scene.render.resolution_y,scene.render.resolution_percentage = 1600,1100,100
    volume = bpy.data.objects['PRAETOR_GAUCHE'].copy()
    volume.name = 'VOLUME_ARMURE_GAUCHE'
    scene.collection.objects.link(volume)
    volume.display_type='WIRE';volume.show_in_front=False;volume.hide_render=True;volume.hide_select=True
    volume.color=(.2,.7,1,1);volume.hide_set(True)
    data = bpy.data.curves.new('LIAISON_HUD_SUSPENDU','CURVE');data.dimensions='3D'
    spline = data.splines.new('POLY');spline.points.add(1);spline.points[0].co=(0,0,0,1)
    link = bpy.data.objects.new('LIAISON_HUD_SUSPENDU',data);scene.collection.objects.link(link)
    link.parent=anchor;link.hide_render=True;link.hide_select=True;link.show_in_front=False
    link_update()
    # One proposal using the same controls exposed to the artist; no solver.
    for height in (40,):
        scene.kharvox_floating.height=height
        report=audit()
        print('FLOATING_CHECK '+json.dumps(dict(height=height,intersections=report['physical_intersections'],
            overlaps=report['consultation_projected_overlaps'],indeterminate=report['indeterminate_projections'],
            angles=report['consultation_angles'],affected=[dict(angle=row['roll_degrees'],panels=row['projections'])
                for row in report['sweep'] if any(item['triangles'] for item in row['projections'])])),flush=True)
        if report['consultation_angles'] and not (report['physical_intersections'] or report['consultation_projected_overlaps'] or report['indeterminate_projections']):
            break
    else:
        raise ValueError('Dégagement non confirmé : aucune scène livrée comme validée')
    assert report['dimensions_unchanged'] and report['anchor_unchanged'] and report['anchor_count']==1
    for name,matrix in source_panels.items():
        assert np.allclose(matrix,np.array(control.matrix_world.inverted() @ bpy.data.objects[name].matrix_world),atol=2e-4)
    assert all(np.allclose(matrix,np.array(rig.pose.bones[name].matrix_basis),atol=1e-6) for name,matrix in bones.items())
    assert all(sha(Path(path))==digest for path,digest in sealed.items())
    report.update(protected_inputs=sealed,hand_bones_unchanged=True,relative_panel_layout_unchanged=True,
        read_direction='Axes de lecture existants conservés par transformation rigide, aucune réflexion ni échelle négative.')
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'diagnostic.json').write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding='utf-8')
    write_pose(report)
    (OUT/'LIRE-MOI.txt').write_text('HUD SUSPENDU — BLENDER UNIQUEMENT\n\nOuvrir ATELIER_HUD_SUSPENDU.cmd.\nN > KHARVOX : hauteur, déport hors armure et inclinaison.\nLa hauteur suit une direction radiale du bras, verticale dans la vue initiale.\nDéport : éloignement de la surface initiale vers le casque témoin.\nCes directions restent solidaires de l’ancrage pendant la rotation du bras.\nG / R permettent aussi de déplacer et orienter le groupe. Après un réglage G/R, cliquer « Utiliser ce placement comme base » avant de reprendre les curseurs.\nConsulter / Retourner le poignet simulent la supination avec les seuils existants.\nMode réglage garde le HUD visible. Désactiver ce mode pour voir l’activation et les fondus.\nVue joueur / Œil G / Œil D. La caméra est un témoin de travail, pas une mesure du casque réel.\nFil de fer : copie d’affichage du véritable maillage, aucun changement de squelette.\nLa liaison relie l’ancrage existant au groupe HUD ; elle n’est pas un second ancrage.\nEnregistrer la position HUD écrit hud-floating.json et HUD-Suspendu-Praetor.blend uniquement.\nLes dimensions et la disposition relative des panneaux sont conservées. La hauteur Options et les graphismes restent les approximations déjà présentes dans la maquette.\nVérifier bras + deux yeux écrit diagnostic.json, sans déplacer automatiquement le HUD.\nCette copie conserve les sources précédentes. Aucun export de jeu, aucune compilation, aucune modification du pipeline.\nSTOP : validation manuelle avant toute suite.\n',encoding='utf-8')
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type=='VIEW_3D':
                area.spaces.active.show_region_ui=True
                area.spaces.active.region_3d.view_perspective='CAMERA'
    base.select(bpy.context,base.CONTROL)
    bpy.context.preferences.filepaths.save_version=0
    scene.render.filepath=str(OUT/'apercu.png')
    bpy.ops.wm.save_as_mainfile(filepath=str(SCENE))
    bpy.ops.render.render(write_still=True)
    print('FLOATING_READY '+json.dumps({key:report[key] for key in ('adjustments','consultation_angles','physical_intersections','consultation_projected_overlaps','dimensions_unchanged','anchor_unchanged')}),flush=True)


if __name__=='__main__':
    if '--build' in sys.argv:
        build()
    else:
        register()
