# Reprendre le travail sur DOOM VR PLUS

État : 16 novembre 2026. Fork de CactusVRStudios/KHARVOX, base locale upstream
`76d728e`. Développement **AI-assisted avec OpenAI Codex**, réglages artistiques
et validation en jeu par le mainteneur.

## Ce qui est terminé

- Poses Praetor ajustées dans Blender pour dix armes : pistolet, combat shotgun,
  Super Shotgun, heavy assault rifle, plasma rifle, rocket launcher, Gauss,
  chaingun, BFG 9000 et chainsaw. Validation humaine de la prise à deux mains.
- Réutilisation de la pose droite de poignée en prise à une main, puis dans
  les transitions une/deux mains. Validation humaine reçue.
- Crouch physique, recentrage deux sticks et préservation des commandes.
- Correction des ralentissements signalés par comparaison au mod d'origine.
- HUD suspendu, orienté et espacé manuellement dans Blender ; dernier placement
  testé et validé en jeu. Ne pas réappliquer l'ancien espacement automatique.
- Grenade fragmentaire texturée tenue uniquement en main gauche, doigts figés
  selon la pose validée ; main droite conserve l'arme. Validation visuelle reçue.
- Launcher Avalonia avec cadre GIF animé, français/anglais immédiat et
  mémorisé, résumé des ajouts et version compilée visible (`1.4.1`).

## Ce qui reste à faire — uniquement sur nouvelle demande

1. **Grenade :** le modèle tenu est distinct du projectile. Raccorder la
   position, la vitesse et le déclenchement natifs au relâchement sans contourner
   charges/cooldowns. L'origine et le délai actuels ne sont pas corrigés.
   L'aperçu ne lit pas encore le type d'équipement ni le cooldown.
2. **Éclairage :** le renderer GLB utilise son éclairage PBR actuel, pas les
   lumières dynamiques natives de DOOM. La réutilisation des bras natifs des
   interactions reste une étude ; aucune intégration native démontrée.
3. **Distribution complète :** les ressources extraites du jeu ne sont pas
   publiées. Une génération locale et ses conditions de redistribution doivent
   être établies avant de proposer un paquet Praetor complet au public.

## Points d'entrée

| Sujet | Fichiers |
|---|---|
| Renderer et pose des mains | `src/hands/HandRenderer.cpp`, `WeaponSupportConfig.h`, `WeaponSupportPolicy.h`, `HandPoseMath.h` |
| Rattachements et arme | `src/weapon/WeaponHook.cpp` |
| Suivi/input/activation | `src/openxr/OpenXRBootstrap.cpp` |
| Crouch/recentrage | `PhysicalCrouchPolicy.h`, `StickRecenterPolicy.h`, `src/camera/BodyStanceHeight.h` |
| Grenade | `src/openxr/GrenadeThrowPolicy.h`, `src/weapon/GrenadeThrowAim.h`, `src/hands/GrenadeVisualPolicy.h` |
| HUD | `src/hud/HudHook.cpp`, `ForearmHudPolicy.h`, `WristHudGesturePolicy.h` |
| Calibration/export des mains | `tools/hand_workshop.py`, `hand_workshop_blender.py`, `verify_native_skeleton_conversion.py`, `weapon_grip_checks.py` |
| Atelier/export HUD | `tools/hud_floating_preview.py`, `export_forearm_hud.py`, `audit_hud_scale_chain.py` |
| Atelier/export grenade | `tools/grenade_workshop_blender.py`, `export_grenade_pose.py` |
| Launcher | `launcher/KharvoxDesktop/` ; backend `launcher/KharvoxLaunchHost/` |

Les shaders `HandPbr.*` restent ceux du renderer actuel. Le code de provenance
GPU préexistant est expérimental et désactivé par défaut. Ne pas le confondre
avec une solution validée d'occlusion ou d'éclairage.

## Ressources et repères

Les modèles Praetor, leurs textures, les scènes Blender et les poses artistiques
qui embarquent des données natives restent locaux. Les profils de référence
ne remplacent pas les meshes ni les poses de doigts baked dans leurs GLB.
Pour reprendre ce travail précis, demander les fichiers autorisés au mainteneur
et les préparer localement ; ne pas les deviner à partir d'une capture.

Le workflow mains repose sur des joints natifs vérifiés, des matrices locales
par rapport à l'arme, les bons parents d'animation et une échelle par modèle.
Pour le shotgun : soutien lié à `pump`, poignée liée à `base`.
Le pivot d'animation n'est pas forcément le point d'accroche physique.

Le HUD utilise `HUD_anchor_left`, lié à `LeftForeArmRoll2`. L'export conserve
les matrices ancrage-locales, en centimètres ; XR droite/haut/arrière devient
DOOM avant/gauche/haut. Ne pas appliquer deux fois la calibration de main libre.
Les canvases natifs vie/munitions font 512 × 300 ; la hauteur Options est
dynamique. Les pixels SWF natifs ne sont pas identiques à la maquette Blender.

## Compiler et vérifier

Windows, SDK .NET 10 pour le launcher ; CMake, MSVC ou compilateur C++ compatible
et Ninja pour les tests. Le projet de base et ses notices décrivent les
dépendances natives. Les dépendances SFS et fixtures de jeu restent optionnelles.

```powershell
dotnet publish launcher/KharvoxDesktop/KharvoxDesktop.csproj -c Release -r win-x64 --self-contained true -o build/launcher
```

Pour l'interface sans jeu :

```powershell
build/launcher/KharvoxDesktop.exe --preview
build/launcher/KharvoxDesktop.exe --screenshot build/launcher-fr.png fr
build/launcher/KharvoxDesktop.exe --screenshot build/launcher-en.png en
```

`--self-test` nécessite un hôte de lancement compatible dans le paquet, mais ne
lance pas DOOM. Pour CMake/CTest, suivre le README upstream et limiter les
fixtures aux ressources réellement disponibles. Ne pas déclarer les tests
avec modèles natifs réussis dans un clone public qui ne les possède pas.

La dernière suite locale complète comptait 158 tests CTest. Le dernier launcher
passe dix groupes de self-tests, ainsi que les quatre pages en français et en
anglais. Ces résultats concernent la configuration locale testée, pas toutes
les machines. Les contrôleurs de validation incluent Quest/Touch.

## Contrôle de la copie publique avant publication

La copie nettoyée a été compilée avec `KHARVOX_BUILD_LAYER=OFF` et passe
123 tests CTest, sans les fixtures Praetor locales. Le launcher nettoyé est
compilé séparément et ses pages sont contrôlées dans les deux langues.
Les captures du README ont un champ de chemin d'installation vide.

`config/praetor-reference/` conserve les matrices et paramètres validés,
sans distribuer les meshes/textures. Ces profils ne s'installent pas par défaut.
Les ateliers historiques attendent encore une organisation de workspace avec
les exports, les références et les outils locaux ; examiner leurs paramètres
avant de les exécuter dans un clone renommé. Le code ne fournit pas encore un
installateur public automatique de ces ressources.
