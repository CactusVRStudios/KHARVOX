"""Offline export gate. No scene writes, fitting, game capture or UI changes.

The two-hand check is a geometric simulation, not headset validation. Runtime
target selection must match reviewedSupportPointInBody; unknown weapons keep
the native-anchor path until their integration explicitly changes it.
"""
import sys
sys.dont_write_bytecode = True
import json
from pathlib import Path
import numpy as np


def runtime_target_mode(weapon):
    return 'authored_grip' if weapon in ('heavy_assault_rifle', 'plasma_rifle', 'rocket_launcher', 'gauss_cannon', 'chaingun', 'bfg', 'chainsaw', 'pistol') else 'native_anchor'


def unit(value):
    vector = np.asarray(value, dtype=float)
    if vector.shape != (3,) or not np.isfinite(vector).all() or np.linalg.norm(vector) < 1e-8:
        raise ValueError('Vecteur de visee absent ou degenere.')
    return vector / np.linalg.norm(vector)


def frame(forward, up):
    # Same forward/up construction as solveTwoHandOrientation.
    forward = unit(forward)
    right = unit(np.cross(forward, up))
    return np.column_stack((right, np.cross(right, forward), -forward))


def check_geometry(pivot, grip, primary_grip, forward, up, radius, target_mode, aim_mode='barrel'):
    values = np.asarray([pivot, grip, primary_grip], dtype=float)
    if values.shape != (3, 3) or not np.isfinite(values).all() or not np.isfinite(radius) or radius <= 0:
        raise ValueError('Points ou rayon d accroche invalides.')
    pivot, grip, primary_grip = values
    forward, up = unit(forward), unit(up)
    if target_mode not in ('authored_grip', 'native_anchor'):
        raise ValueError('Politique de cible runtime inconnue.')
    target = grip if target_mode == 'authored_grip' else pivot
    gap = float(np.linalg.norm(target - grip))
    result = dict(animation_pivot_m=pivot.tolist(), authored_grip_m=grip.tolist(),
        runtime_target_m=target.tolist(), runtime_target_mode=target_mode,
        pivot_to_grip_m=float(np.linalg.norm(pivot - grip)), capture_error_m=gap,
        capture_radius_m=float(radius), errors=[], orientation_cases=[])
    if gap > radius:
        result['errors'].append('Le runtime utilise le pivot hors du rayon de la main posee.')
    if target_mode == 'native_anchor' and gap > .0001:
        result['errors'].append('Pivot d animation distinct du point de manette : la cible runtime doit etre corrigee avant integration.')
    expected_line = grip - primary_grip
    selected_line = target - primary_grip
    if np.linalg.norm(expected_line) < .001 or np.linalg.norm(selected_line) < .001:
        result['errors'].append('Prise secondaire confondue avec la prise principale.')
    elif aim_mode == 'barrel':
        # Scale-relative stock positions are test scenarios, never exported
        # offsets. Exercise both no-stock and virtual-stock aiming lines.
        span = float(np.linalg.norm(expected_line))
        for stock_span in (0., .5, 1., 2.):
            stock = primary_grip - forward * span * stock_span
            try:
                rotation = frame(target-stock, up) @ frame(selected_line, up).T
                dot = float(np.dot(rotation @ forward, forward))
            except ValueError:
                dot = -1.
            result['orientation_cases'].append(dict(stock_span=stock_span, forward_dot=dot))
            if dot <= 0:
                result['errors'].append('Le solveur retourne le canon dans un scenario de prise a deux mains.')
        if float(np.dot(expected_line, forward)) <= 0:
            result['errors'].append('La main de soutien posee est derriere la prise principale pour une visee barrel.')
    elif aim_mode == 'side-grip':
        # Exercise rotations about the actual barrel, not a constant result.
        lateral = expected_line-forward*np.dot(expected_line, forward)
        if np.linalg.norm(lateral) < .001:
            result['errors'].append('Poignee laterale confondue avec l axe du canon.')
        else:
            x, y, z = forward
            cross = np.array(((0,-z,y),(z,0,-x),(-y,x,0)))
            for angle in (0., np.pi/2, np.pi, -np.pi/2):
                rotation = np.eye(3)*np.cos(angle)+(1-np.cos(angle))*np.outer(forward,forward)+np.sin(angle)*cross
                dot = float(np.dot(rotation@forward,forward))
                result['orientation_cases'].append(dict(mode='roll_only', roll_radians=angle, forward_dot=dot))
                if dot <= 0:
                    result['errors'].append('La correction de roulis inverse le canon.')
    else:
        result['errors'].append('Mode de visee inconnu.')
    result['passed'] = not result['errors']
    return result


def calibration_matrix(directory, side):
    values = {}
    for name in ('hand_models.cfg', 'hand_models_calibration_default.cfg', 'hand_models_calibration_saved.cfg'):
        path = Path(directory)/name
        if path.is_file():
            for line in path.read_text(encoding='utf-8-sig').splitlines():
                if '=' in line and not line.lstrip().startswith('#'):
                    key, value = line.split('=', 1)
                    key = key.strip()
                    # Runtime calibration files override position/rotation;
                    # scale and mirror flags are owned by hand_models.cfg.
                    if name == 'hand_models.cfg' or key in (side+'_position', side+'_rotation'):
                        values[key] = value.strip()
    try:
        position = np.asarray([float(v) for v in values[side+'_position'].split()])
        angles = np.radians([float(v) for v in values[side+'_rotation'].split()]) / 2
        scale = float(values[side+'_scale'])
        mirror = values[side+'_fist_mirror_x'] == '1'
        if position.shape != (3,) or angles.shape != (3,) or not np.isfinite(position).all() or not np.isfinite(angles).all() or not np.isfinite(scale) or scale <= 0:
            raise ValueError('Calibration invalide.')
    except (KeyError, ValueError) as error:
        raise ValueError('Calibration FIST manquante ou invalide : '+side) from error
    sx, sy, sz = np.sin(angles)
    cx, cy, cz = np.cos(angles)
    x, y, z, w = sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz, cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz
    rotation = np.array(((1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)),
        (2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)),
        (2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y))))
    result = np.eye(4)
    result[:3, :3] = rotation @ np.diag((-scale if mirror else scale, scale, scale))
    result[:3, 3] = position
    return result, scale


def exported_grips(output, reference, report=None):
    output, reference = Path(output), Path(reference)
    if report is None:
        report = json.loads((output/'export/report.json').read_text(encoding='utf-8'))
    conversion = report['calibration']
    if conversion.get('export_verified') is not True:
        raise ValueError('Conversion non verifiee : controle des prises impossible.')
    row = next(line.split() for line in (output/'export/support_hands.cfg').read_text().splitlines()
               if line.split()[1] == report['weapon'])
    if row[0] != '5' or len(row) != 83:
        raise ValueError('Profil v5 requis pour le controle du pivot et des prises.')
    units = float(conversion['native_cm_per_unit'])/100
    asset_units = float(row[12])
    if not np.isfinite(units) or units <= 0 or not np.isfinite(asset_units) or asset_units <= 0:
        raise ValueError('Echelle de conversion invalide.')
    pivot = np.asarray([float(v) for v in row[6:9]])
    points, frames = {}, {}
    for hand, side in enumerate(('left', 'right')):
        mapped = np.asarray(report['hands'][side]['fist_to_asset'], dtype=float)
        if mapped.shape != (4, 4) or not np.isfinite(mapped).all():
            raise ValueError('Repere de main manquant ou invalide : '+side)
        calibration, scale = calibration_matrix(reference, side)
        frames[side] = mapped @ np.linalg.inv(calibration)
        asset_grip = frames[side][:3, 3]
        # Same fixed-size transform as the renderer, with an identity native
        # weapon orientation: translations are already in native weapon space.
        anchor = pivot if hand == 0 else np.asarray(report['hands'][side]['anchor_native'])
        meters = asset_units * scale / float(row[13+hand])
        points[side] = meters*asset_grip + (units-meters)*anchor
    return report, row, units, pivot, points, frames


def controller_relative_support(support, primary, basis):
    basis = np.asarray(basis, dtype=float)
    if basis.shape != (3,3) or not np.isfinite(basis).all():
        raise ValueError('Repere de manette invalide.')
    scales = np.linalg.norm(basis, axis=0)
    if np.any(scales < 1e-8):
        raise ValueError('Repere de manette degenere.')
    rotation = basis/scales
    if not np.allclose(rotation.T@rotation, np.eye(3), atol=.0001) or np.linalg.det(rotation) < .99:
        raise ValueError('Repere de manette non rigide ou en miroir.')
    result = rotation.T@(np.asarray(support)-np.asarray(primary))
    if result.shape != (3,) or not np.isfinite(result).all():
        raise ValueError('Prise relative invalide.')
    return result


def pistol_aim_profile(output, reference):
    report, row, units, pivot, points, frames = exported_grips(output, reference)
    if report['weapon'] != 'pistol':
        raise ValueError('Creation de profil limitee au pistolet.')
    shared = next(line.split() for line in (Path(reference)/'two_hand_weapon_profiles_default.cfg').read_text().splitlines()
                  if line.split()[:2] == ['2','shotgun'])
    offset = controller_relative_support(points['left'], points['right'], frames['right'][:3,:3])
    # Existing roll-only mode preserves dominant-hand aiming for a cupped grip.
    # Radius/weight are the existing project settings; position comes from pose.
    radius, weight = float(shared[6]), float(shared[7])
    basis = report['weapon_axes_native']
    checked = check_geometry(pivot*units, points['left'], points['right'], basis['forward'], basis['up'],
                             radius, 'authored_grip', 'side-grip')
    if not checked['passed']:
        raise ValueError('Profil pistolet bloque : '+' '.join(checked['errors']))
    return '2 pistol side-grip '+' '.join(format(v,'.12g') for v in (*offset, radius, weight))


def check_export(output, reference, *, target_mode=None, report=None, aim_profiles_path=None):
    reference = Path(reference)
    report, row, units, pivot, points, frames = exported_grips(output, reference, report)
    profile = None
    for path in (Path(aim_profiles_path) if aim_profiles_path else reference/'two_hand_weapon_profiles_default.cfg',
                 reference/'two_hand_weapon_profiles_saved.cfg'):
        if path.is_file():
            for line in path.read_text(encoding='utf-8-sig').splitlines():
                fields = line.split()
                if len(fields) == 8 and fields[:2] == ['2', report['weapon']]:
                    profile = fields
    if profile is None:
        raise ValueError('Profil de visee a deux mains absent.')
    basis = report.get('weapon_axes_native')
    if not basis or basis.get('verified') is not True:
        raise ValueError('Axes canon/haut non verifies : export concerne bloque.')
    result = check_geometry(pivot*units, points['left'], points['right'], basis['forward'], basis['up'],
        float(profile[6]), target_mode or runtime_target_mode(report['weapon']), profile[2])
    result.update(weapon=report['weapon'], scope='offline geometry and two-hand solver scenarios; human game validation required')
    return result
