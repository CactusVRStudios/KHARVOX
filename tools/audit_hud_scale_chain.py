"""Read-only Blender/production HUD dimensional audit. Never saves the scene."""
import sys
sys.dont_write_bytecode = True
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

import bpy
import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(source, scene, runtime):
    sealed = {str(p): sha(p) for p in (source, scene)}
    data = json.loads(source.read_text(encoding='utf-8-sig'))
    author = module('hud_author_audit', 'hud_left_workshop.py')
    reader = module('hud_geometry_audit', 'preview_praetor_hud_clearance.py')
    bpy.ops.wm.open_mainfile(filepath=str(scene), load_ui=False, use_scripts=False)
    current = author.payload()
    for name, item in data['transforms'].items():
        if not np.allclose(item['matrix_anchor_local_cm'], current['transforms'][name]['matrix_anchor_local_cm'], atol=2e-4):
            raise ValueError('Scene et JSON differents : ' + name)
    frame = np.array(data['anchor']['frame_in_grip_xr_cm'])
    if not np.allclose(frame, current['anchor']['frame_in_grip_xr_cm'], atol=2e-4):
        raise ValueError('Ancrage scene/JSON different')
    points, triangles, _, production = reader.load_runtime_hand(runtime, 'left', True)
    provenance = json.loads((ROOT / 'build/praetor-hands/provenance.json').read_text())
    hand = next(h for h in provenance['hands'] if h['physical_hand'] == 'left')
    expected = np.diag((100., 100., 100., 1.)) @ np.array(production['raw_grip_to_baked_mesh_matrix_meters']) @ np.array(hand['alignment']['FIST'])
    native = np.array(data['anchor']['native_to_grip_xr_cm'])
    calibration_error = float(np.max(np.abs(expected-native)))
    if calibration_error > 2e-4:
        raise ValueError('Calibration runtime differente de Blender : ' + str(calibration_error))
    deps = bpy.context.evaluated_depsgraph_get()
    obj = bpy.data.objects['PRAETOR_GAUCHE'].evaluated_get(deps)
    mesh = obj.to_mesh()
    transform = np.array(bpy.data.objects['AVANT_BRAS_GAUCHE'].matrix_world.inverted() @ obj.matrix_world)
    actual = reader.transformed(np.array([v.co[:] for v in mesh.vertices]), transform)
    obj.to_mesh_clear()
    bounds = np.array((actual.min(0), actual.max(0)))
    bounds_error = float(np.max(np.abs(bounds-np.array(production['bounds_cm']))))
    if bounds_error > .002:
        raise ValueError('Geometrie Blender/runtime differente : ' + str(bounds_error) + ' cm')
    units = 39.3701
    if (runtime/'world_scale.cfg').exists():
        units = float((runtime/'world_scale.cfg').read_text().strip())
    if not np.isfinite(units) or units <= 0:
        raise ValueError('Echelle monde invalide')
    xr_to_doom = np.eye(4)
    xr_to_doom[:3, :3] = reader.DOOM_TO_XR.T
    panels = []
    forearm_cm = hand['forearm_source_units'] * np.linalg.norm(native[:3, 0])
    root_inverse = np.array(bpy.data.objects['AVANT_BRAS_GAUCHE'].matrix_world.inverted())
    for name in ('HUD_Munitions', 'HUD_Vie_Armure', 'HUD_Options'):
        item = data['transforms'][name]
        panel = bpy.data.objects[name]
        vertices = np.array([v.co[:] for v in panel.data.vertices])
        measured = np.ptp(vertices, axis=0)[:2] * np.array(panel.scale)[:2]
        dimensions = np.array((item['width_cm'], item['height_cm']))
        if not np.allclose(measured, dimensions, atol=2e-4):
            raise ValueError('Dimensions du plan modifiees sans metadonnees : ' + name)
        local = np.array(item['matrix_anchor_local_cm'])
        xr = frame @ local
        world = root_inverse @ np.array(panel.matrix_world)
        if not np.allclose(world, xr, atol=2e-4):
            raise ValueError('Transformation parente appliquee deux fois : ' + name)
        doom = xr_to_doom @ xr
        intervals = reader.rectangle_depth_intervals(points, triangles, xr[:3, 3],
            (xr[:3, 2], xr[:3, 0], xr[:3, 1]), *dimensions)
        intersections = sum(lo <= 0 <= hi for lo, hi in intervals)
        panels.append(dict(name=name, blender_size_cm=dimensions.tolist(),
            kharvox_size_cm=dimensions.tolist(), doom_size_units=(dimensions*units*.01).tolist(),
            center_blender_grip_cm=xr[:3, 3].tolist(), center_doom_grip_units=(doom[:3, 3]*units*.01).tolist(),
            width_to_forearm_length=float(dimensions[0]/forearm_cm),
            parent_matrix_error_cm=float(np.max(np.abs(world-xr))), intersections=intersections,
            height_provisional=bool(item.get('dimensions_provisional'))))
    if any(sha(Path(p)) != digest for p, digest in sealed.items()):
        raise ValueError('Source modifiee pendant le controle')
    return dict(inputs=sealed, runtime=str(runtime), units_per_meter=units,
        centimetres_per_native_unit=np.linalg.norm(native[:3, :3], axis=0).tolist(),
        forearm_native_length=hand['forearm_source_units'], forearm_length_cm=float(forearm_cm),
        runtime_geometry_bounds_cm=production['bounds_cm'], blender_geometry_bounds_cm=bounds.tolist(),
        geometry_bounds_error_cm=bounds_error, calibration_matrix_error=calibration_error,
        panels=panels, confirmed_two_hand_bug='HUD omitted live joint and inverse bind used by rendered hand',
        free_hand_scale_fault_confirmed=False, arbitrary_scale_correction=1.,
        limitations=['Blender artwork is an approximate reconstruction, not native SWF pixels.',
            'Options height is provisional; game preserves its dynamic native aspect.',
            'Runtime native GUI dimensions and stereo occlusion require the independent test package.'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for key in ('source', 'scene', 'runtime', 'out'):
        parser.add_argument('--'+key, type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    report = audit(args.source.resolve(), args.scene.resolve(), args.runtime.resolve())
    (args.out/'comparaison.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    lines = ['# Controle HUD / Praetor', '',
        'Sources Blender et JSON lues uniquement. Aucun coefficient empirique.',
        f"Avant-bras : {report['forearm_length_cm']:.6f} cm ; monde : {report['units_per_meter']} unites/metre.",
        f"Ecart des enveloppes Blender/GLB runtime : {report['geometry_bounds_error_cm']:.9f} cm.", '',
        '| Plan | Blender cm | KHARVOX cm | DOOM unites | Rapport largeur/avant-bras | Intersections |',
        '|---|---|---|---|---|---|']
    for p in report['panels']:
        sizes = [' x '.join(f'{v:.6f}' for v in p[k]) for k in ('blender_size_cm','kharvox_size_cm','doom_size_units')]
        lines.append(f"| {p['name']} | {' | '.join(sizes)} | {p['width_to_forearm_length']:.6f} | {p['intersections']} |")
    lines += ['', 'Cause confirmee en prise a deux mains : joint anime et inverse bind omis par le HUD.',
        'Main libre : aucune faute de facteur uniforme demontree. Ne pas assimiler le canevas au contenu visible.',
        '', *report['limitations']]
    (args.out/'comparaison.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    print(json.dumps(report))
