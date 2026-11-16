"""Convert the explicitly validated grenade pose, without touching its sources.

Run in Blender background. This produces assets in physical Grip XR metres;
it does not claim a working native throw or install files into any runtime.
"""
import sys
sys.dont_write_bytecode = True
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

import bpy
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'build/grenade-workshop/frag'
OUT = SOURCE / 'conversion-1'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def dump(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding='utf-8')


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'KHARVOX/tools' / filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def decoded_geometry(raw):
    assert struct.unpack_from('<III', raw) == (0x46546c67, 2, len(raw))
    length = struct.unpack_from('<I', raw, 12)[0]
    document = json.loads(raw[20:20+length])
    binary = raw[28+length:]
    a = document['accessors'][0]
    view = document['bufferViews'][a['bufferView']]
    points = np.frombuffer(binary, dtype='<f4', count=a['count']*3,
        offset=view.get('byteOffset', 0)).reshape(-1, 3)
    assert len(document['images']) == 3 and all(i['mimeType'] == 'image/png' for i in document['images'])
    assert len(document['nodes']) == 1 and 'matrix' not in document['nodes'][0] and 'scale' not in document['nodes'][0]
    return points, document


def main():
    if OUT.exists():
        raise ValueError('Sortie existante : préserver cette révision et choisir une nouvelle destination.')
    pose_path, scene_path = SOURCE/'pose.json', SOURCE/'atelier.blend'
    pose = json.loads(pose_path.read_text(encoding='utf-8'))
    before = {str(p): sha(p) for p in (pose_path, scene_path, SOURCE/'reference-native.json')}
    before.update(pose['input_hashes'])
    for p, digest in before.items():
        if sha(p) != digest:
            raise ValueError('Source modifiée : ' + p)
    OUT.mkdir()
    destinations = [OUT/n for n in ('source-manifest.json', 'fichiers-prevus.json',
        'conversion.json', 'conversion.md', 'grenade_pose.json',
        'assets/GRENADE_FRAG.glb', 'assets/PRAETOR_GRENADE_LEFT.glb')]
    dump(OUT/'fichiers-prevus.json', dict(destinations=[str(p) for p in destinations]))
    dump(OUT/'source-manifest.json', before)
    workshop = module('grenade_export_authoring', 'grenade_workshop_blender.py')
    bpy.ops.wm.open_mainfile(filepath=str(scene_path), use_scripts=False)
    actual = workshop.payload()
    comparisons = {}
    for key in ('root_matrix_cm', 'rig_matrix_local', 'mesh_matrix_local'):
        comparisons[key] = float(np.abs(np.array(pose['hand'][key])-actual['hand'][key]).max())
    if set(pose['hand']['bones']) != set(actual['hand']['bones']):
        raise ValueError('Scene/JSON : squelette différent')
    for name, matrix in pose['hand']['bones'].items():
        comparisons['bone:'+name] = float(np.abs(np.array(matrix)-actual['hand']['bones'][name]).max())
    comparisons['grenade_anchor_local'] = float(np.abs(np.array(pose['grenade']['matrix_anchor_local_cm'])-actual['grenade']['matrix_anchor_local_cm']).max())
    for name, matrix in pose['grenade']['mesh_local_matrices'].items():
        comparisons['mesh:'+name] = float(np.abs(np.array(matrix)-actual['grenade']['mesh_local_matrices'][name]).max())
    if max(comparisons.values()) > 2e-4:
        raise ValueError('La scène et le JSON ne correspondent pas ; ne pas choisir arbitrairement une pose.')
    if not np.allclose(pose['anchor']['bone_to_anchor_bind'],actual['anchor']['bone_to_anchor_bind'],atol=2e-4):
        raise ValueError('Ancrage grenade différent')
    (OUT/'assets').mkdir()
    scale = np.diag((.01, .01, .01, 1.))
    parts = [('PRAETOR_GRENADE_LEFT', bpy.data.objects['PRAETOR_left'], ROOT/'build/praetor-hands'),
        ('GRENADE_FRAG', next(o for o in bpy.data.objects[workshop.CONTROL].children if o.type=='MESH'), SOURCE/'textures')]
    if sum(o.type=='MESH' for o in bpy.data.objects[workshop.CONTROL].children) != 1:
        raise ValueError('Cet export est limité au seul mesh fragmentaire validé.')
    report = dict(stage='CONVERSION_ONLY_NATIVE_THROW_NOT_READY', equipment='frag_grenade',
        source_pose_sha256=before[str(pose_path)], source_scene_sha256=before[str(scene_path)],
        human_blender_validation_received=True, automatic_fitting=False, source_pose_changed=False,
        coordinate_space='physical_left_controller_grip_xr_metres', centimetres_to_metres=.01,
        native_first_person_transform_applied=False, arbitrary_offset_added=False,
        matrix_layout='row_major_column_vectors', maximum_scene_json_component_error=max(comparisons.values()),
        bones_checked=len(pose['hand']['bones']), assets={}, game_validated=False,
        artist_intersections=workshop.audit(), runtime_adapter_implemented=False,
        native_launch_origin_speed_verified=False, immediate_release_verified=False,
        native_inventory_and_cooldown_verified=False)
    for name, obj, textures in parts:
        cm, _, _ = workshop.builder.bake(obj, np.eye(4))
        points, normals, mapping = workshop.builder.bake(obj, scale)
        _, _, uv, indices = mapping
        images = [(textures/n).read_bytes() for n in ('BaseColor.png', 'Normal.png', 'MetallicRoughness.png')]
        raw = workshop.builder.glb(points, normals, uv, indices, name, images,
            dict(source_pose_sha256=report['source_pose_sha256'], coordinate_space=report['coordinate_space'],
                metres_per_asset_unit=1., no_legacy_hand_calibration_to_reapply=True))
        decoded, _ = decoded_geometry(raw)
        error = float(np.linalg.norm(decoded*100-cm, axis=1).max())
        if error > 2e-4:
            raise ValueError('Perte de géométrie pendant la conversion : ' + name)
        replay_error = 0.
        for angle in (0., .6, 1.8, 3.14):
            c, s = np.cos(angle), np.sin(angle)
            rotation = np.array(((c,0,s),(0,1,0),(-s,0,c)))
            for origin in (np.zeros(3), np.array((1.2,-.4,2.))):
                expected = (cm*.01)@rotation.T+origin
                drawn = decoded@rotation.T+origin
                replay_error = max(replay_error,float(np.linalg.norm(drawn-expected,axis=1).max()))
        if replay_error > 2e-6:
            raise ValueError('Transformation contrôleur incorrecte : ' + name)
        path = OUT/'assets'/(name+'.glb')
        path.write_bytes(raw)
        report['assets'][name] = dict(path=str(path),sha256=sha(path),vertices=len(points),triangles=len(indices)//3,
            dimensions_cm=((points.max(0)-points.min(0))*100).tolist(),conversion_error_cm=error,
            controller_replay_error_m=replay_error,three_embedded_textures=True)
    control_world = np.array(bpy.data.objects[workshop.CONTROL].matrix_world)
    grip_transform = scale@control_world
    grip_transform[:3,:3]=control_world[:3,:3]
    dump(OUT/'grenade_pose.json', dict(format='KHARVOX_GRENADE_CONVERTED',version=1,
        source_pose_sha256=report['source_pose_sha256'],equipment='frag_grenade',physical_hand='left',
        units='metres',coordinate_space=report['coordinate_space'],
        held_hand='assets/PRAETOR_GRENADE_LEFT.glb',held_grenade='assets/GRENADE_FRAG.glb',
        grenade_model_origin_grip_m=grip_transform[:3,3].tolist(),
        grenade_frame_grip_m=grip_transform.tolist(),game_runtime_loadable=False,
        reapply_hand_calibration=False,native_release_implemented=False))
    for p, digest in before.items():
        if sha(p) != digest:
            raise ValueError('Une source a changé durant la conversion : ' + p)
    report['protected_sources_verified']=len(before)
    dump(OUT/'conversion.json',report)
    (OUT/'conversion.md').write_text(
        '# Conversion grenade validée dans Blender\n\n'
        'Scène et JSON originaux préservés ; '+str(report['bones_checked'])+' articulations comparées.\n'
        'Sommets exprimés directement en mètres dans le repère Grip gauche XR.\n'
        'Ne pas réappliquer la calibration du GLB de main libre. Aucun ajustement des contacts.\n\n'
        'Les deux GLB incluent les trois textures de leur matériau et ont été vérifiés après décodage.\n'
        'Les contacts validés artistiquement sont conservés, même si le contrôle de triangles signale des croisements.\n\n'
        '**Ce dossier ne constitue pas un paquet jouable.** Le renderer actuel ne charge pas encore ces assets.\n'
        'La publication du type/charges/cooldown, le raccord natif de départ/vitesse et le déclenchement immédiat restent à établir.\n'
        'Aucun lancer fonctionnel ni absence du délai de deux secondes ne sont revendiqués.\n',encoding='utf-8')
    print('GRENADE_CONVERSION '+json.dumps(report,ensure_ascii=True),flush=True)


if __name__=='__main__':main()
