"""Export only the two visually accepted pumps; no executable or package.

Blender centimetres are converted to the nominal weapon frame once. v3
profiles keep that authored glove scale physically constant at runtime while
the contact anchor continues to use the actual native weapon transform.
"""
import sys
sys.dont_write_bytecode = True
import hashlib
import importlib.util
import itertools
import json
import pathlib
import shutil
import struct

import bpy
import numpy as np
from mathutils.bvhtree import BVHTree

ROOT = pathlib.Path(__file__).resolve().parents[2]
REVIEW = ROOT/'build/pump-review-v3'
OUT = REVIEW/'runtime-fixture'
SOURCE = ROOT/'references/test-fixtures/reviewed-support'


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT/'KHARVOX/tools'/filename)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    exporter = module('existing_support_export', 'export_reviewed_support_hands.py')
    builder_module = module('existing_praetor_builder', 'build_praetor_hands.py')
    builder = vars(builder_module)
    hud = module('production_hand_geometry', 'preview_praetor_hud_clearance.py')
    audit = json.loads((REVIEW/'propositions-v3.json').read_text())
    cm_per_unit = audit['weapon_cm_per_source_unit']
    if audit['status'] != 'VISUAL_PROPOSALS_ONLY_NO_RUNTIME_BUILD':
        raise RuntimeError('Unexpected review identity')
    provenance = json.loads((ROOT/'build/praetor-hands/provenance.json').read_text())
    bpy.ops.wm.open_mainfile(filepath=str(ROOT/'build/praetor-imported.blend'))
    native_hand = next(o for o in bpy.context.scene.objects if o.type=='MESH' and o.get('kharvox_native_hands'))
    rig = next(m.object for m in native_hand.modifiers if m.type=='ARMATURE')
    fist_from_anatomy=[]
    for index,side in enumerate(('left','right')):
        wrist,basis,_ = builder_module.landmarks(rig,side)
        if side=='right':
            # The mirrored support anatomy is left-chiral. Match it to the
            # right native mesh using its anatomical dorsal, not palmar, axis.
            basis[:,2] *= -1
        native = np.eye(4)
        native[:3,:3],native[:3,3]=basis,wrist
        fist_from_anatomy.append(np.array(provenance['hands'][index]['alignment']['FIST'])@native)
    bpy.ops.wm.open_mainfile(filepath=str(REVIEW/'propositions-v3.blend'))
    (OUT/'assets/3D').mkdir(parents=True, exist_ok=True)
    config = hud.key_values(SOURCE/'hand_models.cfg')
    for name in ('hand_models_calibration_default.cfg','hand_models_calibration_saved.cfg'):
        if (SOURCE/name).exists():
            config.update(hud.key_values(SOURCE/name))
    references = [float(config[side+'_scale']) for side in ('left','right')]
    fist_hashes = [exporter.fnv64((SOURCE/config[side+'_fist']).read_bytes()) for side in ('left','right')]
    native_scales = [np.linalg.norm(np.array(hand['alignment']['FIST'])[:3,0])*references[i]
                     for i,hand in enumerate(provenance['hands'])]
    if not np.isclose(native_scales[0],audit['hand_source']['meters_per_native_unit'],atol=1e-10):
        raise RuntimeError('Free-hand reference calibration differs from the approved review')
    entries, rows = {}, []
    for key, weapon, title in (('shotgun','shotgun','FUSIL A POMPE'),
                              ('super-shotgun','super_shotgun','SUPER SHOTGUN')):
        entry = audit['weapons'][key]
        if entry['status'] != 'CONTACT_PROPOSAL' or entry['crossing_triangle_pairs']:
            raise RuntimeError('Unaccepted geometry state')
        if digest(pathlib.Path(entry['source'])) != entry['source_sha256']:
            raise RuntimeError('Weapon Cast source changed after review')
        scene = bpy.data.scenes[title]
        bpy.context.window.scene = scene
        hand = next(o for o in scene.objects if o.type=='MESH' and o.name.startswith('Main de soutien'))
        if not np.allclose(np.array(hand.matrix_world),entry['hand_matrix_cm'],atol=1e-5):
            raise RuntimeError('Scene and reviewed hand matrix disagree')
        vertices_cm, normals, mapping = builder_module.bake(hand,np.eye(4))
        _,_,uvs,indices = mapping
        faces = indices.reshape(-1,3)
        tree = exporter.weapon_bvh(scene,hand,builder)
        anchor = np.array(entry['contact_surface_cm'])/cm_per_unit
        exported = []
        for side in ('left','right'):
            asset_from_anatomy = np.array(hand.matrix_world)
            asset_from_anatomy[:3,:] /= cm_per_unit
            points = vertices_cm/cm_per_unit
            normal,triangles = normals.copy(),faces.copy()
            grip = anchor.copy()
            if side=='right':
                # Preserve the existing physical-right mirrored-pose contract;
                # never reflect the weapon. Match the right FIST reference.
                points = anchor+(points-anchor)*(native_scales[1]/native_scales[0])
                points[:,1] *= -1
                normal[:,1] *= -1
                triangles = triangles[:,(0,2,1)].copy()
                grip[1] *= -1
                ratio=native_scales[1]/native_scales[0]
                asset_from_anatomy[:3,:3] *= ratio
                asset_from_anatomy[:3,3]=anchor+(asset_from_anatomy[:3,3]-anchor)*ratio
                asset_from_anatomy=np.diag((1,-1,1,1))@asset_from_anatomy
            fist_to_asset=asset_from_anatomy@np.linalg.inv(fist_from_anatomy[side=='right'])
            crossings = len(tree.overlap(BVHTree.FromPolygons((points*cm_per_unit).tolist(),
                triangles.tolist(),all_triangles=True,epsilon=.002)))
            if crossings:
                raise RuntimeError(weapon+' '+side+' intersects selected weapon meshes: '+str(crossings))
            relative = 'assets/3D/SUPPORT_'+weapon.upper()+'_'+side.upper()+'.glb'
            raw = exporter.geometry_glb(builder,points,normal,uvs,triangles.reshape(-1),weapon+'_'+side,
                dict(source='visually accepted two-pump Blender V3',
                     review_blend_sha256=digest(REVIEW/'propositions-v3.blend'),
                     coordinate_space='nominal weapon units; v3 fixed physical glove scale',
                     no_runtime_skinning=True,no_runtime_fitting=True))
            validation = exporter.validate_glb(raw,points,triangles)
            if side=='left' and not np.allclose(points*cm_per_unit,vertices_cm,atol=2e-5):
                raise RuntimeError('Runtime left glove no longer matches approved Blender vertices')
            (OUT/relative).write_bytes(raw)
            exported.append(dict(path=relative,hash=exporter.fnv64(raw),anchor=grip.tolist(),
                crossing_triangle_pairs=crossings,sha256=hashlib.sha256(raw).hexdigest(),
                fist_to_asset=fist_to_asset.tolist(),**validation))
            print(weapon,side,'crossings',crossings,'round_trip_OK',flush=True)
        row = ['4',weapon,exported[0]['path'],exported[1]['path'],str(exported[0]['hash']),str(exported[1]['hash'])]
        row += [format(v,'.12g') for v in exported[0]['anchor']+exported[1]['anchor']]
        row += [format(cm_per_unit*.01,'.12g'),*[format(v,'.12g') for v in references],*[str(v) for v in fist_hashes]]
        for result in exported:
            row += [format(v,'.12g') for v in np.array(result['fist_to_asset']).T.flatten()]
        rows.append(' '.join(row))
        entries[weapon]=exported
    # Bound every production finger-morph combination in raw controller space.
    # The renderer uses independent linear openness weights in [0,1], so the
    # eight corners bound every possible intermediate finger state as well.
    bounds=[]
    for side in ('left','right'):
        points,_,_,source = hud.load_runtime_hand(SOURCE,side,False)
        morph=(SOURCE/'assets/3D'/('DOOM_'+side.upper()+'_HAND_FINGERS.bin')).read_bytes()
        if len(morph) != 216+len(points)*18*4:
            raise RuntimeError('Unexpected production finger payload')
        deltas=np.frombuffer(morph,dtype='<f4',offset=216).reshape(3,len(points),6)[:,:,:3]
        calibration=np.array(source['raw_grip_to_baked_mesh_matrix_meters'])
        deltas_cm=deltas@calibration[:3,:3].T*100
        samples=np.concatenate([points+np.einsum('s,svi->vi',weights,deltas_cm)
            for weights in itertools.product((0.,1.),repeat=3)])@hud.DOOM_TO_XR
        bounds.append((samples.min(0).tolist(),samples.max(0).tolist()))
    native_hud=(SOURCE/'offhand_hud_original.cfg').read_text().splitlines()
    native_hud=[line.strip() for line in native_hud if line.strip() and not line.lstrip().startswith('#')]
    if native_hud[0] != '3 1' or len(native_hud)!=7:
        raise RuntimeError('Unexpected original six-mode native HUD calibration')
    hud_text='4 1 1 3.5\n'+'\n'.join(native_hud[1:])+'\n'
    hud_text+='\n'.join(' '.join(format(v,'.12g') for v in low+high) for low,high in bounds)+'\n'
    for name in ('offhand_hud.cfg','offhand_hud_default.cfg'):
        (OUT/name).write_text(hud_text,encoding='utf-8')
    (OUT/'support_hands.cfg').write_text('\n'.join(rows)+'\n',encoding='utf-8')
    copies=['hand_models.cfg','hand_models_calibration_default.cfg','HandPbr.vert.spv','HandPbr.frag.spv']
    if (SOURCE/'hand_models_calibration_saved.cfg').exists():
        copies.append('hand_models_calibration_saved.cfg')
    copies += ['assets/3D/DOOM_'+side+'_'+kind+extension for side in ('LEFT','RIGHT')
               for kind,extension in (('HAND_FIST','.glb'),('HAND_GUN','.glb'),('HAND_FINGERS','.bin'))]
    for relative in copies:
        shutil.copyfile(SOURCE/relative,OUT/relative)
    (OUT/'provenance.json').write_text(json.dumps(dict(
        status='TEST_FIXTURE_ONLY_NOT_A_PLAYABLE_PACKAGE',review_user_accepted=True,
        review_blend_sha256=digest(REVIEW/'propositions-v3.blend'),
        review_json_sha256=digest(REVIEW/'propositions-v3.json'),
        actual_weapon_basis='not measured in game; live transform verification outstanding',
        meters_per_asset_unit=cm_per_unit*.01,reference_hand_scales=references,
        fist_hashes=fist_hashes,weapons=entries,raw_grip_doom_bounds_cm=bounds,
        hud_artwork='unchanged native SWF; original six-mode tangent placement and scales',
        hud_rotation='180 degrees in the actual pixel plane; no mirror',
        hud_clearance='minimum 3.5cm from production free-hand or locked mesh envelope; native render timing still needs game verification',
        copied_assets={name:digest(OUT/name) for name in copies}),indent=2),encoding='utf-8')
    print('EXPORTED 2 PUMPS / 4 STATIC GLBS / HUD V4 / NO EXECUTABLE',flush=True)


if __name__=='__main__':
    main()
