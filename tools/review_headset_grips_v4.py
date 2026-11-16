"""Measured weapon scale, calibrated physical gloves; review assets only.

F4 and headset photographs belong to separate sessions. F4 does not identify
the weapon or Grip state, so no per-photo transform match is claimed here.
"""
import sys
sys.dont_write_bytecode = True
import json
import pathlib
import re
import hashlib
import math

import bpy
import numpy as np
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'KHARVOX/tools'))
import preview_praetor_pumps_v3 as pumps
reading, review = pumps.reading, pumps.review
OUT = ROOT/'build/pump-review-v4'
review.OUT = OUT


def observations():
    path = OUT/'F4-20261114-214951.log'
    frames = []
    for part in re.split(r'^=== SNAPSHOT ', path.read_text(), flags=re.M)[1:]:
        entry = dict(snapshot=int(part.split()[0]), weapon='NOT_RECORDED', grip='NOT_RECORDED')
        for role in ('PROP', 'WEAPON_PRESENT'):
            line = re.search(r'^'+role+r' .*?origin=([^ ]+) axis=([^ \n]+)', part, re.M)
            if not line:
                continue
            origin = np.array([float(v) for v in line[1].split(',')])
            axes = np.array([float(v) for v in line[2].split(',')]).reshape(3,3)
            entry[role] = dict(origin=origin.tolist(), axes=axes.tolist(),
                scale=np.linalg.norm(axes, axis=1).tolist(), determinant=float(np.linalg.det(axes)))
        if all(k in entry for k in ('PROP','WEAPON_PRESENT')):
            entry['submitted_origin_difference'] = float(np.linalg.norm(
                np.array(entry['PROP']['origin'])-entry['WEAPON_PRESENT']['origin']))
            entry['submitted_axes_max_difference'] = float(np.abs(
                np.array(entry['PROP']['axes'])-entry['WEAPON_PRESENT']['axes']).max())
        frames.append(entry)
    scales = np.array([f['PROP']['scale'] for f in frames if 'PROP' in f])
    scale = float(np.median(scales))
    if not np.allclose(scales, scale, atol=2e-5):
        raise RuntimeError('Nonconstant measured weapon scale; do not use a single fitting scale')
    result = dict(f4_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), frames=frames,
        measured_scale=scale, launcher_scale=.77, units_per_meter=39.3701,
        previous_review_scale=1, photos_session='2026-11-14 22:00-22:03',
        f4_session='2026-11-14 before 21:49:51',
        limitations=['Weapon and Grip state not recorded by F4.',
            'Photos are not synchronized to these F4 matrices.',
            'Static exported geometry does not reproduce live skinning or recoil.'])
    (OUT/'analyse-captures.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print('OBSERVED SCALE',scale,'SNAPSHOTS',len(frames),flush=True)
    return result


def side_fit(spec, objects, hand_scale, weapon_scale):
    points, faces = [], []
    for obj in objects:
        offset=len(points)
        points.extend(tuple(v.co*weapon_scale) for v in obj.data.vertices)
        faces.extend(tuple(offset+i for i in p.vertices) for p in obj.data.polygons)
    tree=BVHTree.FromPolygons(points,faces,all_triangles=True)
    best=None
    names=np.array(review.HAND_BONES)
    # Keep bone lengths and the free glove's physical size. Rotate the native
    # skeleton rather than interpolating vertex positions between finger poses.
    for recipe,data in review.AUTHORED_HANDS.items():
        if recipe[:3] not in ((10,32,35),(20,35,30),(30,50,30),(40,65,30)):
            continue
        raw,triangles,uvs,_=data
        radial=review.unit(spec['radial'])
        forward=review.unit(spec['forward'])
        rotation=np.column_stack((radial,forward,np.cross(radial,forward)))
        palm=(names=='lefthand')&(raw[:,1]>0)&(raw[:,1]<12)
        # Grasp axis derived from the curled index/middle/ring joint region.
        centers=[]
        for finger in ('index','middle','ring'):
            joints=np.array([np.median(raw[names=='lefthand'+finger+str(i)],axis=0)
                for i in (1,2,3)])*hand_scale
            yz=joints[:,1:]
            solution=np.linalg.lstsq(np.c_[2*yz,np.ones(len(yz))],np.sum(yz**2,axis=1),rcond=None)[0]
            centers.append(solution[:2])
        grasp=np.r_[0.,np.median(centers,axis=0)]
        angles=(-45,-20,0,20,45) if spec['stem']=='chainsaw' else (0,)
        for angle in angles:
          r=math.radians(angle)
          forward0=forward*math.cos(r)+np.cross(radial,forward)*math.sin(r)
          rotation=np.column_stack((radial,forward0,np.cross(radial,forward0)))
          for lateral in (-2.,0.,2.):
            for depth in (-2.,-1.,0.,1.,2.,3.,4.,5.,6.):
                target=np.array(spec['contact'])*weapon_scale+radial*lateral+rotation[:,2]*depth
                matrix=np.eye(4)
                matrix[:3,:3]=rotation*hand_scale
                matrix[:3,3]=target-rotation@grasp
                placed=review.transformed(raw,matrix)
                crossings=len(tree.overlap(pumps.bvh(placed,triangles)))
                distances=[tree.find_nearest(Vector(p))[3] for p in placed[palm][::12]]
                gap=min(d for d in distances if d is not None)
                score=(crossings,max(0,gap-1.1),gap+.04*(lateral*lateral+depth*depth)+.002*abs(angle))
                if best is None or score<best[0]:
                    best=(score,matrix,data,recipe,gap,target)
    score,matrix,data,recipe,gap,target=best
    return matrix,data,dict(crossing_triangle_pairs=score[0],palm_distance_cm=float(gap),
        contact_surface_cm=target.tolist(),finger_closure=list(recipe),hand_matrix_cm=matrix.tolist(),
        status='VISUAL_REVIEW_REQUIRED')


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    bpy.context.preferences.filepaths.save_version=0
    measured=observations()
    cm_per_unit=measured['measured_scale']*100/measured['units_per_meter']
    pumps.WEAPON_CM_PER_UNIT=cm_per_unit
    pumps.use_gpu()
    free,free_triangles,free_uvs,source=reading.physical_hand()
    hand_scale=source['meters_per_native_unit']*100
    pumps.support_poses()
    material=review.texture_material('Praetor / echelle physique conservee',review.HANDS/'BaseColor.png',
        review.HANDS/'Normal.png',review.HANDS/'MetallicRoughness.png')
    audit=dict(status='VISUAL_REVIEW_REQUIRED_NO_RUNTIME_BUILD',observations=measured,
        hand_cm_per_source_unit=hand_scale,weapon_cm_per_source_unit=cm_per_unit,weapons={})
    for key in ('shotgun','super-shotgun','gauss','chainsaw'):
        spec=dict(review.SPECS[key])
        if key=='chainsaw':
            # The former (10,13.6,3) point is beside the starter cord bones
            # (cord_01..16 and handle), not the front wraparound support bar.
            spec.update(contact=(12.3,0,17.3),radial=(0,-1,0),forward=(1,0,0))
        scene=bpy.data.scenes.new(spec['title'])
        bpy.context.window.scene=scene
        entry={}
        objects,weapon=review.weapon(spec,scene.collection,entry)
        if spec['kind']=='under':
            matrix,data,fit=pumps.fit(spec,objects,hand_scale)
        else:
            matrix,data,fit=side_fit(spec,objects,hand_scale,cm_per_unit)
        if not np.allclose(matrix[:3,:3].T@matrix[:3,:3],np.eye(3)*hand_scale**2,atol=1e-8):
            raise RuntimeError('Glove physical scale changed')
        for obj in objects:
            obj.scale=(cm_per_unit,)*3
        raw,triangles,uvs,_=data
        hand=review.mesh_object('Soutien / taille physique constante',raw,triangles,uvs,material,scene.collection)
        hand.matrix_world=Matrix(matrix.tolist())
        placed=review.transformed(raw,matrix)
        # Replay the production fixed-size contract with every recorded native
        # basis. This catches exporting centimetres with the former unit-scale
        # denominator while presenting a 0.77-scale Blender weapon.
        baked=placed/cm_per_unit
        anchor=np.array(fit['contact_surface_cm'])/cm_per_unit
        physical=cm_per_unit/100
        max_error=0.
        for frame in measured['frames']:
            native=np.array(frame['PROP']['axes']).T
            lengths=np.linalg.norm(native,axis=0)
            rotation=native/lengths
            world=native/measured['units_per_meter']
            fixed=rotation*physical
            shift=(world-fixed)@anchor
            replay=baked@fixed.T+shift
            expected=(placed/100)@rotation.T
            error=float(np.linalg.norm(replay-expected,axis=1).max())
            if error>.00002:
                raise RuntimeError('Blender/runtime physical round-trip exceeds 0.02 mm')
            max_error=max(max_error,error)
            reference=np.linalg.norm((baked[::53]-baked[0])*physical,axis=1)
            for scale in (.5,.77,1.,2.):
                varied=rotation*scale/measured['units_per_meter']
                offset=(varied-fixed)@anchor
                actual=baked@fixed.T+offset
                distances=np.linalg.norm(actual[::53]-actual[0],axis=1)
                if not np.allclose(reference,distances,atol=2e-5):
                    raise RuntimeError('Weapon scale resized the glove')
        fit['roundtrip_max_error_m']=max_error
        fit['roundtrip_native_frames']=len(measured['frames'])
        fit['export_meters_per_asset_unit']=physical
        points=np.concatenate((weapon*cm_per_unit,placed))
        center=(points.max(0)+points.min(0))/2
        span=float(np.ptp(points,axis=0).max())
        review.lighting(scene,center,span)
        pumps.render(scene,key+'-v4',center,span,spec['title'],
            'Arme : echelle mesuree 0,77 / gant : taille libre conservee / proposition',(-.6,1.5,.55))
        contact=np.array(fit['contact_surface_cm'])
        pumps.render(scene,key+'-contact-v4',contact,60,spec['title'],
            'Contact paume / doigts / poignee - controle des vrais maillages',
            (1.35,.8,-.1) if spec['kind']=='under' else (.7,1.5,.75))
        entry.update(fit)
        audit['weapons'][key]=entry
        print('REVIEW',key,fit,flush=True)
    # HUD artwork remains native in the game. This diagnostic has deliberately
    # labelled reading markers, never a claim to render the unavailable SWF.
    scene=bpy.data.scenes.new('HUD / gabarit local')
    bpy.context.window.scene=scene
    review.mesh_object('Praetor libre',free,free_triangles,free_uvs,material,scene.collection)
    forearm=free[(free[:,0]>-29)&(free[:,0]<-4)]
    height=float(forearm[:,2].max())+3.5
    font=bpy.data.fonts.load(r'C:\Windows\Fonts\bahnschrift.ttf')
    reading.panel(scene,'upright',(-26,0,height),'VIE','125','ARMURE 18',(.08,.65,1),font)
    reading.panel(scene,'upright',(2,0,height),'MUNITIONS','20 / 60','POMPE',(1,.55,.12),font)
    for obj in scene.objects:
        if obj.type=='EMPTY' and obj.name.startswith('HUD diagnostic'):
            obj.scale=(25/12,25/(512/300)/7,1)
    review.lighting(scene,(-7,0,3),90)
    pumps.render(scene,'hud-v4',(-7,0,3),90,'HUD / CIBLE DE LECTURE',
        'Gabarit local / marge 3,5 cm / textes de controle : SWF natif conserve',(0,-.75,1.65))
    audit['hud']=dict(status='LOCAL_LAYOUT_TARGET_NOT_NATIVE_GUI_REPLAY',clearance_cm=3.5,
        native_canvas_width_cm=25,native_canvas_aspect=512/300,
        unresolved=['Live native GUI pixel axes not present in F4.',
            'Whole-arm bounding-box clearance can overestimate oblique surfaces.',
            'Legacy attached glove and raw controller have different wrist origins.'])
    (OUT/'propositions-v4.json').write_text(json.dumps(audit,indent=2),encoding='utf-8')
    bpy.ops.file.pack_all()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'propositions-v4.blend'))


def refine_chainsaw():
    audit=json.loads((OUT/'propositions-v4.json').read_text())
    bpy.ops.wm.open_mainfile(filepath=str(OUT/'propositions-v4.blend'))
    bpy.context.preferences.filepaths.save_version=0
    pumps.use_gpu()
    scene=bpy.data.scenes[review.SPECS['chainsaw']['title']]
    bpy.context.window.scene=scene
    hand=next(o for o in scene.objects if o.type=='MESH' and o.name.startswith('Soutien'))
    vertices,faces=[],[]
    for obj in scene.objects:
        if obj.type!='MESH' or obj==hand:
            continue
        base=len(vertices)
        vertices.extend(tuple(obj.matrix_world@v.co) for v in obj.data.vertices)
        faces.extend(tuple(base+i for i in p.vertices) for p in obj.data.polygons)
    tree=BVHTree.FromPolygons(vertices,faces,all_triangles=True)
    raw=np.array([tuple(v.co) for v in hand.data.vertices])
    triangles=np.array([tuple(p.vertices) for p in hand.data.polygons])
    initial=np.array(hand.matrix_world)
    points=review.transformed(raw,initial)
    shifts=[np.array((x,y,z))*.5 for x in range(-6,7) for y in range(-6,7) for z in range(-6,7)]
    shifts.sort(key=lambda v:float(v@v))
    best=None
    for shift in shifts:
        crossing=len(tree.overlap(pumps.bvh(points+shift,triangles)))
        if best is None or crossing<best[0]:
            best=(crossing,shift)
        if not crossing:
            break
    crossing,shift=best
    matrix=initial.copy()
    matrix[:3,3]+=shift
    hand.matrix_world=Matrix(matrix.tolist())
    entry=audit['weapons']['chainsaw']
    if 'palm_distance_cm' in entry:
        entry['initial_palm_distance_cm']=entry.pop('palm_distance_cm')
    entry.update(crossing_triangle_pairs=crossing,hand_matrix_cm=matrix.tolist(),
        refinement_translation_cm=shift.tolist(),status='VISUAL_REVIEW_REQUIRED')
    print('REFINED CHAINSAW',crossing,shift.tolist(),flush=True)
    cm_per_unit=audit['weapon_cm_per_source_unit']
    for key in ('shotgun','super-shotgun','gauss','chainsaw'):
        item=audit['weapons'][key]
        sub=bpy.data.scenes[review.SPECS[key]['title']]
        glove=next(o for o in sub.objects if o.type=='MESH' and o.name.startswith('Soutien'))
        placed=review.transformed(np.array([tuple(v.co) for v in glove.data.vertices]),np.array(glove.matrix_world))
        baked=placed/cm_per_unit
        anchor=np.array(item['contact_surface_cm'])/cm_per_unit
        physical=cm_per_unit/100
        max_error=0.
        for frame in audit['observations']['frames']:
            axes=np.array(frame['PROP']['axes']).T
            rotation=axes/np.linalg.norm(axes,axis=0)
            fixed=rotation*physical
            shift0=(axes/39.3701-fixed)@anchor
            error=float(np.linalg.norm(baked@fixed.T+shift0-(placed/100)@rotation.T,axis=1).max())
            if error>.00002:
                raise RuntimeError('Native matrix roundtrip exceeds 0.02 mm')
            max_error=max(max_error,error)
        item.update(roundtrip_max_error_m=max_error,roundtrip_native_frames=17,
            export_meters_per_asset_unit=physical)
    placed=review.transformed(raw,matrix)
    combined=np.concatenate((np.array(vertices),placed))
    center=(combined.max(0)+combined.min(0))/2
    span=float(np.ptp(combined,axis=0).max())
    pumps.render(scene,'chainsaw-v4',center,span,'TRONCONNEUSE',
        'Arme : 0,77 / gant : taille libre conservee / prise sur la poignee avant',(-.6,1.5,.55))
    pumps.render(scene,'chainsaw-contact-v4',np.array(entry['contact_surface_cm']),60,'TRONCONNEUSE',
        'Contact de la vraie geometrie / controle paume et doigts',(.7,1.5,.75))
    (OUT/'propositions-v4.json').write_text(json.dumps(audit,indent=2),encoding='utf-8')
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'propositions-v4.blend'))


if __name__=='__main__':
    if '--refine-chainsaw' in sys.argv:
        refine_chainsaw()
    else:
        main()
