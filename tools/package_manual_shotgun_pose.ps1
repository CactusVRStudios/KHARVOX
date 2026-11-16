param([switch]$PlanOnly,[switch]$Refresh,[switch]$Validate)
$ErrorActionPreference='Stop'
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$baseline=Join-Path $workspace 'artifacts/KHARVOX-VR-Valide'
$native=Join-Path $workspace 'build/runtime-msvc'
$export=Join-Path $workspace 'build/manual-shotgun-pose'
$package=Join-Path $workspace 'build/legacy-packages/KHARVOX-VR-Shotgun-Pose-Manuelle'
$paths=Join-Path $export 'package-paths.txt'
$report=Get-Content -LiteralPath (Join-Path $export 'rapport.json') -Raw | ConvertFrom-Json
if($Validate){
    $manifestPath=Join-Path $package 'PACKAGE-MANIFEST.json'
    $manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $checked=0
    foreach($entry in $manifest.files.PSObject.Properties){
        if((Get-FileHash -LiteralPath (Join-Path $package $entry.Name)).Hash -ne $entry.Value){
            throw "Packaged file identity mismatch: $($entry.Name)"
        }
        ++$checked
    }
    foreach($entry in $report.original_package_sha256.PSObject.Properties){
        if((Get-FileHash -LiteralPath (Join-Path $baseline $entry.Name)).Hash -ne $entry.Value){
            throw "Original package changed: $($entry.Name)"
        }
    }
    $ctest=Get-Content -LiteralPath (Join-Path $export 'ctest.log') -Raw
    $tests=Get-Content -LiteralPath (Join-Path $export 'packaged-tests.log') -Raw
    if($ctest -notmatch '100% tests passed, 0 tests failed out of 153'){throw 'CTest did not pass'}
    if(($tests -notmatch 'PASS: manual shotgun pose regression tests completed.') -or
        ($tests -notmatch 'Manual shotgun primary: both eyes') -or
        ($tests -notmatch 'Shared hand textures: created=3 destroyed=3')){throw 'Packaged GPU tests did not pass'}
    $layer=Get-Content -LiteralPath (Join-Path $package 'KharvoxLayer.json') -Raw | ConvertFrom-Json
    $layerTarget=[IO.Path]::GetFullPath((Join-Path $package $layer.layer.library_path))
    if($layerTarget -ne (Join-Path $package 'KharvoxLayer.dll')){throw 'Launcher manifest points outside the new package'}
    $stamp=Get-Content -LiteralPath (Join-Path $package 'native_sfs_build.txt') -Raw
    if($stamp -notmatch $manifest.dllSha256){throw 'DLL/SFS identity pair mismatch'}
    if((Get-FileHash -LiteralPath (Join-Path $package 'shotgun_pose.json')).Hash -ne $report.source_json_sha256){
        throw 'Packaged pose differs from the frozen user input'
    }
    $validation=[ordered]@{status='PASSED_OFFLINE_PENDING_HEADSET_TEST';checkedFiles=$checked;
        ctest='153/153 passed';packagedRegressionTests='passed';
        primaryGpu='both eyes, overlay and scene depth; frozen fingers; invalid-frame fallback; other weapons and handedness preserved';
        atlasImages='3 created / 3 destroyed, no texture or memory leak';
        dllSha256=$manifest.dllSha256;sourcePoseSha256=$manifest.sourcePoseSha256;
        originalPackageUnchanged=$true;hudAndMenuConfigurationsUnchanged=$true;
        sourceFiles=@('src/hands/HandRenderer.cpp','src/hands/WeaponSupportPolicy.h',
            'tests/weapon_support_policy_tests.cpp','tests/hand_render_gpu_tests.cpp',
            'tools/export_manual_shotgun_pose.py','tools/package_manual_shotgun_pose.ps1');
        confirmed=@('Saved JSON and scene agree below 0.001 mm',
            'Export replay verified against 17 native weapon matrices',
            'Hand scale is independent of weapon scale',
            'Primary and support meshes are static and share existing texture atlases');
        pending=@('User headset test of standard shotgun Grip and release',
            'SFS/AER in-game occlusion, variants, recoil and animation continuity',
            'No other weapons generalized before user validation')}
    [IO.File]::WriteAllText((Join-Path $export 'package-validation.json'),
        ($validation | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    $validationText="KHARVOX - POSE MANUELLE SHOTGUN`n`nCompilation Release reussie.`n"+
        "CTest : 153/153. Tests du paquet et rendu GPU des deux mains : reussis.`n"+
        "Deux yeux, overlay et scene depth ; doigts figes au Grip ; repli et mode gaucher testes.`n"+
        "Trois atlas partages ; aucune fuite GPU detectee.`nDLL SHA256 : $($manifest.dllSha256)`n"+
        "Pose source SHA256 : $($manifest.sourcePoseSha256)`n"+
        "Ancien paquet, HUD et menus conserves. Validation casque encore requise.`n"
    [IO.File]::WriteAllText((Join-Path $package 'VALIDATION.txt'),$validationText,[Text.UTF8Encoding]::new($false))
    $manifest.files.'VALIDATION.txt'=(Get-FileHash -LiteralPath (Join-Path $package 'VALIDATION.txt')).Hash
    $manifest.validation='153/153 CTest + packaged regression and manual primary GPU tests passed'
    [IO.File]::WriteAllText($manifestPath,($manifest | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    Write-Output "Validated $checked file hashes; DLL/SFS and launcher target correct; 153/153 CTest and packaged tests passed"
    exit 0
}
$items=@{}
foreach($file in Get-ChildItem -LiteralPath $baseline -File -Recurse){
    $relative=[IO.Path]::GetRelativePath($baseline,$file.FullName)
    $items[$relative]=$file.FullName
}
foreach($relative in @('support_hands.cfg','primary_hands.cfg',
        'assets/3D/SUPPORT_SHOTGUN_LEFT.glb','assets/3D/PRIMARY_SHOTGUN_RIGHT.glb')){
    $items[$relative.Replace('/',[IO.Path]::DirectorySeparatorChar)]=Join-Path $export $relative
}
$items['shotgun_pose.json']=Join-Path $export 'pose-source.json'
$items['manual-pose-provenance.json']=Join-Path $export 'rapport.json'
foreach($relative in @('KharvoxLayer.dll','native_sfs_build.txt')){$items[$relative]=Join-Path $native $relative}
foreach($name in @('KharvoxHandRenderGpuTests.exe','KharvoxWeaponSupportPolicyTests.exe')){
    $items['tests\'+$name]=Join-Path $native $name
}
$generated=@{}
$layer=Get-Content -LiteralPath (Join-Path $baseline 'KharvoxLayer.json') -Raw | ConvertFrom-Json
$layer.layer.library_path='.\KharvoxLayer.dll'
$generated['KharvoxLayer.json']=($layer | ConvertTo-Json -Depth 8)+"`n"
$generated['README-CORRECTIFS.txt']=@'
KHARVOX - TEST DE TA POSE MANUELLE - FUSIL A POMPE STANDARD

1. Ouvrir KharvoxDesktop.exe dans CE dossier puis LAUNCH GAME.
2. Prendre le fusil a pompe standard et verrouiller le Grip de soutien.
3. Les deux mains et les doigts reprennent la pose que tu as enregistree dans Blender.
4. Relacher le Grip retablit les chemins precedents, dont les doigts libres.

Ce dossier contient une DLL de test separee et ses fichiers SFS correspondants.
Le launcher, le HUD, les menus et leurs reglages restent ceux du paquet precedent.
Les autres armes et le mode gaucher conservent leurs poses precedentes.
La nouvelle pose a deux mains est limitee au shotgun en mode droitier.

L'export conserve ta pose sans ajustement automatique ni collision.
Les atlas sont partages et les deux maillages sont charges au demarrage.
Aucun skinning CPU ni calcul d'ajustement n'est ajoute pendant le jeu.

A verifier au casque : placement pendant le Grip, taille des mains, relachement,
tir et rechargement. Tester SFS/AER selon les modes que tu utilises.
Le raccord mathematique est teste hors jeu ; le resultat casque reste a valider.

Les fichiers Blender editables et l'ancien paquet restent dans leurs dossiers.
shotgun_pose.json dans CE paquet est la copie figee utilisee pour cet export.
Les documents sur les precedentes versions sont conserves a titre d'historique.
Les tests de ce paquet se lancent avec tests\run-manual-shotgun.cmd.
'@
$generated['tests\run-manual-shotgun.cmd']=@'
@echo off
setlocal
call "%~dp0run-tests.cmd" --no-pause
if errorlevel 1 goto failed
pushd "%~dp0.."
"%~dp0KharvoxWeaponSupportConfigTests.exe" "%CD%\support_hands.cfg" 2
if errorlevel 1 goto failed_inside
"%~dp0KharvoxHandRenderGpuTests.exe" "%CD%" --expect-shared-atlas --expect-two-pump-profiles --expect-manual-primary
if errorlevel 1 goto failed_inside
popd
echo PASS: manual shotgun pose regression tests completed.
if /i not "%~1"=="--no-pause" pause
exit /b 0
:failed_inside
popd
:failed
echo FAIL: a manual shotgun regression test failed.
if /i not "%~1"=="--no-pause" pause
exit /b 1
'@
$generated['MANUAL-SHOTGUN-TEST.md']=@'
# Test de la pose manuelle

Les deux mains natives sont cuites depuis le JSON et la scene utilisateur du 15 novembre.
Le JSON correspond a la scene a moins de 0,001 mm. La geometrie exportee est relue et
verifiee avec les 17 matrices d'arme natives disponibles, sans ajustement automatique.

La main de soutien reutilise support_hands.cfg. primary_hands.cfg ajoute une pose
statique pour la main physique droite, uniquement pendant le verrouillage a deux mains.
Absence du profil, mauvais hash ou repere invalide : repli sur le rendu calibre existant.
Le relachement, les autres armes et le mode gaucher gardent les chemins precedents.

Les atlas et shaders existants sont reutilises. Le code et la configuration du HUD
ne sont pas modifies. Le suivi de poignet existant reste applicable a la main deplacee.
La DLL/SFS et les tests sont regroupes dans ce paquet ; aucun fichier de jeu installe
ni fichier du paquet precedent n'est remplace.

Ce controle hors jeu ne valide pas l'occlusion, les variantes d'arme, les animations,
le confort ou les performances de la scene complete au casque. Le test en jeu reste requis.
'@
$planned=@($items.Keys | ForEach-Object {Join-Path $package $_})
$planned+=@($generated.Keys | ForEach-Object {Join-Path $package $_})
$planned+=Join-Path $package 'PACKAGE-MANIFEST.json'
$planned | Sort-Object -Unique | Set-Content -LiteralPath $paths -Encoding UTF8
if($PlanOnly){Write-Output "Planned exact paths: $paths ($($planned.Count) entries)";exit 0}
if((Test-Path -LiteralPath $package) -and !$Refresh){throw 'New test package already exists; use -Refresh only for this generated test package'}
$dllHash=(Get-FileHash -LiteralPath (Join-Path $native 'KharvoxLayer.dll')).Hash
$sfs=Get-Content -LiteralPath (Join-Path $native 'native_sfs_build.txt') -Raw
if($sfs -notmatch $dllHash){throw 'Native SFS stamp does not match the new DLL'}
foreach($name in $report.original_package_sha256.PSObject.Properties.Name){
    if((Get-FileHash -LiteralPath (Join-Path $baseline $name)).Hash -ne $report.original_package_sha256.$name){
        throw "Original package changed during export: $name"
    }
}
foreach($relative in $items.Keys){
    $target=Join-Path $package $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $items[$relative] -Destination $target -Force
}
foreach($relative in $generated.Keys){
    $target=Join-Path $package $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    [IO.File]::WriteAllText($target,$generated[$relative],[Text.UTF8Encoding]::new($false))
}
# Verify the unchanged native HUD/menu inputs and all untouched support poses.
foreach($relative in $items.Keys){
    if($relative -match '\.cfg$' -and $relative -notin @('support_hands.cfg','primary_hands.cfg')){
        if((Get-FileHash -LiteralPath (Join-Path $package $relative)).Hash -ne
           (Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash){throw "Unexpected configuration change: $relative"}
    }
}
$hashes=[ordered]@{}
foreach($file in Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName){
    $relative=[IO.Path]::GetRelativePath($package,$file.FullName)
    if($relative -ne 'PACKAGE-MANIFEST.json'){$hashes[$relative]=(Get-FileHash -LiteralPath $file.FullName).Hash}
}
$manifest=[ordered]@{package='KHARVOX-VR-Shotgun-Pose-Manuelle';date='2026-11-15';
    dllSha256=$dllHash;sourcePoseSha256=$report.source_json_sha256;files=$hashes;
    scope='shotgun, right-handed two-hand Grip only';
    validation='Compilation complete; see build/manual-shotgun-pose test logs for current results';
    inGameValidation='Pending user headset test'}
[IO.File]::WriteAllText((Join-Path $package 'PACKAGE-MANIFEST.json'),
    ($manifest | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
Write-Output "Created isolated test package: $package"
Write-Output "DLL SHA256: $dllHash"
