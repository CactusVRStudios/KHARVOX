"""Offline native MD6 skeleton evidence; reuse the existing RES-v5 reader.

Only bounded, checked skeleton layouts observed in these local resources are
accepted. Every rest transform must invert the independently stored bind matrix.
Input game archives and artist files are never opened for writing.
"""
import sys
import tempfile
sys.dont_write_bytecode = True
import importlib.util
import json
from pathlib import Path
import re
import struct
import numpy as np

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def quaternion_matrix(q):
    x,y,z,w = q
    return np.array(((1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)),
        (2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)),
        (2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y))))


def parse_skeleton(raw):
    if raw[:4] != bytes.fromhex('0e02534d') or len(raw) < 76:
        raise ValueError('Unsupported native skeleton header')
    count = struct.unpack_from('>H', raw, 14)[0]
    if not 1 <= count <= 512:
        raise ValueError('Invalid native skeleton joint count')
    padded = (count+7)//8*8
    quat_offset = 76
    scale_offset = quat_offset+padded*16
    position_offset = scale_offset+padded*12
    if position_offset+padded*12 > len(raw):
        raise ValueError('Truncated native rest pose')
    quats = np.array(struct.unpack_from('>'+str(padded*4)+'f',raw,quat_offset)).reshape(padded,4)[:count]
    scales = np.array(struct.unpack_from('>'+str(padded*3)+'f',raw,scale_offset)).reshape(padded,3)[:count]
    positions = np.array(struct.unpack_from('>'+str(padded*3)+'f',raw,position_offset)).reshape(padded,3)[:count]
    if not np.isfinite(quats).all() or not np.isfinite(positions).all() or not np.allclose(np.linalg.norm(quats,axis=1),1,atol=1e-5):
        raise ValueError('Invalid native rest transforms')
    if not np.allclose(scales,1,atol=1e-7):
        raise ValueError('Scaled native hierarchy requires separate verification')
    candidates = []
    # Regular layout and the verified Super Shotgun auxiliary-table variant.
    for pose_padding, extra_metadata in ((0,0),(32,80)):
        parent_offset = position_offset+padded*12+pose_padding
        inverse_offset = parent_offset+padded*4+(count+15)//16*16+extra_metadata
        name_offset = inverse_offset+padded*48
        if name_offset+count*5 > len(raw):
            continue
        parents = struct.unpack_from('>'+str(padded)+'h',raw,parent_offset)[:count]
        if parents[0] != -1 or any(not -1 <= parent < i for i,parent in enumerate(parents)):
            continue
        names, cursor = [], name_offset
        try:
            for i in range(count):
                length = struct.unpack_from('<I',raw,cursor)[0];cursor += 4
                if not 1 <= length <= 256 or cursor+length > len(raw):
                    raise ValueError('Invalid native joint name range')
                name = raw[cursor:cursor+length].decode('ascii');cursor += length
                if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*',name):
                    raise ValueError('Invalid native joint name')
                names.append(name)
        except (ValueError,UnicodeError,struct.error):
            continue
        if len(set(names)) != count:
            continue
        worlds = []
        for i in range(count):
            local = np.eye(4);local[:3,:3] = quaternion_matrix(quats[i]);local[:3,3] = positions[i]
            worlds.append(worlds[parents[i]]@local if parents[i]>=0 else local)
        inverses = np.array(struct.unpack_from('>'+str(padded*12)+'f',raw,inverse_offset)).reshape(padded,3,4)[:count]
        if not np.isfinite(inverses).all():
            continue
        error = max(float(np.abs(np.vstack((inverse,[0,0,0,1]))@world-np.eye(4)).max()) for inverse,world in zip(inverses,worlds))
        if error > .0001:
            continue
        candidates.append(dict(names=names, parents=list(parents), local_positions=positions,
            local_quaternions=quats, worlds=np.array(worlds), inverse_bind_error=error,
            offsets=dict(quaternions=quat_offset,positions=position_offset,parents=parent_offset,
                inverse_binds=inverse_offset,names=name_offset), checksum=raw[4:8].hex()))
    if len(candidates) != 1:
        raise ValueError('No unique checked native skeleton layout')
    return candidates[0]


def compare_cast(native, bones):
    if native['names'] != [b.Name() for b in bones] or native['parents'] != [b.ParentIndex() for b in bones]:
        raise ValueError('Native/Cast joint identity or parent mismatch')
    exported = np.array([b.LocalPosition() for b in bones])
    if np.sum(exported*exported) < 1e-8:
        raise ValueError('Insufficient joint baseline for scale measurement')
    factor = float(np.sum(exported*native['local_positions'])/np.sum(exported*exported))
    local_error = float(np.abs(native['local_positions']-exported*factor).max())
    global_error = float(np.linalg.norm(native['worlds'][:,:3,3]-np.array([b.WorldPosition() for b in bones])*factor,axis=1).max())
    q = np.array([b.LocalRotation() for b in bones])
    rotation_error = float(np.minimum(np.abs(native['local_quaternions']-q).max(1),np.abs(native['local_quaternions']+q).max(1)).max())
    if not np.isfinite(factor) or factor<=0 or max(local_error,global_error,rotation_error)>.0001:
        raise ValueError('Native/Cast conversion not consistent across every joint')
    return dict(factor=factor,local_position_error=local_error,global_position_error=global_error,
        rotation_error=rotation_error,inverse_bind_error=native['inverse_bind_error'],joint_count=len(bones),offsets=native['offsets'])


def main():
    workshop = module('native_weapon_workshop',ROOT/'KHARVOX/tools/hand_workshop_blender.py')
    work = workshop.work
    if work.WEAPON not in ('plasma_rifle','rocket_launcher','gauss_cannon','chaingun','bfg','chainsaw','pistol') or work.PROTECTED:
        raise ValueError('Native-source integration requires an explicitly supported, unvalidated weapon')
    stem = work.SPEC['stem']
    family = 'melee' if work.WEAPON=='chainsaw' else 'heavy' if work.WEAPON in ('rocket_launcher','chaingun') else 'energy'
    if work.WEAPON=='pistol':family='pistols'
    model_path = f'objects/weapons/{family}/{stem}'
    declaration_key = stem if work.WEAPON=='gauss_cannon' else work.WEAPON
    base_key = 'plasma_rifle_charged' if work.WEAPON=='plasma_rifle' else declaration_key
    artist_hashes = {p:work.sha(p) for p in (work.OUT/'pose.json',work.OUT/'atelier.blend')}
    reader = module('existing_resource_reader',ROOT/'build/extract-native-hands.py')
    entries = list(reader.entries())
    evidence = work.OUT/'native-reference';evidence.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0,str(ROOT/'.tools/cast/libraries/python'))
    from cast import Cast,Model
    reference_results, natives, bones_by_name = {}, {}, {}
    log = Path(tempfile.gettempdir()) / 'KHARVOX-WEAPON-BONE-SNAPSHOTS.log'
    sections = re.split(r'^=== SNAPSHOT ',log.read_text(),flags=re.M)[1:]
    archived = {int(s.split()[0]):s for s in sections}
    proofs = {'shotgun':([9,10,11],[0,1,2,3,4,5,6,7,8,9,10],[3,1]),
        'double_barrel':([3,4,5,6],[0,1,2,3,4,5,6],[2,1]),
        'heavy_ar':([7,8],[0,1,2,3,15,17],[1])}
    for name in dict.fromkeys(('shotgun','double_barrel','heavy_ar','plasmarifle',stem)):
        entry = next(e for e in entries if e[2].endswith('/'+name+'.bmd6skl'))
        raw = reader.read_resource(entry);native = parse_skeleton(raw)
        cast = next((ROOT/'build/native-weapons-export').rglob(name+'.cast'))
        model = next(m for r in Cast.load(str(cast)).Roots() for m in r.ChildrenOfType(Model));bones=model.Skeleton().Bones()
        result = compare_cast(native,bones)
        if name in proofs:
            snapshots,position_ids,axis_ids = proofs[name];errors=[]
            for snapshot in snapshots:
                rows = re.findall(r'^PROP_BONE (\d+) origin=([^ ]+) axis=([^\n]+)',archived[snapshot],re.M)
                for i in position_ids:
                    errors.append(float(np.linalg.norm(np.array([float(v) for v in rows[i][1].split(',')])-native['worlds'][i,:3,3])))
                for i in axis_ids:
                    axis=np.array([float(v) for v in rows[i][2].split(',')]).reshape(3,3).T
                    errors.append(float(np.abs(axis-native['worlds'][i,:3,:3]).max()))
            if max(errors)>.0001:
                raise ValueError('Native parser does not reproduce the validated '+name+' engine evidence')
            result.update(engine_snapshots=snapshots,engine_reference_error=max(errors))
        destination=evidence/(name+'.bmd6skl');destination.write_bytes(raw)
        result.update(resource=entry[2],archive_patch=entry[6],archive_offset=entry[3],sha256=work.sha(destination),cast_sha256=work.sha(cast))
        reference_results[name]=result;natives[name]=native;bones_by_name[name]=bones
    resources = {
        stem+'.bmd6model':f'generated/basemodel/md6/{model_path}/assets/mesh/{stem}.bmd6model',
        stem+'.md6.decl':f'generated/decls/md6def/zion/{model_path}.md6.decl',
        'weapon-sp.decl':f'generated/decls/weapon/weapon/zion/player/sp/{declaration_key}.decl',
        'weapon-base.decl':f'generated/decls/weapon/weapon/zion/base/{base_key}.decl'}
    if work.WEAPON=='gauss_cannon':
        resources['weapon-parent.decl']='generated/decls/weapon/weapon/zion/player/base/gauss_rifle2.decl'
    if work.WEAPON=='bfg':
        resources['weapon-base.decl']='generated/decls/weapon/weapon/zion/player/sp/bfg_base.decl'
    if work.WEAPON=='pistol':
        del resources['weapon-base.decl']  # The SP declaration directly defines its model.
    raw_files={}
    for filename,key in resources.items():
        entry=next(e for e in entries if e[2]==key);raw=reader.read_resource(entry)
        (evidence/filename).write_bytes(raw);raw_files[filename]=raw
    native=natives[stem];model=raw_files[stem+'.bmd6model']
    if model[:4]!=bytes.fromhex('2b024d4d') or model[8:12].hex()!=native['checksum']:
        raise ValueError('Native model/skeleton revision mismatch')
    length=struct.unpack_from('<I',model,12)[0]
    expected=f'md6/{model_path}/assets/mesh/{stem}.md6skl'
    if model[16:16+length].decode('ascii')!=expected:
        raise ValueError('Native model references a different skeleton')
    if not re.search(rb'offset\s*\(\s*0\s+0\s+0\s*\)',raw_files[stem+'.md6.decl']):
        raise ValueError('Native mesh mount requires separate verification')
    declaration = raw_files['weapon-sp.decl'] if work.WEAPON in ('rocket_launcher','pistol') else raw_files['weapon-base.decl']
    if f'handsModelMD6 = "zion/{model_path}.md6"'.encode() not in declaration:
        raise ValueError('First-person weapon model relationship not verified')
    if work.WEAPON=='gauss_cannon':
        if b'inherit = "weapon/zion/player/base/gauss_rifle2"' not in raw_files['weapon-sp.decl'] or b'inherit = "weapon/zion/base/gauss_rifle"' not in raw_files['weapon-parent.decl']:
            raise ValueError('Gauss first-person inheritance chain not verified')
    if work.WEAPON=='chaingun' and b'inherit = "weapon/zion/base/chaingun"' not in raw_files['weapon-sp.decl']:
        raise ValueError('Chaingun first-person inheritance chain not verified')
    if work.WEAPON=='chainsaw' and b'inherit = "weapon/zion/base/chainsaw"' not in raw_files['weapon-sp.decl']:
        raise ValueError('Chainsaw first-person inheritance chain not verified')
    if work.WEAPON=='bfg' and b'inherit = "weapon/zion/player/sp/bfg_base"' not in raw_files['weapon-sp.decl']:
        raise ValueError('BFG first-person inheritance chain not verified')
    if work.WEAPON in ('gauss_cannon','chaingun','bfg','chainsaw','pistol'):
        for side,item in work.settings()['attachments'].items():
            index=item['index']
            if native['names'][index]!=item['joint']:
                raise ValueError('Native attachment identity mismatch: '+side)
    if work.WEAPON in ('rocket_launcher','gauss_cannon','chaingun','bfg','chainsaw','pistol'):
        parent = 'base_jnt' if work.WEAPON=='bfg' else 'origin'
        if work.WEAPON=='bfg' and not np.allclose(native['worlds'][native['names'].index(parent),:3,:3],np.eye(3),atol=1e-5):
            raise ValueError('BFG muzzle parent rotates the native barrel reference')
        muzzle_tags = re.findall(rb'tag "muzzle"\s*\{([^}]+)\}',raw_files[stem+'.md6.decl'])
        if not muzzle_tags or any(not re.search(rb'rot\s*\(\s*0\s+0\s+0\s+1\s*\)',tag)
                or f'parent "{parent}"'.encode() not in tag for tag in muzzle_tags):
            raise ValueError('Native muzzle orientation requires separate verification')
    runner=(ROOT/'KHARVOX/launcher/KharvoxLauncher/KharvoxRunner.cs').read_text(encoding='utf-8-sig')
    values={key:re.search(r'WriteTemporary\("'+key+r'", "([0-9.]+)"\)',runner) for key in ('weapon_scale.cfg','world_scale.cfg','hands_projection_scale.cfg')}
    if not all(values.values()):raise ValueError('Launcher engine units not verified')
    weapon_scale=float(values['weapon_scale.cfg'][1]);world_scale=float(values['world_scale.cfg'][1])
    if float(values['hands_projection_scale.cfg'][1])!=1:raise ValueError('Non-identity projection requires verification')
    # These are launch configuration values, not guessed model scale factors.
    cm=weapon_scale*100/world_scale;factor=reference_results[stem]['factor']
    pose=json.loads((work.OUT/'pose.json').read_text(encoding='utf-8-sig'))
    if abs(cm*factor-pose['weapon_cm_per_unit'])>.00001:
        raise ValueError('Verified units require a separate rigid conversion; preserve artist inputs')
    report=dict(weapon=work.WEAPON,status='VERIFIED_FROM_NATIVE_GAME_FILES',skeletons=reference_results,
        engine_scale=weapon_scale,world_units_per_meter=world_scale,native_cm_per_unit=cm,
        cast_to_native_scale=factor,game_launched=False,capture_performed=False,
        engine_scale_source='Existing launcher WriteTemporary values and tracked weapon axis construction',
        resource_sha256={name:work.sha(evidence/name) for name in resources})
    (evidence/'reference-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    c=json.loads((work.OUT/'conversion.json').read_text(encoding='utf-8'))
    c.update(export_verified=True,export_block_reason=None,verification_status=report['status'],
        cast_to_native_scale=factor,native_cm_per_unit=cm,native_positions=native['worlds'][:,:3,3].tolist(),
        native_axes=native['worlds'][:,:3,:3].tolist(),native_reference_sha256=work.sha(evidence/'reference-report.json'),
        native_reference_path=str(evidence/'reference-report.json'),
        note='Scale measured from native rest translations, checked by inverse binds and three independently validated engine references. No new capture.')
    (work.OUT/'conversion.json').write_text(json.dumps(c,indent=2),encoding='utf-8')
    workshop.export()
    if any(work.sha(p)!=expected for p,expected in artist_hashes.items()):
        raise RuntimeError('Artist input changed during conversion')
    print('NATIVE CONVERSION VERIFIED',work.WEAPON,json.dumps(reference_results[stem]),flush=True)


if __name__=='__main__':
    main()
