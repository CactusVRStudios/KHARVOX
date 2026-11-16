# KHARVOX — correctifs VR, 14 novembre 2026

## Version Praetor — mains natives converties

Nouveau paquet jouable : `../artifacts/KHARVOX-VR-Praetor/KharvoxDesktop.exe`. Il remplace les anciennes mains par les modèles et textures Praetor de DOOM 2016 exportés localement, avec doigts animés et bas d’avant-bras. **143/143 tests CTest réussis**, dont trois tests sur les nouveaux assets et les contrôles de partage/libération des trois atlas GPU. Les anciennes versions restent conservées.

Voir [NATIVE_HANDS_STUDY.md](NATIVE_HANDS_STUDY.md) pour les ressources confirmées, la reconstruction des matériaux, la génération et les limites. Les avant-bras suivent rigidement les poignets ; lumières et spéculaires natifs ne sont pas intégralement reproduits. Le contact aux armes reste à vérifier au casque. La refonte de grenade native demeure inachevée.

## Suivi des retours du 14 novembre — correctif intermédiaire

Paquet `../artifacts/KHARVOX-VR-posture/` : 138/138 tests CTest réussis (24,14 s), dont rendu GPU et nouvelle politique de déplacement. Le suivi du casque pouvait injecter une commande de marche dès 2,5 mm de déplacement horizontal. Cette conversion est désormais désactivée par défaut ; le suivi visuel 6DoF et le déplacement au stick restent actifs. L'ancien comportement de déplacement physique devient un opt-in `KHARVOX_ROOMSCALE_LOCOMOTION=1`, avec seuil de 10 cm. Il reste à vérifier au casque que cela supprime le symptôme rapporté.

La pose sans pression utilise désormais une vraie pose de repos en C, construite sur les articulations du rig : interpolation locale depuis le repos vers le poing, de 40 % / 65 % / 40 % pour les trois phalanges et de 45 % / 40 % / 30 % pour le pouce. Ces valeurs décrivent les rotations du rig, pas un pourcentage de pression imposé aux commandes. Une entrée nulle sélectionne cette pose ; la gâchette et le grip ferment progressivement les doigts. La prise verrouillée reste figée. Les quatre GLB, leurs textures et la calibration du poignet restent conservés. Le PNG `../artifacts/main-gauche-reference.png` rend les vraies données livrées avec Blender ; ce n'est pas une capture du jeu. Le modèle du dépôt ne comporte pas la même armure d'avant-bras que la référence fournie. Validation après cette modification : 138/138 tests CTest réussis en 14,91 s, dont le rendu GPU.

**Grenade native : travail en cours, non livré comme terminé.** La représentation procédurale est désactivée pendant le gameplay. Le lancer existant envoie encore une impulsion d'équipement au relâchement et conserve l'animation du moteur ; il ne lance pas le même objet que l'aperçu. La demande d'orientation expire après une seconde, ce qui peut aussi expliquer son absence si l'événement natif arrive deux secondes après. Ce lien temporel est une hypothèse à mesurer, pas une cause confirmée par une trace.

Le nouveau paquet active le journal détaillé. Les lignes `[GRENADE-INPUT]` et `[GRENADE-NATIVE]` partagent l'horloge `tick_ms` et enregistrent la libération, le type de message et l'âge de la demande. Le désassemblage Steam situe la fonction appelante entre les RVA `0xD6B210` et `0xD6B9FD` ; son prologue utilise un sélecteur entier et un argument flottant. Son contexte de préparation, ses objets de rendu et son protocole de consommation ne sont pas établis : aucun appel direct à cette fonction n'est ajouté. L'utilisation du modèle/textures du jeu et d'un projectile réellement libéré depuis la main reste à implémenter après identification de ce contexte.

Pour mesurer : maintenir la gâchette de soutien deux secondes, relâcher avec un geste de lancer, attendre le projectile puis quitter le jeu. Conserver `%TEMP%\KHARVOX.log` avant un autre lancement. Ce paquet est destiné à corriger le déplacement/la pose et à recueillir ce diagnostic, pas à prétendre que la refonte de grenade est achevée.

Dépôt officiel cloné avec ses sous-modules dans `KHARVOX/`, base `76d728ec6f692e834081f69379a3d15d12b62ef1` (1.4a). **Nouvelle version : `../artifacts/KHARVOX-VR-mains/KharvoxDesktop.exe`**, avec DLL MSVC/SFS reconstruite, main de soutien attachée, doigts animés et grenade visible. Garder tous les fichiers ensemble. La version précédente reste dans `../artifacts/KHARVOX-VR/`. L’utilisateur confirme que son crouch et son lancer fonctionnent sur cette version précédente avec des contrôleurs Quest/Touch ; les nouveaux ajustements de mains restent à valider au casque.

## Faits observés et changements

| Sujet | Fait confirmé dans le code | Changement |
|---|---|---|
| Mains | `twoHandGrab` masquait le soutien. Après le premier correctif de visibilité, la main utilisait encore le contrôleur et la calibration globale du poing sur un autre repère de modèle. | Prise `GunHolding` attachée au point calibré de l’arme ; poignet capturé relativement à sa rotation résolue. Repères poing/prise transférés à partir des rigs existants. La translation globale du poing ne décale plus le centre de prise verrouillé. |
| Placement et doigts | Les GLB sont des poses statiques avec des origines différentes. Les rigs Blender correspondants sont présents. | Les neuf profils, le miroir gaucher et le solveur existants sont conservés. Trois déformations dérivées des rigs ouvrent index, groupe majeur/annulaire/auriculaire et pouce. Gâchette et Grip pilotent les doigts libres ; le pouce suit le Grip. Aucun suivi optique individuel n’est revendiqué. La prise verrouillée n’est pas déformée. |
| Crouch | `BodyStanceHeight` suivait la posture native ; la translation du casque ne commandait pas le crouch moteur. | Option **Physical Crouch**, désactivée par défaut : entrée à environ −35 cm, sortie à −20 cm, confirmation sur deux échantillons distincts. Impulsions sur la commande native de crouch, priorité aux boutons et au saut, compensation du double abaissement de caméra. |
| Recentrage | La calibration existante s’effectuait au chargement ; F8 était désactivé. | Deux clics de sticks simultanés, fenêtre de 150 ms. Le geste consomme Pause/Mêlée ; les clics seuls sont conservés, avec ce délai. Application quand le suivi et le gameplay sont valides, à une frontière de paire AER ; hauteur debout recalibrée. |
| Grenades | L’équipement ne recevait que le niveau du bouton ; le lancer gestuel ajouté est confirmé utilisable par l’utilisateur. | Pendant le maintien de la gâchette, une grenade 3D générique est tenue dans la main de soutien, avec doigts partiellement refermés. Elle disparaît au relâchement ou à l’annulation. Le raccord de lancer, l’origine, la vitesse, les dégâts et la gestion du projectile natif sont conservés. |
| Textures | Append borné à **8192** ; demandes non copiées conservées dans la source. Ni résidence GPU ni décodage du mip virtuel à ce point. | Instrumentation facultative des demandes répétées/différées et de leur attente observée avant copie, plus samplers, biais LOD, transferts de mip, vues Vulkan, dimensions et FSR effectivement utilisé. Aucun changement de capacité, AA, filtrage ou upscaling. |

Pour le crouch, rester debout à la première calibration et lors d’un recentrage. La référence verticale est conservée pendant une pause ou une perte de suivi ; les escaliers et déplacements du monde ne servent pas à mesurer la baisse physique. Le suivi de hauteur native sert d’indicateur de posture : **il ne constitue pas une mesure de la hitbox**. Le fonctionnement est confirmé par l’utilisateur ; les collisions et situations particulières restent à vérifier.

Pour les grenades, le constructeur natif `idClientGameMsg_HandsThrow` à RVA `0xB1FE40` et son appel depuis `idHands::ThrowItem` (retour `0xD6B937`) ont été examinés dans l’exécutable Steam installé. Il copie les neuf floats de l’axe. Le raccord vérifie la version Steam 2024-03-21, deux signatures, le joueur local et l’appel de libération ; une direction ne peut être consommée qu’une fois et expire après une seconde. Sur GOG, signature différente ou suivi invalide, le bouton natif reste disponible. L’utilisateur confirme le lancer au casque ; sa trajectoire et son origine n’ont pas fait l’objet d’une mesure instrumentée.

La référence [CircuitLordVRModInstaller](https://github.com/CircuitLord/CircuitLordVRModInstaller) précise que ses mods sont fermés. Le dépôt public distinct [kyus2001/titanfall2vr, révision 8c50a7d](https://github.com/kyus2001/titanfall2vr/blob/8c50a7d491d9a275103cfc659430e4bc1b75a608/plugin/src/hands/hand_pose.cpp) a été consulté : son point de prise et son terme par arme sont composés dans le repère final de l’arme. Ce principe est adapté au solveur et aux neuf profils de DOOM ; aucun code ni modèle du mod fermé CircuitLord n’est redistribué.

Les données de doigts sont reproductibles avec Blender 5.2.3 : `blender --background --python tools/build_hand_finger_morphs.py`. Les quatre GLB et les `.blend` restent inchangés. Les nouveaux fichiers `*_HAND_FINGERS.bin` vérifient l’identité des GLB avant application ; les poses statiques restent le repli pour des données absentes ou incompatibles. Les constantes Vulkan restent dans la limite de 128 octets. Verrouillage, poignet et doigts sont mémorisés avec la source de l’image SFS/AER.

La grenade tenue est un modèle procédural original, pas le modèle extrait du jeu. Sa visibilité suit l’armement du geste ; elle ne prédit ni la charge disponible ni le type d’équipement sélectionné. Vérifier la transition visuelle vers le projectile natif, notamment près des murs et avec les équipements alternatifs.

## Texture : cause non confirmée, comparaison à effectuer

La configuration installée observée était SFS à 90 %, avec un fichier de statut `FSR1 ACTIVE` issu d’un lancement antérieur. Cela justifie une comparaison à 100 % sans FSR, sans prouver la cause du flou.

Activer **Extended Logging** et effectuer deux captures de même durée, même sauvegarde, trajet, résolution et AA : SFS puis AER. Conserver séparément `%TEMP%/KHARVOX.log`, avec captures des deux yeux. Comparer un chargement initial puis une scène déjà parcourue ; ensuite varier un seul paramètre : FSR/échelle, puis AA. Les changements graphiques doivent se faire au menu, avec redémarrage si requis par le projet.

```powershell
python tools/compare_texture_diagnostics.py --sfs sfs.log --aer aer.log
```

`[VT-STATS]` compte des observations, incluant les répétitions. Le délai d’un jeton différé jusqu’à une copie ultérieure n’est **pas** un temps de résidence GPU. Les histogrammes Vulkan ne prouvent pas quel mip virtuel le shader a échantillonné. Le comparateur sépare les processus et rend explicites les données absentes. Aucun résultat SFS/AER mesuré en jeu n’est disponible ; aucun correctif du flou n’est proposé avant cette identification.

## Validation et binaires

- **137/137 tests CTest réussis**, sans exclusion, avec MSVC 14.44.35207, SDK Windows 26100, CMake 3.31.6 et Ninja. Configuration Release, assertions conservées pour les tests concernés. Tests Vulkan/SFS réellement exécutés sur **NVIDIA GeForce RTX 3080**. Dernière exécution complète : 23,68 s.
- `hand-render-gpu` utilise le vrai `HandRenderer`, les GLB, les déformations et les shaders livrés : pixels différents entre main ouverte/fermée et index indépendant, pixels identiques pour la prise verrouillée malgré les entrées des doigts, grenade tenue visible. Vérifié dans les deux yeux, droitier/gaucher, chemins overlay sRGB et intégration de scène UNORM. Ce test ne rend pas les armes ni le monde de DOOM.
- DLL complète construite avec `KHARVOX_BUILD_LAYER=ON` et `KHARVOX_BUILD_SFS_COMPILER=ON`. SPIRV-Cross, glslang et SPIRV-Tools proviennent de la révision Vulkan SDK 1.4.304.0 et de ses dépendances épinglées. Le manifeste de capacité contient le SHA-256 de la DLL livrée ; les 75 shaders du profil local existant sont conservés.
- Auto-tests du lanceur historique et du launcher Avalonia : sauvegarde, migration, préréglages, désactivation des options et transfert réel vers `CreateLaunchOptions()` dans les quatre combinaisons SFS/AER × droitier/gaucher. Vérification des bindings et rendu des quatre pages Avalonia en mode headless. Ces tests ne lancent pas DOOM.
- bHaptics et PSVR2 : outils reconstruits, véritables DLL de la distribution locale conservées ; contrôle PE x64 et empreintes approuvées par `tools/verify_package_integrations.ps1`. Aucune fausse DLL de test n’est livrée dans le runtime.
- Paquet final : auto-test de l’exécutable autonome réussi, manifeste SFS et 75 shaders vérifiés, chargement de sa DLL par le vrai chargeur Vulkan réussi (instances et périphériques avec/sans couche). Fenêtre Avalonia réelle ouverte sous Windows et réactive ; précontrôle de l’exécutable installé : Steam. DOOM n’a pas été lancé pour prétendre à une validation en jeu.
- Première validation indépendante MinGW Debug : 110 tests réussis ; les deux limitations libc++ de cette étape ont ensuite été levées par la compilation MSVC complète. Cinq fichiers d’intégration contrôlés avec le compilateur SFS défini puis non défini.
- Journaux récents : `../build/hand-build.log`, `../build/hand-ctest.log`, `../build/hand-render-gpu.log`. Les traces des premières versions restent disponibles.
- **Pour essayer les nouvelles mains et la grenade visible, utiliser `../artifacts/KHARVOX-VR-mains/KharvoxDesktop.exe`.** Les tests binaires et leur script sont joints dans son sous-dossier `tests/`. La version précédente reste disponible pour comparaison.

À vérifier au casque dans les quatre combinaisons droitier/gaucher × SFS/AER : contact et orientation de la main de soutien pour chaque arme, occlusion et présence dans les deux yeux ; hitbox, plafond bas, cinématiques, sauts, escaliers et ascenseurs ; recentrage sans Pause/Mêlée parasite ; grenade vers gauche/droite/haut, rotations artificielles, rechargement/cooldown et annulation lors des menus ou pertes de suivi.

## Launcher Avalonia

Interface sombre et orange, navigation Jouer / Rendu / Mouvements / Aide, sélection du dossier, profils, SFS/AER, commandes gaucher, immersion, cinématiques, calibrages et diagnostic. Le moteur de lancement existant est réutilisé dans **`KharvoxLaunchHost.exe`**, hôte minimal sans l’ancienne interface et ses démonstrations. Le bouton principal permet aussi de terminer la session qu’il a lancée.

Windows Defender a supprimé l’ancien `KharvoxLauncher.exe` avec le signalement `Trojan:Win32/Bearfoos.A!ml`. La cause de cette classification n’est pas établie. Aucune protection ni exclusion antivirus n’a été modifiée. Le nouvel hôte est recompilé depuis une liste explicite des sources du moteur ; il ne réutilise pas le binaire signalé. L’analyse ciblée Defender du nouvel hôte s’est terminée sans signalement, puis son exécution et l’auto-test des options ont réussi. Le frontend et l’hôte sont également actualisés dans le dossier de la version précédente ; sa DLL reste conservée.

Au premier démarrage, les préférences existantes sont importées en lecture seule dans un profil portable distinct, `KharvoxDesktop.settings.json`. Les mains, le crouch et les grenades gestuelles y sont activés pour cet essai ; chaque option reste désactivable. Les profils historiques et leur choix d’activation par défaut sont préservés. L’interface rappelle L3 + R3, les seuils du crouch et le geste de lancer. Le crouch au bouton et le lancer natif restent disponibles.

Publication Windows x64 autonome : .NET 10.0.14, Avalonia 11.3.9 ; aucune installation de SDK .NET n’est nécessaire pour ouvrir `KharvoxDesktop.exe`. Les préférences restent dans le dossier livré. L’installateur CircuitLord ne fournit pas cette interface ni les sources de gameplay. Les licences du launcher et du runtime sont jointes au paquet.

## Pourquoi certaines séquences restent en 2D

Le Cine Window est un choix de présentation du projet. Le réglage observé `OtherCinematicsInQuad=true` force les cinématiques classiques sur l’écran virtuel, même en mode immersif. Désactiver **Regular cinematics in Cine Window** conserve la stéréo pour les séquences prises en charge. Les menus d’amélioration et certains dialogues ont aussi un chemin 2D pour éviter les bords coupés et les mouvements imposés ; leur conversion 3D complète nécessite un travail distinct. Ce comportement n’a pas été modifié par ces correctifs.

## Fichiers modifiés ou ajoutés

- `CMakeLists.txt`
- `Docs/VR_CORRECTIONS.md`
- `launcher/KharvoxLauncher/InfoForm.cs`
- `launcher/KharvoxLauncher/KharvoxRunner.cs`
- `launcher/KharvoxLauncher/LauncherSettings.cs`
- `launcher/KharvoxLauncher/MainForm.cs`
- `launcher/KharvoxLauncher/Program.cs`
- `launcher/KharvoxLauncher/SelfTest.cs`
- `launcher/KharvoxDesktop/App.axaml`
- `launcher/KharvoxDesktop/App.axaml.cs`
- `launcher/KharvoxDesktop/DesktopSelfTest.cs`
- `launcher/KharvoxDesktop/KharvoxDesktop.csproj`
- `launcher/KharvoxDesktop/LauncherViewModel.cs`
- `launcher/KharvoxDesktop/MainWindow.axaml`
- `launcher/KharvoxDesktop/MainWindow.axaml.cs`
- `launcher/KharvoxDesktop/Program.cs`
- `launcher/KharvoxLaunchHost/KharvoxLaunchHost.csproj`
- `launcher/KharvoxLaunchHost/Program.cs`
- `launcher/KharvoxLaunchHost/SettingsLaunchOptions.cs`
- `src/camera/BodyStanceHeight.h`
- `src/camera/CameraHook.cpp`
- `src/camera/CameraHook.h`
- `src/hands/HandPoseMath.h`
- `src/hands/HandInteractionPolicy.h`
- `src/hands/HandPbr.vert`
- `src/hands/HandPbr.vert.spv`
- `src/hands/HandPbr.frag`
- `src/hands/HandPbr.frag.spv`
- `src/hands/HandRenderer.cpp`
- `src/hands/HandRenderer.h`
- `src/hands/HandVisibilityPolicy.h`
- `src/openxr/GrenadeThrowPolicy.h`
- `src/openxr/OpenXRBootstrap.cpp`
- `src/openxr/PhysicalCrouchPolicy.h`
- `src/openxr/StickRecenterPolicy.h`
- `src/vulkan/KharvoxLayer.cpp`
- `src/vulkan/TextureQualityDiagnostics.h`
- `src/vulkan/TextureQualityLog.h`
- `src/vulkan/VirtualTextureDiagnostics.h`
- `src/vulkan/VirtualTextureGuard.h`
- `src/weapon/GrenadeThrowAim.h`
- `src/weapon/WeaponHook.cpp`
- `src/weapon/WeaponHook.h`
- `src/native/NativeStereo.h`
- `assets/3D/DOOM_LEFT_HAND_FINGERS.bin`
- `assets/3D/DOOM_RIGHT_HAND_FINGERS.bin`
- `tests/compare_texture_diagnostics_tests.py`
- `tests/grenade_throw_tests.cpp`
- `tests/hand_transform_tests.cpp`
- `tests/hand_interaction_tests.cpp`
- `tests/hand_render_gpu_tests.cpp`
- `tests/hand_asset_tests.cpp`
- `tests/hand_visibility_policy_tests.cpp`
- `tests/physical_crouch_tests.cpp`
- `tests/stick_recenter_policy_tests.cpp`
- `tests/texture_diagnostics_tests.cpp`
- `tools/compare_texture_diagnostics.py`
- `tools/build_hand_finger_morphs.py`
