"""Actual left/right Praetor meshes; illustration only, no runtime export."""
import sys
sys.dont_write_bytecode=True
import pathlib
import math
import json
import numpy as np
import bpy
from mathutils import Matrix, Vector

ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'KHARVOX/tools'))
import preview_praetor_pumps_v3 as pumps
review=pumps.review
builder=pumps.reading.module('two_hand_builder','build_praetor_hands.py')
OUT=ROOT/'build/pump-review-v4'

bpy.context.preferences.filepaths.save_version=0
pumps.use_gpu()
bpy.ops.wm.open_mainfile(filepath=str(ROOT/'build/praetor-imported.blend'))
left_source=next(o for o in bpy.context.scene.objects if o.type=='MESH' and o.get('kharvox_native_hands'))
left_rig=next(m.object for m in left_source.modifiers if m.type=='ARMATURE')
left_wrist,left_basis,_=builder.landmarks(left_rig,'left')
builder.isolate(left_source,left_rig,'left')
builder.pose(left_rig,'left',{f:'gun' for f in builder.FINGERS})
left_points,_,left_mapping=builder.bake(left_source,np.eye(4))
_,_,left_uvs,left_indices=left_mapping
left_uvs=left_uvs.copy();left_uvs[:,1]=1-left_uvs[:,1]
left_local=(left_points-left_wrist)@left_basis
bpy.ops.wm.open_mainfile(filepath=str(ROOT/'build/praetor-imported.blend'))
obj=next(o for o in bpy.context.scene.objects if o.type=='MESH' and o.get('kharvox_native_hands'))
rig=next(m.object for m in obj.modifiers if m.type=='ARMATURE')
assert all('righthandthumb'+str(i) in rig.pose.bones for i in (1,2,3))
wrist,basis,_=builder.landmarks(rig,'right')
builder.isolate(obj,rig,'right')
builder.pose(rig,'right',{f:'closed' for f in builder.FINGERS})
points,_,mapping=builder.bake(obj,np.eye(4))
_,_,uvs,indices=mapping
uvs=uvs.copy();uvs[:,1]=1-uvs[:,1]
grasp=builder.grasp(rig,'right',np.eye(4))
local=(points-wrist)@basis
grasp=(grasp-wrist)@basis

bpy.ops.wm.open_mainfile(filepath=str(OUT/'propositions-v4.blend'))
scene=bpy.data.scenes[review.SPECS['shotgun']['title']]
bpy.context.window.scene=scene
left=next(o for o in scene.objects if o.type=='MESH' and o.name.startswith('Soutien'))
old_left=left
left=review.mesh_object('Vraie main GAUCHE',left_local,left_indices.reshape(-1,3),
    left_uvs,old_left.data.materials[0],scene.collection)
left.matrix_world=old_left.matrix_world.copy()
bpy.data.objects.remove(old_left,do_unlink=True)
left.name='Vraie main GAUCHE Praetor / sans miroir'
audit=json.loads((OUT/'propositions-v4.json').read_text())
physical=audit['hand_cm_per_source_unit']
weapon_scale=audit['weapon_cm_per_source_unit']
anchor=np.array(audit['weapons']['shotgun']['contact_surface_cm'])
angle=math.radians(45)
rotation=np.array(((math.cos(angle),-math.sin(angle),0),(math.sin(angle),math.cos(angle),0),(0,0,1)))
matrix=np.array(left.matrix_world)
matrix[:3,3]=anchor+rotation@(matrix[:3,3]-anchor)
matrix[:3,:3]=rotation@matrix[:3,:3]
assert np.linalg.det(matrix[:3,:3])>0
left.matrix_world=Matrix(matrix.tolist())

material=left.data.materials[0]
right=review.mesh_object('Vraie main DROITE Praetor / sans miroir',local,
    indices.reshape(-1,3),uvs,material,scene.collection)
# Native right anatomy: radial toward the top of the grip, fingers forward,
# palmar normal toward the player's left. Positive determinant: no reflection.
basis_right=np.array(((0,1,0),(0,0,1),(1,0,0)),dtype=float)
matrix=np.eye(4);matrix[:3,:3]=basis_right*physical
matrix[:3,3]=np.array((-9.,0.,-7.))*weapon_scale-matrix[:3,:3]@grasp
assert np.linalg.det(matrix[:3,:3])>0
right.matrix_world=Matrix(matrix.tolist())

review.setup(scene,False)
scene.cycles.samples=32
scene.render.resolution_x=1920;scene.render.resolution_y=1280
# Rear-left viewpoint shows the left hand's thumb side and both forearms.
review.camera(scene,(22,0,-6),145,(-1.2,1.5,.55))
scene.render.filepath=str(OUT/'shotgun-deux-mains.png')
bpy.ops.render.render(write_still=True,scene=scene.name)
bpy.ops.file.pack_all()
bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'shotgun-deux-mains.blend'))
print('LEFT/RIGHT NATIVE PRAETOR GEOMETRY; POSITIVE DETERMINANTS; NO REFLECTION',flush=True)
