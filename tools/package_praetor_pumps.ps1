param([switch]$Build,[switch]$Validate)
$ErrorActionPreference='Stop'
$workspace=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$baseline=Join-Path $workspace 'artifacts/KHARVOX-VR-Valide'
$native=Join-Path $workspace 'build/runtime-msvc'
$review=Join-Path $workspace 'build/pump-review-v3'
$fixture=Join-Path $review 'runtime-fixture'
$package=Join-Path $workspace 'build/legacy-packages/KHARVOX-VR-Pompes-Praetor-test'
$archive=$package+'.zip'
$pathsFile=Join-Path $review 'package-paths.txt'
$expectedDll='CD044BFCE497A28C33DC77DEDDB7E55719AFFC79626015153486334104D8B701'
if((Test-Path -LiteralPath $archive) -or ((Test-Path -LiteralPath $package) -and !$Validate)){throw 'Output already exists; preserve previous packages'}
if($Validate -and !(Test-Path -LiteralPath $package -PathType Container)){throw 'No package to validate'}
if((Get-FileHash -LiteralPath (Join-Path $native 'KharvoxLayer.dll')).Hash -ne $expectedDll){throw 'DLL differs from the tested build'}
$items=@{}
$generated=@{}
foreach($file in Get-ChildItem -LiteralPath $baseline -File -Recurse){
    $relative=[IO.Path]::GetRelativePath($baseline,$file.FullName)
    if($relative -in @('renderer_status.txt','two_hand_status.txt','hand_calibration_status.txt','fsr1_status.txt')){continue}
    $items[$relative]=$file.FullName
}
foreach($file in Get-ChildItem -LiteralPath $fixture -File -Recurse){
    $relative=[IO.Path]::GetRelativePath($fixture,$file.FullName)
    if($relative -eq 'provenance.json'){$relative='support-provenance.json'}
    $items[$relative]=$file.FullName
}
foreach($name in @('KharvoxLayer.dll','native_sfs_build.txt')){$items[$name]=Join-Path $native $name}
foreach($relative in @($items.Keys | Where-Object {$_ -like 'tests\*.exe'})){
    $replacement=Join-Path $native ([IO.Path]::GetFileName($relative))
    if(!(Test-Path -LiteralPath $replacement -PathType Leaf)){throw "Missing updated test: $relative"}
    $items[$relative]=$replacement
}
$items['tests\KharvoxHudLayoutPolicyTests.exe']=Join-Path $native 'KharvoxHudLayoutPolicyTests.exe'
$layer=Get-Content -LiteralPath (Join-Path $baseline 'KharvoxLayer.json') -Raw | ConvertFrom-Json
$layer.layer.library_path='.\KharvoxLayer.dll'
$generated['KharvoxLayer.json']=($layer | ConvertTo-Json -Depth 8)+"`n"
$generated['README-CORRECTIFS.txt']=@'
KHARVOX — TEST DES DEUX POMPES PRAETOR — 14 novembre 2026

1. Ouvrir KharvoxDesktop.exe dans CE dossier, puis cliquer sur LAUNCH GAME.
2. Le chemin du jeu et les réglages du launcher précédent sont repris.
3. Garder tous les fichiers ensemble. Depuis le ZIP : extraire tout le dossier avant de lancer.

Ce paquet contient la nouvelle DLL et les prises Blender validées pour le fusil
à pompe et le Super Shotgun. La taille du gant reste physique au verrouillage.
Le HUD natif est tourné de 180 degrés et suit le poignet affiché, avec un
dégagement minimal calculé autour de l'armure. Les doigts restent animés en
main libre et fixes au soutien. Les autres armes conservent leur repli calibré.

Le crouch physique, le lancer gestuel existant, le recentrage et les commandes
classiques restent disponibles. Les modèles natifs de grenade feront l'objet
d'un travail séparé.

À TESTER AVEC CETTE VERSION :
- Fusil à pompe puis Super Shotgun : main libre / Grip verrouillé / relâchement.
- Taille identique du gant, placement sous l'arme et doigts en prise.
- HUD lisible comme une montre et dégagé du skin, libre puis verrouillé.
- SFS/AER et mode gaucher, selon les modes que tu utilises.

Tu n'as pas besoin de retourner à l'ancienne version pour ces essais.
F4 peut capturer les transformations pendant le test. Après avoir quitté DOOM,
le journal se trouve dans %TEMP%\KHARVOX-WEAPON-BONE-SNAPSHOTS.log et le journal
général dans %TEMP%\KHARVOX.log. Quatre F4, une par état des deux pompes, aident
à analyser un éventuel écart. Elles ne sont pas un préalable au lancement.

Compilation Release et 153/153 CTest réussis. Les essais en jeu permettront de
confirmer le raccord aux animations et aux transformations natives : il n'est
pas présenté comme déjà validé au casque. Les tests livrés se lancent avec
tests\run-tests.cmd. Détails : POMPES-TEST.md et VALIDATION.txt.

Les documents PERFORMANCE.md, PRAETOR-MAINS.md et CORRECTIFS-VR.md décrivent les
étapes précédentes ; README-CORRECTIFS.txt et POMPES-TEST.md décrivent ce paquet.
'@
$generated['POMPES-TEST.md']=@'
# Test jouable : deux pompes Praetor

Lancer **KharvoxDesktop.exe**, puis **LAUNCH GAME**. Ce launcher charge la
nouvelle `KharvoxLayer.dll` du même dossier. Le launcher Avalonia et le profil
portable sont repris ; la DLL, le manifeste SFS, les profils et les tests sont
mis à jour ensemble.

Les trois points à évaluer sont la taille constante de la main de soutien,
les deux prises acceptées dans Blender, puis le sens et le dégagement du HUD.
Tester libre, Grip maintenu puis relâché pour chaque pompe. Les variantes et
animations locales de l'arme ainsi que l'occlusion réelle restent à confirmer.
Les autres armes restent disponibles avec leur calibration de repli ; leurs
nouvelles prises ne sont pas annoncées comme terminées.

Les profils de soutien v4 sont liés aux empreintes des vrais modèles FIST.
La normalisation conserve le point d'appui natif et l'échelle physique du gant.
Les correspondances de poignets permettent au HUD verrouillé de suivre la
main affichée. Le canevas et les graphismes natifs sont réutilisés, avec une
rotation de 180 degrés sans miroir. Le dégagement vise au moins 3,5 cm devant
une enveloppe conservatrice de l'armure du côté de l'œil central partagé.

Faits vérifiés hors jeu : 153 CTest passent ; quatre modèles exportés sans
intersection des triangles sélectionnés ; rendu GPU des deux pompes, des deux
mains et des deux yeux, intégré/overlay ; doigts libres et figés ; taille
stable lorsque l'échelle native de l'arme varie. Les tests de ce dossier et
le contrôle du launcher sont exécutés avant la livraison.

Mesure locale RTX 3080 : deux yeux 1536×1536, quatre mains et copies de
profondeur, moyenne 0,525 ms, maximum 0,538 ms, aucune allocation après chauffe,
trois textures partagées. Cette mesure exclut le jeu et le compositeur ; elle
ne constitue pas une mesure de fluidité VR en jeu.

L'échelle native effective de l'arme, `modelScale`, `viewOffset`, les animations
et les transitions doivent être évalués avec ce paquet. F4 et les journaux
dans `%TEMP%` permettront d'expliquer les écarts éventuels. Les captures de
l'ancienne version ne sont pas nécessaires avant de tester cette version.

Pour revenir au paquet précédent, lancer son propre KharvoxDesktop.exe.
Les deux dossiers sont séparés. Ne pas mélanger leurs DLL et configurations.
'@
$generated['PRAETOR-TEST.md']="# Test Praetor actuel`n`nCe paquet cible les deux pompes et le HUD. Voir [POMPES-TEST.md](POMPES-TEST.md) et README-CORRECTIFS.txt pour les changements et les essais.`n"
$generated['VALIDATION.txt']=@"
KHARVOX — POMPES PRAETOR — TEST JOUABLE
DLL SHA256 : $expectedDll
CMake Release : réussi
CTest : 153/153 réussis (17,35 s)
Export : 4 GLB statiques, 0 paire de triangles en intersection
Profils : fusil à pompe et Super Shotgun ; mains de soutien gauche/droite
HUD : axes de pixels tournés de 180 degrés ; enveloppes libres/verrouillées
GPU : 0,525363 ms en moyenne, 0 allocation après chauffe, 3 textures partagées
Validation en jeu : à effectuer avec CE paquet ; non déclarée déjà réussie
Les tests embarqués, le launcher et les intégrations sont vérifiés par le script de paquetage.
Le détail des empreintes des fichiers livrés figure dans PACKAGE-MANIFEST.json.
"@
$generated['tests\run-tests.cmd']=@'
@echo off
setlocal
set "KHARVOX_TEST_DIRECTORY=%~dp0"
pushd "%KHARVOX_TEST_DIRECTORY%.."
"%KHARVOX_TEST_DIRECTORY%KharvoxHandAssetTests.exe" "%CD%\assets\3D"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxHandInteractionTests.exe" "%CD%\two_hand_weapon_profiles_default.cfg" "%CD%\assets\3D"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxHandVisibilityPolicyTests.exe"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxGrenadeThrowTests.exe"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxPhysicalCrouchTests.exe"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxStickRecenterTests.exe"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxWeaponSupportPolicyTests.exe"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxWeaponSupportConfigTests.exe" "%CD%\support_hands.cfg" 2
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxHudLayoutPolicyTests.exe" "%CD%\offhand_hud.cfg"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxHandRenderGpuTests.exe" "%CD%" --expect-shared-atlas --expect-two-pump-profiles
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxRoomscaleLocomotionTests.exe"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxHandPerformanceComparisonTests.exe"
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxPerformanceCpuCountersTests.exe" --enabled
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxPerformanceCpuCountersTests.exe" --disabled
if errorlevel 1 goto failed
"%KHARVOX_TEST_DIRECTORY%KharvoxTextureDiagnosticsTests.exe"
if errorlevel 1 goto failed
echo PASS: all packaged regression tests completed.
popd
if /i not "%~1"=="--no-pause" pause
exit /b 0
:failed
echo FAIL: a packaged regression test failed.
popd
if /i not "%~1"=="--no-pause" pause
exit /b 1
'@
foreach($relative in @($generated.Keys)){$items.Remove($relative)}
$relatives=@(@($items.Keys)+@($generated.Keys)+@('PACKAGE-MANIFEST.json') | Sort-Object -Unique)
$planned=@($relatives | ForEach-Object {[IO.Path]::GetFullPath((Join-Path $package $_))})+@($archive)
foreach($path in $planned){
    if(!$path.StartsWith($workspace.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Output escaped workspace'}
}
if(!$Build -and !$Validate){
    [IO.File]::WriteAllLines($pathsFile,$planned,[Text.UTF8Encoding]::new($false))
    Write-Output "PLAN ONLY: $($relatives.Count) package files and one ZIP. No package files created."
    Write-Output "Exact destinations: $pathsFile"
    exit 0
}
if(!(Test-Path -LiteralPath $pathsFile)){throw 'Generate the exact path plan before writing the package'}
if(Compare-Object (Get-Content -LiteralPath $pathsFile) $planned){throw 'Package paths changed since the announced plan'}
if(!$Validate){
    New-Item -ItemType Directory -Path $package | Out-Null
    foreach($relative in $items.Keys){
        $target=Join-Path $package $relative
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $items[$relative] -Destination $target
    }
    foreach($relative in $generated.Keys){
        $target=Join-Path $package $relative
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        [IO.File]::WriteAllText($target,$generated[$relative],[Text.UTF8Encoding]::new($false))
    }
}
if((Get-FileHash -LiteralPath (Join-Path $package 'KharvoxLayer.dll')).Hash -ne $expectedDll){throw 'Packaged DLL differs from the tested build'}
& (Join-Path $PSScriptRoot 'verify_package_integrations.ps1') -Package $package
$launcherLog=Join-Path $review 'package-launcher-selftest.log'
$launcher=Start-Process -FilePath (Join-Path $package 'KharvoxDesktop.exe') -ArgumentList @('--self-test','--log',$launcherLog) -WindowStyle Hidden -Wait -PassThru
if($launcher.ExitCode -ne 0){throw 'Packaged Avalonia launcher test failed'}
Get-Content -LiteralPath $launcherLog
& (Join-Path $package 'tests/run-tests.cmd') --no-pause
if($LASTEXITCODE -ne 0){throw 'Packaged regression tests failed'}
$stamp=Get-Content -LiteralPath (Join-Path $package 'native_sfs_build.txt')
if($stamp.Count -ne 2 -or $stamp[1] -ne $expectedDll){throw 'SFS stamp mismatch'}
$hashes=[ordered]@{}
foreach($relative in $relatives){if($relative -ne 'PACKAGE-MANIFEST.json'){
    $hashes[$relative]=(Get-FileHash -LiteralPath (Join-Path $package $relative)).Hash
}}
$manifest=[ordered]@{
    package='KHARVOX-VR-Pompes-Praetor-test';date='2026-11-14';dllSha256=$expectedDll
    cmakeTests='153/153 passed';packagedTests='passed';launcherSelfTest='passed'
    inGameValidation='pending user test of this new package';files=$hashes
}
[IO.File]::WriteAllText((Join-Path $package 'PACKAGE-MANIFEST.json'),($manifest | ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
# Archive the ready-to-launch directory with a top-level folder, so users can
# extract it without mixing its DLL/configuration with another test package.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($package,$archive,[IO.Compression.CompressionLevel]::Optimal,$true)
$zip=[IO.Compression.ZipFile]::OpenRead($archive)
try{
    if($zip.Entries.Count -ne $relatives.Count){throw 'ZIP entry count mismatch'}
    foreach($entry in $zip.Entries){
        $relative=$entry.FullName.Substring((Split-Path $package -Leaf).Length+1).Replace([char]47,[char]92)
        if($relative -eq 'PACKAGE-MANIFEST.json'){continue}
        $stream=$entry.Open();$sha=[Security.Cryptography.SHA256]::Create()
        try{$hash=[Convert]::ToHexString($sha.ComputeHash($stream))}finally{$stream.Dispose();$sha.Dispose()}
        if($hash -ne $hashes[$relative]){throw "ZIP payload differs: $relative"}
    }
}finally{$zip.Dispose()}
Write-Output "PASS: $($relatives.Count) package files, launcher, native integrations, packaged tests and ZIP payload hashes verified"
Write-Output "Launcher: $(Join-Path $package 'KharvoxDesktop.exe')"
Write-Output "Archive: $archive"
