"""Bake the user's saved shotgun pose without fitting, collisions or pose edits.

Run with Blender --background --factory-startup --python-exit-code 1 --python FILE.
The current playable package and editable user pose are read-only inputs.
"""
import sys
sys.dont_write_bytecode = True
import hashlib
import importlib.util
import json
import pathlib
import shutil
import struct
import subprocess

import bpy
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/manual-shotgun-pose'
SOURCE = ROOT / 'references/KHARVOX-Poseur-Shotgun'
RUNTIME = ROOT / 'build/pump-review-v3/runtime-fixture'


def module(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'KHARVOX/tools' / filename)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def moved(points, matrix):
    return points @ matrix[:3, :3].T + matrix[:3, 3]


def replay(points, cm_points, anchor, meters, frame):
    native = np.array(frame['PROP']['axes']).T
    rotation = native / np.linalg.norm(native, axis=0)
    fixed = rotation * meters
    offset = (native / 39.3701 - fixed) @ anchor
    actual = points @ fixed.T + offset
    expected = (cm_points / 100) @ rotation.T
    return float(np.linalg.norm(actual - expected, axis=1).max())


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for src, dst in (('shotgun_pose.json', 'pose-source.json'),
                     ('Praetor-Shotgun.blend', 'Praetor-Shotgun-source.blend')):
        target = OUT / dst
        if target.exists():
            if digest(target) != digest(SOURCE / src):
                raise RuntimeError('Frozen source already exists with a different pose: ' + str(target))
        else:
            shutil.copy2(SOURCE / src, target)
    pose = json.loads((OUT / 'pose-source.json').read_text())
    if pose['weapon'] != 'shotgun':
        raise RuntimeError('This exporter is limited to the standard shotgun')
    builder = module('manual_export_builder', 'build_praetor_hands.py')
    exporter = module('manual_export_glb', 'export_reviewed_support_hands.py')
    poseur = module('manual_export_poseur', 'manual_praetor_poseur.py')
    bpy.ops.wm.open_mainfile(filepath=str(OUT / 'Praetor-Shotgun-source.blend'))
    bpy.context.view_layer.update()
    initial = {side: builder.bake(bpy.data.objects['PRAETOR_' + side], np.eye(4))[0]
               for side in ('left', 'right')}
    poseur.restore(pose)
    report = dict(weapon='shotgun', status='STATIC_EXPORT_PENDING_GAME_TEST',
                  source_json_sha256=digest(OUT / 'pose-source.json'),
                  source_scene_sha256=digest(OUT / 'Praetor-Shotgun-source.blend'),
                  source_json_version=pose['version'], hands={},
                  automatic_fitting=False, collision_processing=False,
                  runtime_skinning=False, hud_or_menu_changes=False)
    lines = (RUNTIME / 'support_hands.cfg').read_text().splitlines()
    line_index = next(i for i, line in enumerate(lines) if line.split()[:2] == ['4', 'shotgun'])
    row = lines[line_index].split()
    original_row = row.copy()
    meters = float(row[12])
    weapon_cm = float(pose['weapon_cm_per_unit'])
    world_per_native = weapon_cm / 100
    measured = json.loads((ROOT / 'build/pump-review-v4/propositions-v4.json').read_text())
    frames = [f for f in measured['observations']['frames'] if 'PROP' in f]
    if len(row) != 49:
        raise RuntimeError('Expected 17 v3 fields followed by two 16-value v4 matrices')
    primary = row[:17]
    primary[0] = '3'
    config = poseur.load_module('manual_config_reader', 'preview_praetor_hud_clearance.py').key_values(RUNTIME / 'hand_models.cfg')
    primary[2] = config['left_fist']
    primary[4] = str(exporter.fnv64((RUNTIME / primary[2]).read_bytes()))
    for side in ('left', 'right'):
        hand = bpy.data.objects['PRAETOR_' + side]
        points_cm, normals, mapping = builder.bake(hand, np.eye(4))
        _, _, uvs, indices = mapping
        if points_cm.shape != initial[side].shape:
            raise RuntimeError('JSON changed the source topology')
        json_scene_error = float(np.linalg.norm(points_cm - initial[side], axis=1).max())
        if json_scene_error > .002:
            raise RuntimeError('Saved Blender scene and JSON disagree for ' + side
                + ': ' + str(json_scene_error) + ' cm. Do not guess which pose to export.')
        rig = bpy.data.objects['RIG_' + side]
        wrist_cm = np.array(rig.matrix_world @ rig.pose.bones[side + 'hand'].matrix)
        anchor = np.array([float(v) for v in row[6:9]]) if side == 'left' else wrist_cm[:3, 3] / weapon_cm
        cm_to_asset = np.eye(4)
        cm_to_asset[:3, :3] *= 1 / (meters * 100)
        cm_to_asset[:3, 3] = (1 - world_per_native / meters) * anchor
        points = moved(points_cm, cm_to_asset)
        relative = 'assets/3D/' + ('SUPPORT_SHOTGUN_LEFT.glb' if side == 'left' else 'PRIMARY_SHOTGUN_RIGHT.glb')
        raw = exporter.geometry_glb(vars(builder), points, normals, uvs, indices,
            'MANUAL_SHOTGUN_' + side.upper(), dict(source_pose_sha256=report['source_json_sha256'],
                physical_hand=side, role='support' if side == 'left' else 'primary',
                reference_weapon_scale=weapon_cm * 39.3701 / 100,
                coordinate_space='fixed-size weapon asset units with reference anchor compensation',
                no_runtime_skinning=True, no_automatic_fitting=True))
        validation = exporter.validate_glb(raw, points, indices.reshape(-1, 3))
        target = OUT / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)
        error = max(replay(points, points_cm, anchor, meters, frame) for frame in frames)
        if error > .00002:
            raise RuntimeError('Native weapon matrix replay exceeds 0.02 mm')
        # Independent rest-scale test: weapon scaling cannot resize the glove.
        reference = np.linalg.norm(points[::61] - points[0], axis=1) * meters
        for scale in (.5, .77, 1, 2):
            for reflection in (-1, 1):
                native = np.diag((scale * reflection, scale, scale)) / 39.3701
                fixed = np.diag((reflection, 1, 1)) * meters
                placed = points @ fixed.T + (native - fixed) @ anchor
                distances = np.linalg.norm(placed[::61] - placed[0], axis=1)
                if not np.allclose(reference, distances, atol=2e-5):
                    raise RuntimeError('Weapon scale changed physical hand dimensions')
        fnv = exporter.fnv64(raw)
        if side == 'left':
            row[2], row[4] = relative, str(fnv)
            fingers = (RUNTIME / 'assets/3D/DOOM_LEFT_HAND_FINGERS.bin').read_bytes()
            fist_wrist = np.array(struct.unpack_from('<16f', fingers, 28)).reshape(4, 4).T
            fist_to_asset = cm_to_asset @ wrist_cm @ np.linalg.inv(fist_wrist)
            row[17:33] = [format(v, '.12g') for v in fist_to_asset.T.flatten()]
        else:
            primary[3], primary[5] = relative, str(fnv)
            primary[9:12] = [format(v, '.12g') for v in anchor]
        report['hands'][side] = dict(path=relative, sha256=hashlib.sha256(raw).hexdigest(),
            fnv64=str(fnv), json_scene_max_error_cm=json_scene_error,
            native_replay_max_error_m=error, native_frames=len(frames),
            wrist_world_cm=wrist_cm.tolist(), anchor_native=anchor.tolist(),
            **validation)
        print(side, report['hands'][side], flush=True)
    lines[line_index] = ' '.join(row)
    (OUT / 'support_hands.cfg').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    # The existing profile parser accepts data rows only, without comments.
    (OUT / 'primary_hands.cfg').write_text(' '.join(primary) + '\n', encoding='utf-8')
    # All other profiles and every physical-right support field stay unchanged.
    changed = {i for i, (a, b) in enumerate(zip(original_row, row)) if a != b}
    if len(row) != 49 or len(primary) != 17:
        raise RuntimeError('Serialized profile token count changed')
    if not changed <= ({2, 4} | set(range(17, 33))):
        raise RuntimeError('Export unexpectedly altered right support or scale configuration')
    subprocess.run([str(ROOT / 'build/runtime-msvc/KharvoxWeaponSupportConfigTests.exe'),
                    str(OUT / 'support_hands.cfg'), '2'], check=True)
    report['other_support_profiles_unchanged'] = all(
        a == b for i, (a, b) in enumerate(zip((RUNTIME / 'support_hands.cfg').read_text().splitlines(), lines))
        if i != line_index)
    report['original_package_sha256'] = {
        name: digest(RUNTIME / name) for name in ('KharvoxLayer.dll', 'native_sfs_build.txt',
            'KharvoxDesktop.exe', 'offhand_hud.cfg', 'hand_models.cfg', 'hand_models_calibration_default.cfg')}
    (OUT / 'rapport.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    (OUT / 'rapport.md').write_text(
        '# Export de la pose manuelle / shotgun\n\n'
        'Pose utilisateur conservee : aucun ajustement automatique ni calcul de collision.\n'
        'Les deux mains et leurs doigts sont cuits depuis le squelette Praetor natif.\n'
        'Les materiaux partagent les atlas existants. Aucun calcul de skinning en jeu.\n'
        'Le repere et la taille physique sont verifies avec 17 matrices natives F4.\n'
        'Main gauche : profil de soutien existant. Main droite : profil optionnel pendant le Grip.\n'
        'Mode gaucher et autres armes : chemins existants conserves.\n'
        'Validation casque encore necessaire : Grip, relachement, animations, deux yeux SFS/AER.\n', encoding='utf-8')
    assert digest(SOURCE / 'shotgun_pose.json') == report['source_json_sha256']
    assert digest(SOURCE / 'Praetor-Shotgun.blend') == report['source_scene_sha256']
    print('EXPORT COMPLETE: only shotgun left support and right primary; source pose unchanged', flush=True)


if __name__ == '__main__':
    main()
