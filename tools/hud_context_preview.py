"""Blender-only contextual HUD review. Never converts or builds a game package."""
import sys
sys.dont_write_bytecode = True
import importlib.util
import json
import math
import time
from pathlib import Path
import bpy
import numpy as np
from mathutils import Matrix, Vector, Quaternion
from mathutils.bvhtree import BVHTree

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/hud-context-preview'
SCENE = OUT / 'HUD-Contextuel-Praetor.blend'
SOURCE = ROOT / 'build/hud-left-workshop/HUD-Praetor-Gauche.blend'
SOURCE_POSE = ROOT / 'build/hud-left-workshop/hud-left.json'
spec = importlib.util.spec_from_file_location('context_base_hud', ROOT/'KHARVOX/tools/hud_left_workshop.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
base.OUT, base.SCENE = OUT, SCENE
STATE = dict(active=False, since=None, alpha=1., time=0.)


def gesture_metrics(scene):
    panel = bpy.data.objects['HUD_Vie_Armure']
    head = bpy.data.objects['CASQUE_APERCU']
    toward = head.matrix_world.translation-panel.matrix_world.translation
    distance = toward.length
    normal = panel.matrix_world.col[2].xyz.normalized()
    forward = -head.matrix_world.col[2].xyz.normalized()
    direction = -toward.normalized() if distance>1e-6 else Vector((0,0,0))
    return dict(distance_cm=distance, facing=normal.dot(-direction), fov=forward.dot(direction))


def advance(metrics, values, now, state):
    active=state['active']
    angle=values.face_off if active else values.face_on
    fov=values.fov_off if active else values.fov_on
    valid=(values.near_cm-(3 if active else 0)<=metrics['distance_cm']<=values.far_cm+(5 if active else 0)
        and metrics['facing']>=math.cos(math.radians(angle))
        and metrics['fov']>=math.cos(math.radians(fov)))
    if valid!=active:
        if state['since'] is None: state['since']=now
        delay=(values.off_ms if active else values.on_ms)*.001
        if now-state['since']>=delay: state['active']=valid;state['since']=None
    else: state['since']=None
    dt=max(0.,min(.1,now-state['time'])) if state['time'] else 0.
    state['time']=now
    duration=(values.fade_in_ms if state['active'] else values.fade_out_ms)*.001
    step=dt/max(.001,duration)
    state['alpha']=min(1.,state['alpha']+step) if state['active'] else max(0.,state['alpha']-step)
    return state['alpha']


def rotate_arm(self, context):
    if 'context_bind' not in context.scene: return
    root=bpy.data.objects['AVANT_BRAS_GAUCHE']
    bind=Matrix(json.loads(context.scene['context_bind']))
    pivot=Vector(context.scene['context_pivot'])
    axis=Vector(context.scene['context_axis'])
    root.matrix_world=Matrix.Translation(pivot) @ Quaternion(axis,math.radians(self.roll)).to_matrix().to_4x4() @ Matrix.Translation(-pivot) @ bind


class KHARVOX_ContextSettings(bpy.types.PropertyGroup):
    roll: bpy.props.FloatProperty(name='Rotation avant-bras (°)',min=-180,max=180,default=0,update=rotate_arm)
    inspection: bpy.props.BoolProperty(name='Mode réglage : HUD visible',default=True)
    face_on: bpy.props.FloatProperty(name='Face : apparition (°)',default=40,min=0,max=85)
    face_off: bpy.props.FloatProperty(name='Face : disparition (°)',default=55,min=0,max=89)
    fov_on: bpy.props.FloatProperty(name='Champ : apparition (°)',default=25,min=1,max=80)
    fov_off: bpy.props.FloatProperty(name='Champ : disparition (°)',default=35,min=1,max=89)
    near_cm: bpy.props.FloatProperty(name='Distance mini (cm)',default=15,min=1,max=200)
    far_cm: bpy.props.FloatProperty(name='Distance maxi (cm)',default=85,min=2,max=200)
    on_ms: bpy.props.FloatProperty(name='Délai apparition (ms)',default=120,min=0,max=1000)
    off_ms: bpy.props.FloatProperty(name='Délai disparition (ms)',default=180,min=0,max=1000)
    fade_in_ms: bpy.props.FloatProperty(name='Fondu apparition (ms)',default=160,min=1,max=1000)
    fade_out_ms: bpy.props.FloatProperty(name='Fondu disparition (ms)',default=220,min=1,max=1000)


def tick():
    scene=bpy.context.scene
    if scene.get('context_ready'):
        values=scene.kharvox_context
        alpha=advance(gesture_metrics(scene),values,time.monotonic(),STATE)
        scene['hud_context_alpha']=1. if values.inspection else alpha
        for screen in bpy.data.screens:
            for area in screen.areas:
                if area.type=='VIEW_3D':area.tag_redraw()
    return .033


class KHARVOX_OT_context_action(bpy.types.Operator):
    bl_idname='kharvox.context_action'
    bl_label='Aperçu HUD'
    action: bpy.props.StringProperty()
    def execute(self, context):
        scene=context.scene
        if self.action in ('SUPINATION','PRONATION'):
            scene.kharvox_context.inspection=False
            scene.kharvox_context.roll=0 if self.action=='SUPINATION' else 150
        elif self.action.startswith('EYE_'):
            scene.camera=bpy.data.objects[{'EYE_CENTER':'CASQUE_APERCU','EYE_LEFT':'OEIL_GAUCHE','EYE_RIGHT':'OEIL_DROIT'}[self.action]]
            for area in context.screen.areas:
                if area.type=='VIEW_3D':area.spaces.active.region_3d.view_perspective='CAMERA'
        elif self.action=='WALL':
            obj=bpy.data.objects['MUR_TEMOIN'];obj.hide_set(not obj.hide_get());obj.hide_render=obj.hide_get()
        elif self.action=='CHECK':
            report=geometry_audit()
            path=OUT/'diagnostic.json'
            previous=json.loads(path.read_text()) if path.exists() else {}
            previous.update(report)
            path.write_text(json.dumps(previous,indent=2),encoding='utf-8')
            self.report({'INFO'}, 'Triangles traversant les plans : '+str(sum(p['intersections'] for p in report['panels'])))
        return {'FINISHED'}


class KHARVOX_OT_context_save(bpy.types.Operator):
    bl_idname='kharvox.context_save'
    bl_label='Enregistrer la position HUD'
    def execute(self,context):
        try:
            values=context.scene.kharvox_context
            if values.face_off<=values.face_on or values.fov_off<=values.fov_on or values.near_cm>=values.far_cm:
                raise ValueError('Les seuils de disparition doivent dépasser ceux d’apparition.')
            data=base.payload()
            data.update(stage='blender_context_preview_awaiting_explicit_validation',runtime_ready=False,
                initial_runtime_orientation_verified=False,
                appearance='reconstitution : chiffres, jauges et couleurs ; polices/icones non natives',
                gesture_preview={key:getattr(values,key) for key in ('face_on','face_off','fov_on','fov_off','near_cm','far_cm','on_ms','off_ms','fade_in_ms','fade_out_ms')},
                plane_convention='X droite, Y haut, Z face de lecture ; rotations propres, aucune echelle negative',
                geometry_check=geometry_audit())
            (OUT/'hud-context.json').write_text(json.dumps(data,indent=2,ensure_ascii=False),encoding='utf-8')
            bpy.context.preferences.filepaths.save_version=0
            bpy.ops.wm.save_as_mainfile(filepath=str(SCENE))
        except Exception as error:
            self.report({'ERROR'},str(error));return {'CANCELLED'}
        self.report({'INFO'},'Position enregistrée dans '+str(OUT/'hud-context.json')+' ; aucune intégration jeu')
        return {'FINISHED'}


def draw(self,context):
    layout=self.layout;values=context.scene.kharvox_context
    layout.label(text='HUD contextuel — Blender uniquement',icon='INFO')
    layout.label(text='Ancrage existant : LeftForeArmRoll2')
    row=layout.row(align=True)
    row.operator('kharvox.hud_left_select',text='Déplacer HUD').target=base.CONTROL
    op=row.operator('kharvox.hud_left_select',text='Tourner HUD');op.target=base.CONTROL;op.rotate=True
    row=layout.row(align=True)
    for name,label in zip(base.PANELS,('Vie / armure','Munitions','Options')):
        row.operator('kharvox.hud_left_select',text=label).target=name
    layout.operator('kharvox.hud_left_select',text='Voir HUD_anchor_left').target=base.ANCHOR
    row=layout.row(align=True)
    for action,label in (('EYE_CENTER','Vue joueur'),('EYE_LEFT','Œil G'),('EYE_RIGHT','Œil D')):
        row.operator('kharvox.context_action',text=label).action=action
    layout.prop(values,'inspection')
    layout.prop(values,'roll')
    row=layout.row(align=True)
    for action,label in (('SUPINATION','Supination / consulter'),('PRONATION','Retourner le poignet')):
        row.operator('kharvox.context_action',text=label).action=action
    metrics=gesture_metrics(context.scene)
    layout.label(text='Face : %.0f° | champ : %.0f° | %.0f cm' % (
        math.degrees(math.acos(max(-1,min(1,metrics['facing'])))),math.degrees(math.acos(max(-1,min(1,metrics['fov'])))),metrics['distance_cm']))
    layout.label(text='Opacité : %d %%'%round(context.scene.get('hud_context_alpha',1)*100))
    box=layout.box()
    for key in ('face_on','face_off','fov_on','fov_off','near_cm','far_cm','on_ms','off_ms','fade_in_ms','fade_out_ms'):
        box.prop(values,key)
    layout.operator('kharvox.hud_left_select',text='Déplacer / orienter le casque témoin').target='CASQUE_APERCU'
    layout.operator('kharvox.context_action',text='Vérifier les intersections').action='CHECK'
    layout.operator('kharvox.context_action',text='Afficher / masquer le mur témoin').action='WALL'
    layout.label(text='G / R pour ajuster ; Ctrl+Z pour annuler')
    layout.label(text='Polices / icônes reconstituées, pas les SWF natifs')
    layout.operator('kharvox.context_save',icon='FILE_TICK')
    layout.label(text='STOP après enregistrement : validation requise')


def register():
    for cls in (KHARVOX_ContextSettings,KHARVOX_OT_context_action,KHARVOX_OT_context_save):
        old=getattr(bpy.types,cls.__name__,None)
        if old:bpy.utils.unregister_class(old)
        bpy.utils.register_class(cls)
    bpy.types.Scene.kharvox_context=bpy.props.PointerProperty(type=KHARVOX_ContextSettings)
    base.KHARVOX_PT_hud_left.draw=draw
    base.register()
    if not bpy.app.background and not bpy.app.timers.is_registered(tick):bpy.app.timers.register(tick,first_interval=.5)


def material(name,color,alpha=1):
    mat=bpy.data.materials.new(name);mat.use_nodes=True
    nodes=mat.node_tree.nodes;nodes.clear();links=mat.node_tree.links
    output=nodes.new('ShaderNodeOutputMaterial');mix=nodes.new('ShaderNodeMixShader')
    transparent=nodes.new('ShaderNodeBsdfTransparent');emission=nodes.new('ShaderNodeEmission')
    emission.inputs['Color'].default_value=(*color,1);emission.inputs['Strength'].default_value=.8
    geometry=nodes.new('ShaderNodeNewGeometry');front=nodes.new('ShaderNodeMath');front.operation='SUBTRACT';front.inputs[0].default_value=1
    links.new(geometry.outputs['Backfacing'],front.inputs[1])
    opacity=nodes.new('ShaderNodeMath');opacity.operation='MULTIPLY';opacity.inputs[1].default_value=alpha
    driver=opacity.inputs[1].driver_add('default_value').driver;driver.expression='opacity*'+str(alpha)
    var=driver.variables.new();var.name='opacity';var.targets[0].id_type='SCENE';var.targets[0].id=bpy.context.scene;var.targets[0].data_path='["hud_context_alpha"]'
    links.new(front.outputs[0],opacity.inputs[0]);links.new(opacity.outputs[0],mix.inputs[0])
    links.new(transparent.outputs[0],mix.inputs[1]);links.new(emission.outputs[0],mix.inputs[2]);links.new(mix.outputs[0],output.inputs['Surface'])
    if hasattr(mat,'surface_render_method'):mat.surface_render_method='DITHERED'
    mat.diffuse_color=(*color,alpha)
    return mat


def mesh_child(name,parent,vertices,faces,mat):
    data=bpy.data.meshes.new(name);data.from_pydata(vertices,[],faces)
    obj=bpy.data.objects.new(name,data);bpy.context.scene.collection.objects.link(obj)
    obj.parent=parent;obj.matrix_parent_inverse=Matrix.Identity(4)
    obj.data.materials.append(mat);obj.hide_select=True;obj.show_in_front=False
    return obj


def bar(parent,name,x,y,width,height,mat):
    return mesh_child(name,parent,[(x,y,.035),(x+width,y,.035),(x+width,y+height,.035),(x,y+height,.035)],[(0,1,2,3)],mat)


def line(parent,name,points,mat,width=.035,closed=False):
    curve=bpy.data.curves.new(name,'CURVE');curve.dimensions='3D';curve.bevel_depth=width;curve.bevel_resolution=1
    spline=curve.splines.new('POLY');spline.points.add(len(points)-1)
    for dst,point in zip(spline.points,points):dst.co=(*point,1)
    spline.use_cyclic_u=closed
    obj=bpy.data.objects.new(name,curve);bpy.context.scene.collection.objects.link(obj);obj.parent=parent
    obj.data.materials.append(mat);obj.hide_select=True;obj.show_in_front=False
    return obj


def text(parent,name,content,x,y,size,mat):
    curve=bpy.data.curves.new(name,'FONT');curve.body=content;curve.size=size;curve.extrude=0
    font=Path('C:/Windows/Fonts/bahnschrift.ttf')
    if font.exists():curve.font=bpy.data.fonts.load(str(font),check_existing=True)
    obj=bpy.data.objects.new(name,curve);bpy.context.scene.collection.objects.link(obj)
    obj.parent=parent;obj.location=(x,y,.06);obj.hide_select=True;obj.show_in_front=False;curve.materials.append(mat)
    return obj


def geometry():
    obj=bpy.data.objects['PRAETOR_GAUCHE'];ev=obj.evaluated_get(bpy.context.evaluated_depsgraph_get());mesh=ev.to_mesh();mesh.calc_loop_triangles()
    points=np.array([obj.matrix_world@v.co for v in mesh.vertices]);triangles=np.array([list(t.vertices) for t in mesh.loop_triangles])
    ev.to_mesh_clear();return points,triangles


def geometry_audit():
    reader=base.module('context_intersections','preview_praetor_hud_clearance.py')
    points,triangles=geometry();panels=[]
    head=bpy.data.objects['CASQUE_APERCU'].matrix_world.translation
    for name in base.PANELS:
        obj=bpy.data.objects[name];world=obj.matrix_world
        basis=np.array((world.col[2].xyz,world.col[0].xyz,world.col[1].xyz))
        intervals=reader.rectangle_depth_intervals(points,triangles,np.array(world.translation),basis,obj['width_cm'],obj['height_cm'])
        panels.append(dict(name=name,intersections=sum(a<=0<=b for a,b in intervals),
            maximum_armor_depth_cm=max((b for a,b in intervals),default=-999),
            front_facing_player=world.col[2].xyz.dot(head-world.translation)>0,
            matrix_determinant=world.to_3x3().determinant()))
    return dict(stage='BLENDER_ONLY_PHASE_B',anchor_count=sum(o.name==base.ANCHOR for o in bpy.context.scene.objects),
        anchor_bone=bpy.data.objects[base.ANCHOR].parent_bone,panels=panels,
        appearance='reconstruction : valeurs statiques, polices/icones approximatives, aucune animation SWF native',
        depth='occlusion geometrique reelle ; aucun show_in_front sur les elements du HUD',
        runtime_changes=False,phase_c_authorized=False)


def build():
    if (SCENE.exists() and '--refresh-preview' not in sys.argv) or (OUT/'hud-context.json').exists():
        raise ValueError('Scène existante : ouvrir le CMD, ne pas reconstruire une pose enregistrée.')
    source_hashes={str(p):base.sha(p) for p in (SOURCE,SOURCE_POSE,ROOT/'build/hud-left-workshop/test/KharvoxLayer.dll',ROOT/'artifacts/KHARVOX-VR-Valide/KharvoxLayer.dll')}
    bpy.ops.wm.open_mainfile(filepath=str(SOURCE));scene=bpy.context.scene
    scene.name='KHARVOX - HUD contextuel - validation Blender'
    scene['hud_context_alpha']=1.
    # Keep precisely one anchor; all proposal edits occur only in this copy.
    assert sum(o.name==base.ANCHOR for o in scene.objects)==1
    reader=base.module('context_existing_geometry','preview_praetor_hud_clearance.py')
    points,triangles=geometry();life=bpy.data.objects['HUD_Vie_Armure']
    old_matrices={name:base.rows(bpy.data.objects[name].matrix_world) for name in base.PANELS}
    old_screen=next(a for a in bpy.context.screen.areas if a.type=='VIEW_3D').spaces.active.region_3d
    old_view=old_screen.view_rotation@Vector((0,0,1))
    original_back=life.matrix_world.col[2].xyz.dot(old_view)<0
    # The previously saved initial view is behind the HUD. Turn the plane as
    # a rigid object (det +1), never negate scale or mirror text/UV coordinates.
    normal=life.matrix_world.col[2].xyz.normalized()
    if normal.dot(old_view)<0:normal=-normal
    bvh=BVHTree.FromPolygons(points.tolist(),triangles.tolist(),all_triangles=True)
    hit,face,_,_=bvh.ray_cast(life.matrix_world.translation+normal*100,-normal,200)
    if hit is not None and face.dot(normal)>.6:normal=face.normalized()
    horizontal=life.matrix_world.col[0].xyz
    horizontal=(horizontal-normal*horizontal.dot(normal)).normalized()
    up=normal.cross(horizontal).normalized()
    # Center the proposal on the anatomical anchor in the surface tangent
    # plane. The previous pose is kept untouched in its original directory.
    tangent=bpy.data.objects[base.ANCHOR].matrix_world.translation-life.matrix_world.translation
    tangent-=normal*tangent.dot(normal)
    original_life_center=life.matrix_world.translation.copy()
    correction=[]
    for name in base.PANELS:
        obj=bpy.data.objects[name];center=obj.matrix_world.translation.copy()+tangent
        if name=='HUD_Options':
            # Keep the reconstructed native option circles left of the health
            # digits. Spacing follows their displayed bounds, not a game offset.
            relative=(obj.matrix_world.translation-original_life_center).dot(horizontal)
            overlap=relative+(-10+4*1.6+.65)-(-10-.5)
            center-=horizontal*max(0.,overlap)
        world=Matrix.Identity(4)
        for i,axis in enumerate((horizontal,up,normal)):
            for j in range(3):world[j][i]=axis[j]
        basis=np.array((normal,horizontal,up))
        intervals=reader.rectangle_depth_intervals(points,triangles,np.array(center),basis,obj['width_cm'],obj['height_cm'])
        lift=max((b for a,b in intervals),default=-.2)+.2
        world.translation=center+normal*lift
        obj.matrix_world=world
        obj.show_wire=False;obj.show_name=False
        obj.data.materials.clear();obj.data.materials.append(material('Canevas transparent '+name,(0,0,0),0))
        correction.append(dict(name=name,lift_cm=lift))
    bpy.context.view_layer.update()
    colors={key:material('HUD '+key,rgb) for key,rgb in dict(white=(.85,.94,1),blue=(.02,.35,1),green=(.02,.9,.45),yellow=(1,.65,.02),frame=(.22,.55,.7)).items()}
    health=bpy.data.objects['HUD_Vie_Armure'];ammo=bpy.data.objects['HUD_Munitions'];options=bpy.data.objects['HUD_Options']
    text(health,'Lecture normale','VIE / ARMURE  -  123 ABC',-10,6,.65,colors['white'])
    text(health,'Vie 152','152',-10,2.2,2.3,colors['white']);text(health,'Croix sante','+',-5.7,2.45,1.6,colors['blue'])
    bar(health,'Fond vie',-3.8,2.7,14,1.15,colors['frame']);bar(health,'Jauge vie',-3.8,2.7,13,1.15,colors['blue'])
    text(health,'Armure 61','61',-9,-.2,1.65,colors['white'])
    line(health,'Bouclier',[(-5.9,1,.06),(-4.8,.7,.06),(-5,-.3,.06),(-5.4,-.6,.06),(-5.8,-.3,.06)],colors['green'],closed=True)
    bar(health,'Fond armure',-3.8,.1,14,.8,colors['frame']);bar(health,'Jauge armure',-3.8,.1,8.5,.8,colors['green'])
    text(ammo,'Ammo 32','32',-2,-4,2.2,colors['white']);text(ammo,'Legende ammo','MUNITIONS',2,-3.8,.8,colors['white'])
    line(ammo,'Silhouette arme',[(-7,-3.2,.06),(-3.2,-3.2,.06),(-3.2,-2.7,.06),(3,-2.7,.06),(3,-2.2,.06),(-4,-2.2,.06),(-4,-1.9,.06),(-6.5,-1.9,.06)],colors['white'],.07)
    for i in range(5):
        x=-10+i*1.6;y=0
        line(options,'Option '+str(i),[(x+math.cos(k*math.tau/32)*.65,y+math.sin(k*math.tau/32)*.65,.05) for k in range(32)],colors['yellow'] if i<2 else colors['frame'],.05,True)
    text(options,'Libelle options','OPTIONS',-10,-1.45,.65,colors['white'])
    # Place the manipulation handle on the visible HUD; preserve every child
    # world matrix so moving the origin cannot change the proposed layout.
    control=bpy.data.objects[base.CONTROL]
    child_worlds=[(child,child.matrix_world.copy()) for child in control.children]
    control.matrix_world=health.matrix_world.copy()
    bpy.context.view_layer.update()
    for child,world in child_worlds:child.matrix_world=world
    bpy.context.view_layer.update()
    center=health.matrix_world.translation
    camera_data=bpy.data.cameras.new('Casque de consultation');camera=bpy.data.objects.new('CASQUE_APERCU',camera_data);scene.collection.objects.link(camera)
    camera.matrix_world=health.matrix_world.copy();camera.matrix_world.translation=center+normal*60
    camera_data.lens=43;camera_data.clip_start=.1;camera_data.clip_end=1000;scene.camera=camera
    for name,offset in (('OEIL_GAUCHE',-3.2),('OEIL_DROIT',3.2)):
        eye=bpy.data.objects.new(name,camera_data.copy());scene.collection.objects.link(eye);eye.parent=camera;eye.location=(offset,0,0)
    root=bpy.data.objects['AVANT_BRAS_GAUCHE'];rig=bpy.data.objects['SQUELETTE_PRAETOR_GAUCHE']
    axis=(rig.matrix_world@rig.pose.bones['lefthand'].head-rig.matrix_world@rig.pose.bones['leftforearm'].head).normalized()
    scene['context_bind']=json.dumps(base.rows(root.matrix_world));scene['context_pivot']=list(center);scene['context_axis']=list(axis)
    wall_data=bpy.data.meshes.new('Mur temoin');wall_data.from_pydata([(-6,-6,0),(0,-6,0),(0,6,0),(-6,6,0)],[],[(0,1,2,3)])
    wall=bpy.data.objects.new('MUR_TEMOIN',wall_data);scene.collection.objects.link(wall);wall.matrix_world=health.matrix_world.copy();wall.matrix_world.translation=center+normal*5
    wall_mat=bpy.data.materials.new('Mur opaque de controle');wall_mat.diffuse_color=(.16,.18,.21,1);wall.data.materials.append(wall_mat);wall.hide_set(True);wall.hide_render=True
    # Inspection camera and world use ordinary depth. No overlay exemption.
    scene.render.engine='CYCLES';scene.cycles.samples=12
    scene.cycles.use_denoising=True
    scene.render.resolution_x=1200;scene.render.resolution_y=850;scene.render.resolution_percentage=100
    scene.world.use_nodes=True
    background=scene.world.node_tree.nodes.get('Background')
    background.inputs['Color'].default_value=(.25,.25,.25,1)
    background.inputs['Strength'].default_value=1
    scene.view_settings.view_transform='Standard'
    light_data=bpy.data.lights.new('Lumiere de controle','AREA');light=bpy.data.objects.new('Lumiere de controle',light_data);scene.collection.objects.link(light)
    light.matrix_world=camera.matrix_world.copy();light_data.energy=10000;light_data.shape='DISK';light_data.size=50
    register();scene['context_ready']=True
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type=='VIEW_3D':
                space=area.spaces.active;space.shading.type='MATERIAL';space.shading.use_scene_world=False
                space.show_region_ui=True;space.region_3d.view_perspective='CAMERA'
    bpy.context.view_layer.update();base.select(bpy.context,base.CONTROL)
    report=geometry_audit();report.update(source_hashes=source_hashes,original_view_was_behind_life=original_back,
        original_panel_matrices=old_matrices,proposed_normal_lifts=correction,
        anatomical_anchor_tangent_centering_cm=list(tangent),
        mirror_finding='No negative determinant; old view saw back of planes. Native GUI UV/winding still needs phase C validation.',
        rendering_finding='Hand pipeline depthTest=true, depthWrite=true. Native GUI depth/order not fully reconstructed; no runtime change.',
        geometry_clearance_margin_cm=.2)
    assert not sum(p['intersections'] for p in report['panels'])
    assert all(p['front_facing_player'] and p['matrix_determinant']>.999 for p in report['panels'])
    for path,digest in source_hashes.items():assert base.sha(Path(path))==digest
    OUT.mkdir(parents=True,exist_ok=True)
    (OUT/'diagnostic.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    (OUT/'LIRE-MOI.txt').write_text('HUD CONTEXTUEL - PHASE B BLENDER UNIQUEMENT\nN > KHARVOX > HUD gauche.\nVue joueur / Oeil G / Oeil D : lecture depuis le casque et stereo parallele 64 mm.\nMode reglage : visible pour deplacer G / tourner R.\nSupination / Retourner le poignet : simulation du geste, seuils relatifs au casque, hysteresis, delais et fondus.\nLe curseur Rotation avant-bras fait tourner le bras entier autour de son axe longitudinal sans modifier les doigts.\nLe casque temoin peut etre deplace/oriente pour tester le champ de vision relatif.\nVerifier les intersections actualise le diagnostic ; pas de correction automatique pendant tes reglages.\nMur temoin : doit masquer les elements du HUD places derriere lui. Aucun affichage a travers les obstacles.\nAncrage HUD_anchor_left existant, sur LeftForeArmRoll2 ; aucun doublon.\nReconstitution visuelle : couleurs, chiffres, jauges et options ; polices/icones approximatives et valeurs fixes, sans animations SWF natives.\nCanevas vie et ammo 25 x 14.65 cm ; Options 20 cm de large, hauteur technique provisoire.\nProposition initiale : plans paralleles au plan tangent local de protection, face de lecture corrigee par rotation rigide et degagement mesure avec marge 2 mm.\nLa pose precedente est conservee dans hud-left-workshop.\nEnregistrer la position HUD : hud-context.json et cette copie blend seulement.\nSTOP : aucune conversion runtime, aucun build ni changement du jeu. Accord explicite requis pour la phase C.\n',encoding='utf-8')
    bpy.context.preferences.filepaths.save_version=0
    bpy.ops.wm.save_as_mainfile(filepath=str(SCENE))
    scene.render.filepath=str(OUT/'apercu.png');bpy.ops.render.render(write_still=True)
    print('CONTEXT_READY '+json.dumps(dict(panels=report['panels'],lifts=correction,original_back=original_back)),flush=True)


if __name__=='__main__':
    if '--build' in sys.argv:build()
    else:register()
