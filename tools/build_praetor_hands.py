"""Blender --background --python this_file -- --source imported.blend --output assets.

Bake locally exported native Praetor hands to the existing KHARVOX GLB/morph
contract. Original repo assets remain read-only wrist calibration references.
The exporter must be run after praetor_materials.py; no game archive is modified.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import pathlib
import struct
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Quaternion, Vector

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('original_hand_rigs', ROOT/'tools/build_hand_finger_morphs.py')
donor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(donor)
FINGERS = ('index','middle','ring','pinky','thumb')
GROUPS = (('index',),('middle','ring','pinky'),('thumb',))


def landmarks(rig, side, original=False, fit=None):
    prefix = 'DEF-hand.'+side if original else side+'hand'
    wrist = rig.matrix_world @ rig.pose.bones[prefix].matrix
    heads=[]
    for finger in FINGERS[:4]:
        name = 'DEF-f_'+finger+'.01.'+side if original else side+'hand'+finger+'1'
        heads.append(np.array(rig.matrix_world @ rig.pose.bones[name].head))
    points = np.array([np.array(wrist.translation),*heads])
    if fit is not None:
        points = points @ fit[:3,:3].T + fit[:3,3]
    forward = points[1:].mean(axis=0)-points[0]
    forward /= np.linalg.norm(forward)
    radial = points[1]-points[4]
    radial -= forward*np.dot(radial,forward)
    width = np.linalg.norm(radial)
    radial /= width
    basis = np.column_stack((radial,forward,np.cross(radial,forward)))
    return points[0],basis,width


def reference_frames():
    result={}
    for physical in ('LEFT','RIGHT'):
        bpy.ops.wm.open_mainfile(filepath=str(ROOT/'assets/3D'/('HandLeft.blend' if physical=='LEFT' else 'HandRight.blend')))
        for kind,number in (('FIST',1),('GUN',2)):
            scene=bpy.data.scenes[('L' if physical=='LEFT' else 'R')+'_Pose_'+str(number)]
            bpy.context.window.scene=scene
            obj=next(o for o in scene.objects if o.type=='MESH' and len(o.data.vertices)>1000)
            rig=donor.rig_for(obj);side=donor.hand_side(obj)
            _,positions,uvs=donor.glb(ROOT/'assets/3D'/('DOOM_'+physical+'_HAND_'+kind+'.glb'))
            fit,_,_,_=donor.correspond(obj,positions,uvs)
            result[(physical,kind)]=landmarks(rig,side,True,fit)
    return result


def alignment(native,reference):
    origin,basis,width=native;target,target_basis,target_width=reference
    rotation=target_basis@basis.T*(target_width/width)
    result=np.eye(4);result[:3,:3]=rotation;result[:3,3]=target-rotation@origin
    if np.linalg.det(rotation)<=0:raise RuntimeError('Unexpected hand reflection')
    return result


def isolate(obj, rig, side):
    mesh=obj.data;bm=bmesh.new();bm.from_mesh(mesh)
    bm.verts.ensure_lookup_table()
    remove=[]
    for vertex in mesh.vertices:
        own=other=0.
        for item in vertex.groups:
            name=obj.vertex_groups[item.group].name.lower()
            if name.startswith(side):own+=item.weight
            elif name.startswith('right' if side=='left' else 'left'):other+=item.weight
        if own<=other:remove.append(bm.verts[vertex.index])
    bmesh.ops.delete(bm,geom=remove,context='VERTS')
    wrist=rig.pose.bones[side+'hand'].head.copy()
    elbow=rig.pose.bones[side+'forearm'].head.copy()
    axis=(wrist-elbow).normalized()
    # Keep the native hand and lower forearm, without the shoulder/upper arm.
    cut=elbow+(wrist-elbow)*.12
    bmesh.ops.bisect_plane(bm,geom=list(bm.verts)+list(bm.edges)+list(bm.faces),
        plane_co=cut,plane_no=axis,dist=.0001,clear_inner=True,clear_outer=False)
    boundary=[e for e in bm.edges if e.is_boundary and all(abs((v.co-cut).dot(axis))<.001 for v in e.verts)]
    if boundary:
        cap=bmesh.ops.holes_fill(bm,edges=boundary,sides=0)
        for face in cap['faces']:face.smooth=False
    bm.normal_update();bm.to_mesh(mesh);bm.free();mesh.update()
    if len(mesh.vertices)<1000:raise RuntimeError('Hand isolation removed too much geometry')
    return float((wrist-cut).length)


def pose(rig,side,states):
    for finger in FINGERS:
        first=rig.data.bones[side+'hand'+finger+'1'].matrix_local
        second=rig.data.bones[side+'hand'+finger+'2'].matrix_local
        curl=first.col[1].xyz.cross(second.col[1].xyz).dot(first.col[0].xyz)
        sign=1 if curl>=0 else -1
        state=states[finger]
        if finger=='thumb':
            degrees={'free':(0,0,2),'gun':(10,20,10),'closed':(20,35,25)}[state]
        else:
            degrees={'free':(0,9,5),'gun':(20,35,15),'closed':(35,55,35)}[state]
        for segment,angle in enumerate(degrees,1):
            bone=rig.pose.bones[side+'hand'+finger+str(segment)]
            bone.rotation_mode='QUATERNION'
            bone.rotation_quaternion=Quaternion((1,0,0),sign*math.radians(angle))
    bpy.context.view_layer.update()


def grasp(rig,side,transform):
    roots=[];tips=[]
    for finger in FINGERS[:4]:
        first=rig.pose.bones[side+'hand'+finger+'1']
        second=rig.pose.bones[side+'hand'+finger+'2']
        last=rig.pose.bones[side+'hand'+finger+'3']
        # Cast's Blender display-bone tails are only .0025 long; use the actual
        # phalanx spacing, not those display tails, to estimate the fingertip.
        length=(last.head-second.head).length*.75
        roots.append(np.array(rig.matrix_world@first.head))
        tips.append(np.array(rig.matrix_world@(last.head+last.matrix.col[1].xyz.normalized()*length)))
    point=(np.mean(roots,axis=0)+np.mean(tips,axis=0))*.5
    return transform[:3,:3]@point+transform[:3,3]


def evaluated(obj):
    item=obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh=item.to_mesh();mesh.calc_loop_triangles()
    return item,mesh


def corner_mapping(mesh):
    lookup={};vertices=[];loops=[];uvs=[];triangles=[]
    for triangle in mesh.loop_triangles:
        face=[]
        for index in triangle.loops:
            vertex=mesh.loops[index].vertex_index
            uv=mesh.uv_layers.active.data[index].uv
            normal=mesh.corner_normals[index].vector
            key=(vertex,round(uv.x,6),round(uv.y,6),*(round(v,5) for v in normal))
            if key not in lookup:
                lookup[key]=len(vertices);vertices.append(vertex);loops.append(index);uvs.append((uv.x,1-uv.y))
            face.append(lookup[key])
        triangles.extend(face)
    return np.array(vertices),np.array(loops),np.array(uvs,dtype='<f4'),np.array(triangles,dtype='<u4')


def bake(obj,transform,mapping=None):
    item,mesh=evaluated(obj)
    try:
        if mapping is None:mapping=corner_mapping(mesh)
        vertices,loops,_,_=mapping
        positions=np.array([v.co[:] for v in mesh.vertices])[vertices]
        normals=np.array([n.vector[:] for n in mesh.corner_normals])[loops]
        world=np.array(item.matrix_world)
        matrix=transform@world
        positions=positions@matrix[:3,:3].T+matrix[:3,3]
        normals=normals@np.linalg.inv(matrix[:3,:3])
        normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-12)
        if not np.isfinite(positions).all() or not np.isfinite(normals).all():raise RuntimeError('Non-finite mesh data')
        return positions,normals,mapping
    finally:item.to_mesh_clear()


def glb(positions,normals,uvs,indices,scene_name,textures,provenance):
    binary=bytearray();views=[];accessors=[]
    def view(raw,target=None):
        while len(binary)%4:binary.append(0)
        item={'buffer':0,'byteOffset':len(binary),'byteLength':len(raw)}
        if target:item['target']=target
        binary.extend(raw);views.append(item);return len(views)-1
    for values,width,kind,component in ((positions,3,'VEC3',5126),(normals,3,'VEC3',5126),(uvs,2,'VEC2',5126),(indices,1,'SCALAR',5125)):
        values=np.asarray(values,dtype='<u4' if component==5125 else '<f4')
        item={'bufferView':view(values.tobytes(),34963 if width==1 else 34962),'componentType':component,'count':values.size//width,'type':kind}
        if len(accessors)==0:item.update(min=values.min(axis=0).tolist(),max=values.max(axis=0).tolist())
        accessors.append(item)
    images=[{'bufferView':view(data),'mimeType':'image/png'} for data in textures]
    document={'asset':{'version':'2.0','generator':'KHARVOX native Praetor adapter'},'scene':0,
        'scenes':[{'name':scene_name,'nodes':[0]}],
        'nodes':[{'mesh':0,'rotation':[0,0,0,1],'name':scene_name}],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1,'TEXCOORD_0':2},'indices':3,'material':0,'mode':4}]}],
        'materials':[{'name':'Native Praetor hands','doubleSided':True,'alphaMode':'OPAQUE',
            'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicRoughnessTexture':{'index':2},'metallicFactor':1,'roughnessFactor':1},
            'normalTexture':{'index':1}}],
        'textures':[{'source':i,'sampler':0} for i in range(3)],
        'samplers':[{'magFilter':9729,'minFilter':9729,'wrapS':10497,'wrapT':10497}],
        'images':images,'buffers':[{'byteLength':len(binary)}],'bufferViews':views,'accessors':accessors,'extras':provenance}
    encoded=json.dumps(document,separators=(',',':')).encode()
    encoded+=b' '*((-len(encoded))%4);binary+=b'\0'*((-len(binary))%4)
    return struct.pack('<III',0x46546c67,2,28+len(encoded)+len(binary))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(binary),0x004e4942)+binary


def build_side(source,output,physical,references,textures,origin):
    bpy.ops.wm.open_mainfile(filepath=str(source))
    obj=next(o for o in bpy.context.scene.objects if o.type=='MESH' and o.get('kharvox_native_hands'))
    rig=next(m.object for m in obj.modifiers if m.type=='ARMATURE')
    side=physical.lower()
    native=landmarks(rig,side)
    transforms={kind:alignment(native,references[(physical,kind)]) for kind in ('FIST','GUN')}
    forearm_length=isolate(obj,rig,side)
    states={finger:'closed' for finger in FINGERS};pose(rig,side,states)
    closed,normals,mapping=bake(obj,transforms['FIST'])
    _,_,uvs,indices=mapping
    fist_centre=grasp(rig,side,transforms['FIST'])
    fist_wrist=transforms['FIST']@np.array(rig.matrix_world@rig.pose.bones[side+'hand'].matrix)
    morphs=[];grasp_deltas=[]
    for group in GROUPS:
        pose(rig,side,{finger:'free' if finger in group else 'closed' for finger in FINGERS})
        opened,opened_normals,_=bake(obj,transforms['FIST'],mapping)
        morphs.append(np.concatenate((opened-closed,opened_normals-normals),axis=1))
        grasp_deltas.append(grasp(rig,side,transforms['FIST'])-fist_centre)
    provenance={'source':'DOOM 2016 local Praetor Cast export','source_cast_sha256':origin['source_cast_sha256'],
        'physical_hand':side,'native_skin':True,'forearm':'rigid lower forearm attached to tracked wrist',
        'material_approximation':'native colour/normal/smoothness; existing dielectric hand shader, no native specular RGB or scene lighting'}
    prefix='DOOM_'+physical+'_HAND_'
    fist=glb(closed,normals,uvs,indices,('L' if side=='left' else 'R')+'_Pose_1',textures,provenance)
    (output/(prefix+'FIST.glb')).write_bytes(fist)
    pose(rig,side,{finger:'gun' for finger in FINGERS})
    gun_p,gun_n,_=bake(obj,transforms['GUN'],mapping)
    gun=glb(gun_p,gun_n,uvs,indices,('L' if side=='left' else 'R')+'_Pose_2',textures,provenance)
    (output/(prefix+'GUN.glb')).write_bytes(gun)
    gun_wrist=transforms['GUN']@np.array(rig.matrix_world@rig.pose.bones[side+'hand'].matrix)
    gun_centre=grasp(rig,side,transforms['GUN'])
    header=struct.pack('<8sIQQ32f15f',b'KHFING02',len(closed),donor.fnv(fist),donor.fnv(gun),
        *fist_wrist.T.flatten(),*gun_wrist.T.flatten(),*gun_centre,*fist_centre,*np.array(grasp_deltas).flatten())
    morphs=np.array(morphs,dtype='<f4')
    (output/(prefix+'FINGERS.bin')).write_bytes(header+morphs.tobytes())
    print(physical,'vertices',len(closed),'triangles',len(indices)//3,'forearm_source_units',forearm_length,
        'morph_max',[float(np.linalg.norm(delta[:,:3],axis=1).max()) for delta in morphs],flush=True)
    return {'physical_hand':side,'vertices':len(closed),'triangles':len(indices)//3,
            'forearm_source_units':forearm_length,'alignment':{kind:value.tolist() for kind,value in transforms.items()},
            'fist_sha256':hashlib.sha256(fist).hexdigest(),'gun_sha256':hashlib.sha256(gun).hexdigest()}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=pathlib.Path,required=True)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    parser.add_argument('--audit',type=pathlib.Path,required=True)
    args=parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    origin=json.loads(args.audit.read_text(encoding='utf-8'))
    source=args.source.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    if output==ROOT/'assets/3D':raise RuntimeError('Use a separate staging directory')
    textures=[(output/name).read_bytes() for name in ('BaseColor.png','Normal.png','MetallicRoughness.png')]
    references=reference_frames()
    provenance={'source_cast_sha256':origin['sha256'],'source_blend':str(source),'hands':[]}
    for physical in ('LEFT','RIGHT'):
        provenance['hands'].append(build_side(source,output,physical,references,textures,provenance))
    (output/'provenance.json').write_text(json.dumps(provenance,indent=2),encoding='utf-8')


if __name__=='__main__':main()
