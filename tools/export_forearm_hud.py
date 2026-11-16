"""Convert the saved forearm-local HUD without modifying any artist input."""
import sys
sys.dont_write_bytecode = True
import hashlib
import argparse
import json
import subprocess
from pathlib import Path
import numpy as np

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/hud-left-workshop'
SOURCE = OUT / 'hud-left.json'
PANEL_NAMES = ('HUD_Munitions', 'HUD_Vie_Armure', 'HUD_Options')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matrix(value, rigid=True):
    result = np.asarray(value, dtype=float)
    if result.shape != (4,4) or not np.isfinite(result).all() or not np.allclose(result[3], (0,0,0,1), atol=1e-6):
        raise ValueError('Matrice HUD invalide')
    basis = result[:3,:3]
    if rigid and (not np.allclose(basis.T@basis,np.eye(3),atol=2e-4) or np.linalg.det(basis)<.999):
        raise ValueError('Echelle/reflexion HUD inattendue ; export bloque')
    return result


def convert(data):
    if data.get('format')!='KHARVOX_FOREARM_HUD_AUTHORING' or data.get('version')!=1 or data.get('units')!='centimetres' or data.get('physical_hand')!='left':
        raise ValueError('Format de pose HUD incompatible')
    anchor=data['anchor']
    if anchor['name']!='HUD_anchor_left' or anchor['bone']!='LeftForeArmRoll2':
        raise ValueError('Ancrage avant-bras incompatible')
    for name,expected in data['input_hashes'].items():
        path=Path(name).resolve()
        if not path.is_relative_to(ROOT) or sha(path)!=expected:
            raise ValueError('Source changee : '+str(path))
    frame=matrix(anchor['frame_in_grip_xr_cm'])
    native=matrix(anchor['native_to_grip_xr_cm'],False) @ matrix(anchor['bone_matrix_native_at_bind']) @ matrix(anchor['bone_to_anchor_bind'],False)
    if not np.allclose(native,frame,atol=2e-4):
        raise ValueError('Le repere os/avant-bras ne correspond pas au repere Grip enregistre')
    # DOOM forward/left/up -> XR right/up/back. Only the output/world side
    # changes basis: panel local right/up/front remains exactly as authored.
    doom_to_xr=np.array(((0,-1,0),(0,0,1),(-1,0,0)),dtype=float)
    xr_to_doom=np.eye(4);xr_to_doom[:3,:3]=doom_to_xr.T
    lines=['1 1'];panels=[];maximum_error=0.
    for slot,name in enumerate(PANEL_NAMES):
        item=data['transforms'][name]
        local=matrix(item['matrix_anchor_local_cm'])
        xr=frame@local
        converted=xr_to_doom@xr
        width=float(item['width_cm']);height=float(item['height_cm'])
        if not (np.isfinite(width) and np.isfinite(height) and 1<=width<=100 and 1<=height<=100):
            raise ValueError('Dimensions HUD invalides')
        if slot<2 and not np.isclose(height/width,300/512,atol=1e-5):
            raise ValueError('Proportions du canevas natif modifiees')
        if slot==2 and not item.get('dimensions_provisional'):
            raise ValueError('Hauteur Options attendue provisoire ; le contenu natif determine sa hauteur reelle')
        restored=np.eye(4);restored[:3,:3]=doom_to_xr
        error=float(np.max(np.abs(restored@converted-xr)))
        maximum_error=max(maximum_error,error)
        values=[width,0. if slot==2 else height,*converted[:3,:].reshape(-1)]
        lines.append(' '.join(format(float(v),'.10g') for v in values))
        panels.append(dict(slot=slot,blender_object=name,matrix_doom_grip_cm=converted.tolist(),width_cm=width,
            height_cm=None if slot==2 else height,roundtrip_max_error=error))
    return '\n'.join(lines)+'\n',dict(source=str(SOURCE),source_sha256=sha(SOURCE),
        native_anchor_roundtrip_verified=True,max_roundtrip_error=maximum_error,panels=panels,
        native_owner_mapping={'0':'WeaponInfo / munitions','1':'BottomLeft / vie et armure','2':'ProgMeter / options'},
        automatic_clearance_disabled=True,controller_offset_added=False,game_validated=False)


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--source',type=Path,default=SOURCE)
    parser.add_argument('--out',type=Path,default=OUT)
    parser.add_argument('--scene',type=Path)
    parser.add_argument('--runtime',type=Path,default=ROOT/'artifacts/KHARVOX-VR-Valide')
    args=parser.parse_args()
    SOURCE=args.source.resolve();OUT=args.out.resolve()
    if not SOURCE.is_relative_to(ROOT) or not OUT.is_relative_to(ROOT):raise ValueError('Sortie hors workspace')
    before=sha(SOURCE)
    data=json.loads(SOURCE.read_text(encoding='utf-8-sig'))
    config,report=convert(data)
    scene=args.scene or SOURCE.with_name('HUD-Contextuel-Praetor.blend' if 'gesture_preview' in data else 'HUD-Praetor-Gauche.blend')
    blender=ROOT/'.tools/blender-5.2.3/blender-5.2.3-windows-x64/blender.exe'
    subprocess.run([str(blender),'--background','--factory-startup','--disable-autoexec',
        '--python-exit-code','1','--python',str(Path(__file__).with_name('audit_hud_scale_chain.py')),
        '--','--source',str(SOURCE),'--scene',str(scene),'--runtime',str(args.runtime.resolve()),'--out',str(OUT)],check=True)
    report['dimensional_audit']='comparaison.json'
    if 'gesture_preview' in data:
        gesture=data['gesture_preview']
        keys=('face_on','face_off','fov_on','fov_off','near_cm','far_cm','on_ms','off_ms','fade_in_ms','fade_out_ms')
        values=[float(gesture[key]) for key in keys]
        if not np.isfinite(values).all() or not (0<=values[0]<values[1]<90 and 0<values[2]<values[3]<90
            and 1<=values[4]<values[5]<=200 and all(0<=v<=2000 for v in values[6:8]) and all(1<=v<=2000 for v in values[8:])):
            raise ValueError('Seuils du geste invalides')
        (OUT/'offhand_hud_gesture.cfg').write_text('1 1 '+' '.join(format(v,'.10g') for v in values)+'\n',encoding='ascii')
        report['gesture_parameters']=gesture
        report['phase_c_requested_by_user']=True
    (OUT/'offhand_forearm_left.cfg').write_text(config,encoding='ascii')
    (OUT/'conversion.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    if sha(SOURCE)!=before: raise ValueError('Pose source modifiee')
    print(json.dumps(report))
