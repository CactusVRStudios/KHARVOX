"""Windows entry point reusing the existing per-weapon Blender poseur."""
import sys
sys.dont_write_bytecode = True
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WEAPONS = {
    'super_shotgun': dict(title='Super Shotgun', stem='double_barrel', meshes=[1],
        attachments={'left': {'joint':'hinge','index':2}, 'right': {'joint':'base','index':1}}, protected=True, axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'heavy_assault_rifle': dict(title="Fusil d'assaut lourd", stem='heavy_ar', meshes=[0],
        attachments={'left': {'joint':'base_jnt','index':1}, 'right': {'joint':'base_jnt','index':1}}, protected=True, axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'plasma_rifle': dict(title='Fusil a plasma', stem='plasmarifle', meshes=[1,2],
        attachments={'left': {'joint':'base_jnt','index':1}, 'right': {'joint':'base_jnt','index':1}}, protected=True,
        axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'rocket_launcher': dict(title='Lance-roquettes', stem='rocketlauncher', meshes=[4],
        attachments={'left': {'joint':'base_jnt','index':1}, 'right': {'joint':'base_jnt','index':1}}, protected=True,
        axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'gauss_cannon': dict(title='Canon Gauss', stem='gauss_rifle', meshes=[1,2],
        attachments={'left': {'joint':'handle_pivot_3_jnt','index':6}, 'right': {'joint':'back_grip_jnt','index':2}}, protected=True,
        axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'chaingun': dict(title='Mitrailleuse', stem='chaingun', meshes=[0,2],
        attachments={'left': {'joint':'front_grip_jnt','index':7}, 'right': {'joint':'base_grip_adjust_jnt','index':2}}, protected=True,
        axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'bfg': dict(title='BFG 9000', stem='bfg', meshes=[4],
        attachments={'left': {'joint':'base_jnt','index':1}, 'right': {'joint':'base_jnt','index':1}}, protected=True,
        axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'chainsaw': dict(title='Tronconneuse', stem='chainsaw', meshes=[0,1],
        attachments={'left': {'joint':'root','index':1}, 'right': {'joint':'root','index':1}}, protected=True,
        axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
    'pistol': dict(title='Pistolet', stem='pistol', meshes=[0],
        attachments={'left': {'joint':'base_jnt','index':1}, 'right': {'joint':'base_jnt','index':1}}, protected=True,
        axes=dict(forward=[1,0,0],up=[0,0,1],verified=True)),
}
WEAPON = sys.argv[sys.argv.index('--weapon')+1] if '--weapon' in sys.argv else 'super_shotgun'
if WEAPON not in WEAPONS:
    raise ValueError('Arme non autorisee dans cet atelier.')
SPEC = WEAPONS[WEAPON]
TITLE = SPEC['title']
PROTECTED = SPEC['protected']
OUT = ROOT / 'build/hand-workshop' / WEAPON
REFERENCE = ROOT / 'artifacts/KHARVOX-VR-Valide'
BLENDER = ROOT / '.tools/blender-5.2.3/blender-5.2.3-windows-x64/blender.exe'
SCRIPT = ROOT / 'KHARVOX/tools/hand_workshop_blender.py'
CAST = next((ROOT / 'build/native-weapons-export').rglob(SPEC['stem']+'.cast'))



def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def baseline():
    return {str(p.relative_to(REFERENCE)): sha(p) for p in REFERENCE.rglob('*') if p.is_file()}


def settings():
    p = OUT / 'workshop.json'
    if not p.exists():
        p.write_text(json.dumps(dict(weapon=WEAPON, stem=SPEC['stem'], meshes=SPEC['meshes'],
            attachments=SPEC['attachments'], cast_sha256=sha(CAST)), indent=2), encoding='utf-8')
    data = json.loads(p.read_text())
    if data['weapon'] != WEAPON or data['cast_sha256'] != sha(CAST):
        raise ValueError('Arme ou export Cast incompatible ; aucune integration effectuee.')
    return data


def manifest_for(target):
    old = target / 'PACKAGE-MANIFEST.json'
    archive = target / 'REFERENCE-PACKAGE-MANIFEST.json'
    if old.exists() and not archive.exists():
        old.rename(archive)
    files = {str(p.relative_to(target)): sha(p) for p in target.rglob('*')
             if p.is_file() and p.name != 'PACKAGE-MANIFEST.json'}
    old.write_text(json.dumps(dict(format='KHARVOX_WORKSHOP_PACKAGE', weapon=WEAPON,
        game_validated=False, files=files), indent=2), encoding='utf-8')


def package(capture=False):
    if PROTECTED:
        raise ValueError('Arme validee : paquet protege, aucune modification.')
    if capture:
        raise ValueError('Capture automatique retiree du workflow.')
    for name in ('KharvoxLayer.dll', 'native_sfs_build.txt'):
        if not (ROOT / 'build/runtime-msvc' / name).is_file():
            raise FileNotFoundError('Paquet jouable bloque : ' + name
                + ' absent du build. Verifier notamment l historique de protection Windows. Aucun binaire de remplacement utilise.')
    target = OUT / ('calibration-test' if capture else 'test')
    # A data-only stage may already exist. Fill missing runtime files while
    # retaining test settings; generated profiles/assets are refreshed below.
    for source in REFERENCE.rglob('*'):
        if not source.is_file():
            continue
        destination = target / source.relative_to(REFERENCE)
        if not destination.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
    for name in ('KharvoxLayer.dll', 'native_sfs_build.txt'):
        shutil.copy2(ROOT / 'build/runtime-msvc' / name, target / name)
    stamp = (target / 'native_sfs_build.txt').read_text().splitlines()
    if len(stamp) < 2 or stamp[1].lower() != sha(target / 'KharvoxLayer.dll'):
        raise ValueError('DLL et certificat SFS incompatibles.')
    manifest = json.loads((REFERENCE / 'KharvoxLayer.json').read_text(encoding='utf-8-sig'))
    manifest['layer']['library_path'] = str(target / 'KharvoxLayer.dll')
    (target / 'KharvoxLayer.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    data = settings()
    a, b = (data['attachments'][s] for s in ('left', 'right'))
    sys.path.insert(0, str(ROOT / '.tools/cast/libraries/python'))
    from cast import Cast, Model
    model = next(m for r in Cast.load(str(CAST)).Roots() for m in r.ChildrenOfType(Model))
    bones = model.Skeleton().Bones()
    for item in (a, b):
        if bones[item['index']].Name() != item['joint']:
            raise ValueError('Nom et indice de joint incoherents.')
    inherited = REFERENCE / 'hand_joint_bindings.cfg'
    lines = inherited.read_text().splitlines() if inherited.is_file() else []
    lines = [line for line in lines if line.split()[1] != WEAPON]
    lines.append(f"1 {WEAPON} {len(bones)} {a['index']} {a['joint']} "
        f"{b['index']} {b['joint']} {sha(CAST)} 0")
    (target / 'hand_joint_bindings.cfg').write_text('\n'.join(lines)+'\n', encoding='utf-8')
    manifest_for(target)
    return target


def launch(path):
    subprocess.Popen([str(path / 'KharvoxDesktop.exe')], cwd=path)


def run_blender(action):
    subprocess.run([str(BLENDER), '--background', '--factory-startup', '--python-exit-code', '1',
        '--python', str(SCRIPT), '--', action, '--weapon', WEAPON], check=True)


def verify_export():
    import importlib.util
    spec = importlib.util.spec_from_file_location('weapon_grip_checks', ROOT/'KHARVOX/tools/weapon_grip_checks.py')
    checks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checks)
    # Existing test settings, when present, are the actual runtime calibration.
    calibration = OUT/'test' if (OUT/'test/hand_models.cfg').is_file() else REFERENCE
    aim_path = None
    if WEAPON == 'pistol':
        aim_path = OUT/'export/two_hand_weapon_profiles_default.cfg'
        inherited = (calibration/'two_hand_weapon_profiles_default.cfg').read_text(encoding='utf-8-sig').splitlines()
        lines = [line for line in inherited if len(line.split()) < 2 or line.split()[1] != WEAPON]
        lines.append(checks.pistol_aim_profile(OUT, calibration))
        aim_path.write_text('\n'.join(lines)+'\n', encoding='utf-8')
    result = checks.check_export(OUT, calibration, aim_profiles_path=aim_path)
    (OUT/'export/grip-check.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    if not result['passed']:
        raise ValueError('Controle automatique des prises : '+' '.join(result['errors']))
    # Reuse the native regression including the captured heavy-rifle reversal.
    regression = ROOT/'build/runtime-msvc/KharvoxWeaponSupportPolicyTests.exe'
    if not regression.is_file():
        raise ValueError('Test natif de non-regression des prises indisponible ; paquet bloque.')
    subprocess.run([str(regression)], check=True)
    return result


def main(action):
    if PROTECTED and action != 'open':
        raise ValueError('Arme validee : atelier en consultation uniquement.')
    OUT.mkdir(parents=True, exist_ok=True)
    originals = baseline()
    settings()
    if action == 'open':
        if not (OUT / 'atelier.blend').exists():
            run_blender('build')
        subprocess.Popen([str(BLENDER), str(OUT / 'atelier.blend'), '--python', str(SCRIPT), '--', 'ui', '--weapon', WEAPON], cwd=ROOT)
    elif action == 'prepare':
        run_blender('export')
        verify_export()
        target = package()
        report = json.loads((OUT / 'export/report.json').read_text())
        for name in ('support_hands.cfg', 'primary_hands.cfg', 'grip-check.json'):
            shutil.copy2(OUT / 'export' / name, target / name)
        if WEAPON == 'pistol':
            shutil.copy2(OUT/'export/two_hand_weapon_profiles_default.cfg', target/'two_hand_weapon_profiles_default.cfg')
        for item in report['assets']:
            shutil.copy2(OUT / 'export' / item, target / item)
        (target / 'workshop-provenance.json').write_text(json.dumps(report, indent=2))
        manifest_for(target)
        print('Paquet pret :', target)
    elif action == 'launch':
        target = OUT / 'test'
        report = json.loads((target / 'workshop-provenance.json').read_text())
        if report['pose_sha256'] != sha(OUT / 'pose.json'):
            raise ValueError('Pose modifiee : cliquer Preparer le test avant de lancer.')
        if report['scene_sha256'] != sha(OUT / 'atelier.blend'):
            raise ValueError('Scene modifiee : preparer de nouveau le test.')
        for item in report['hands'].values():
            if sha(target / item['asset']) != item['asset_sha256']:
                raise ValueError('Asset du paquet modifie : preparer de nouveau le test.')
        launch(target)
    else:
        raise ValueError('Commande inconnue.')
    if baseline() != originals:
        raise RuntimeError('La reference shotgun a change pendant cette operation.')


if __name__ == '__main__':
    try:
        main(sys.argv[1] if len(sys.argv) > 1 else 'open')
    except Exception as error:
        print('ATELIER BLOQUE :', error, flush=True)
        sys.exit(1)
