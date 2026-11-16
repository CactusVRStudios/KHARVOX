"""Verify existing joint evidence, without capturing or changing an artist pose.

Run with Blender's Python (numpy), then use the existing workshop GLB exporter.
Archive snapshots have no weapon label: identity is explicitly inferred from
the ordered static-joint fingerprint, never from the snapshot ordinal alone.
"""
import sys
import tempfile
sys.dont_write_bytecode = True
import importlib.util
import json
from pathlib import Path
import re
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('verified_workshop', ROOT/'KHARVOX/tools/hand_workshop_blender.py')
workshop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(workshop)
work = workshop.work


def main():
    if work.PROTECTED:
        raise ValueError('Arme validee protegee : aucune reconversion.')
    if work.WEAPON in ('plasma_rifle','rocket_launcher','gauss_cannon','chaingun','bfg','chainsaw','pistol'):
        native_spec = importlib.util.spec_from_file_location('native_reference', ROOT/'KHARVOX/tools/verify_native_skeleton_conversion.py')
        native = importlib.util.module_from_spec(native_spec)
        native_spec.loader.exec_module(native)
        return native.main()
    paths = [work.OUT/'atelier.blend', work.OUT/'pose.json',
        ROOT/'references/KHARVOX-Shotgun-Conversion-Native/pose-convertie.json',
        ROOT/'references/KHARVOX-Shotgun-Conversion-Native/Praetor-Shotgun-Conversion-Native.blend']
    original_hashes = {str(p): work.sha(p) for p in paths}
    source = json.loads(paths[2].read_text(encoding='utf-8-sig'))
    log = Path(tempfile.gettempdir()) / 'KHARVOX-WEAPON-BONE-SNAPSHOTS.log'
    sections = re.split(r'^=== SNAPSHOT ', log.read_text(), flags=re.M)[1:]
    bones = workshop.model().Skeleton().Bones()
    landmarks = {
        'super_shotgun': ([0,1,2,3,4,5,6], ['gunroot','base','hinge','buckle','handle','trigger','release'], 3),
        'heavy_assault_rifle': ([0,1,2,3,15,17], ['origin','base_jnt','back_sight_jnt','front_sight','attach_top_jnt','attach_barrel_jnt'], 2),
        'plasma_rifle': ([0,1,9,12,13], ['origin','base_jnt','front_post_top_jnt','front_sight_base_jnt','attach_top_jnt'], 2),
    }
    indices, names, required = landmarks[work.WEAPON]
    if [bones[i].Name() for i in indices] != names:
        raise ValueError('Static joint fingerprint changed; export blocked.')
    exported = np.array([bones[i].WorldPosition() for i in indices])
    matches = []
    scales = {}
    for section in sections:
        number = int(section.split()[0])
        rows = re.findall(r'^PROP_BONE (\d+) origin=([^ ]+) axis=([^\n]+)', section, re.M)
        prop = re.search(r'^PROP entity=.* axis=([^\n]+)', section, re.M)
        if not prop or len(rows) <= max(indices):
            continue
        axis = np.array([float(v) for v in prop[1].split(',')]).reshape(3,3)
        sizes = np.linalg.norm(axis, axis=1)
        if not np.allclose(axis@axis.T, np.eye(3)*sizes[0]**2, atol=.00001):
            continue
        scales[number] = float(sizes.mean())
        observed = np.array([[float(v) for v in row[1].split(',')] for row in [rows[i] for i in indices]])
        factor = float(np.sum(exported*observed)/np.sum(exported*exported))
        error = float(np.linalg.norm(observed-exported*factor, axis=1).max())
        if factor <= 0 or error > .0001:
            continue
        if [int(row[0]) for row in [rows[i] for i in indices]] != indices:
            raise ValueError('Unexpected native joint order.')
        # Both chosen attachment axes must agree with the authored bind frame.
        for index in {a['index'] for a in work.settings()['attachments'].values()}:
            native_axis = np.array([float(v) for v in rows[index][2].split(',')]).reshape(3,3).T
            q = bones[index].WorldRotation()
            expected_axis = np.array(workshop.Quaternion((q[3], *q[:3])).to_matrix())
            if not np.allclose(native_axis, expected_axis, atol=.00001):
                raise ValueError('Attachment rotation is not the exported reference.')
        matches.append(dict(snapshot=number, factor=factor, max_residual=error))
    if len(matches) < required:
        raise ValueError('Conversion native de '+work.WEAPON+' non verifiable : '
            +str(len(matches))+' releve(s) compatible(s), '+str(required)+' requis. '
            'Pose preservee ; export et paquet bloques, aucune echelle supposee.')
    factor = float(np.median([m['factor'] for m in matches]))
    if np.ptp([m['factor'] for m in matches]) > .00001:
        raise ValueError('Inconsistent native scale.')
    # Match this ordered seven-joint fingerprint against all available exports.
    from cast import Cast, Model
    compatible = []
    observed = exported*factor
    for path in (ROOT/'build/native-weapons-export').rglob('*.cast'):
        model = next(m for r in Cast.load(str(path)).Roots() for m in r.ChildrenOfType(Model))
        candidate = model.Skeleton().Bones()
        if len(candidate) <= max(indices):
            continue
        points = np.array([b.WorldPosition() for b in [candidate[i] for i in indices]])
        denominator = np.sum(points*points)
        if denominator == 0:
            continue
        fitted = float(np.sum(points*observed)/denominator)
        if fitted > 0 and np.linalg.norm(points*fitted-observed,axis=1).max() < .0001:
            compatible.append(str(path.resolve()))
    if compatible != [str(work.CAST.resolve())]:
        raise ValueError('Ambiguous archive identity: '+str(compatible))
    if not all(i in scales for i in (9,10,11)):
        raise ValueError('Validated shotgun reference prop scale missing.')
    prop_ratio = float(np.median([scales[m['snapshot']] for m in matches])/np.median([scales[i] for i in (9,10,11)]))
    cm = source['weapon_cm_per_unit']*prop_ratio
    c = json.loads((work.OUT/'conversion.json').read_text())
    artist_pose = json.loads((work.OUT/'pose.json').read_text(encoding='utf-8-sig'))
    if abs(factor*cm-artist_pose['weapon_cm_per_unit']) > .00001:
        raise ValueError('Artist display geometry and native geometry require a separate conversion; preserve originals.')
    c.update(export_verified=True, coordinate_frame='cast_weapon_centimetres',
        cast_to_native_scale=factor, native_cm_per_unit=cm, export_block_reason=None,
        native_positions=(np.array([b.WorldPosition() for b in bones])*factor).tolist(),
        native_log_sha256=work.sha(log), identity_method='Inference by unique ordered static-joint fingerprint across available exports',
        static_joint_names=names, static_joint_indices=indices, existing_snapshots=matches, prop_scale_ratio_to_validated_shotgun=prop_ratio,
        excluded_from_scale_fit=[b.Name() for i,b in enumerate(bones) if i not in indices],
        note='Existing archive evidence, no new capture. Artist centimetre geometry unchanged; native bind translations converted by measured scale.')
    (work.OUT/'conversion.json').write_text(json.dumps(c,indent=2),encoding='utf-8')
    workshop.export()
    assert all(work.sha(Path(p)) == value for p,value in original_hashes.items()), 'Original artist/reference inputs changed'
    print('CONVERSION AND GLB EXPORT PASS',json.dumps(matches), 'native_cm_per_unit=',cm, flush=True)


if __name__ == '__main__':
    try:
        main()
    except ValueError as error:
        print('CONVERSION BLOQUEE : '+str(error), flush=True)
        sys.exit(1)
