# Ralentissements VR — enquête du 14 novembre 2026

Priorité : retrouver la fluidité avant de reprendre les matériaux et les points de prise. Le paquet d'essai est `artifacts/KHARVOX-VR-performance`. Les anciennes versions restent disponibles.

## Faits observés

- L'utilisateur confirme que le ralentissement existait avant les mains Praetor. Leurs textures ne peuvent donc pas expliquer son apparition. Il signale une disparition des lenteurs pendant les interactions/cinématiques ; le code y masque les mains ajoutées et change également la présentation de la scène.
- Le journal conservé dans `build/perf-analysis/user-KHARVOX.log` provient du paquet Praetor, SFS, rendu 100 %, RTX 3080, Virtual Desktop OpenXR. En gameplay, les lots de 120 images prennent environ 13 à 14 secondes. Des intervalles d'environ 90 ms sont enregistrés entre la sortie d'un Present et l'entrée du suivant. Ce délai ne désigne pas une fonction fautive : il comprend le travail et les attentes du jeu.
- Les timestamps de la commande XR indiquent environ 1 à 1,4 ms en gameplay. Leur portée exclut le rendu DOOM exécuté auparavant. Ils ne mesurent pas toute l'image ni toutes les attentes GPU.
- La configuration originale enregistrée indique SFS 110 %, celle du paquet testé SFS 100 %. La journalisation étendue est désactivée dans la première et activée dans la seconde. Ce sont des réglages enregistrés, pas la preuve de tous les réglages utilisés lors de chaque comparaison.
- Les 75 shaders du profil SFS sont identiques par SHA-256 entre la distribution originale locale et le paquet testé. La compilation est Release, `/O2 /Ob2 /DNDEBUG`.
- Aucun chargement GLB, décodage de PNG, hash de texture ou skinning CPU n'est ajouté par image. Les déformations des doigts s'effectuent dans le vertex shader. La géométrie Praetor compte 5 679 sommets par pose, contre 13 404 pour les anciennes mains.
- Les trois atlas Praetor partagés représentent 168,75 Mio de texels RGBA non compressés, hors alignement. Ils restent une charge mémoire supplémentaire. Le renderer actuel partage également les atlas identiques des anciennes poses.
- Le diagnostic VT a observé 5 614 327 demandes, zéro report et un pic de 4 503 pages, sous la limite de 8 192. La capacité reste inchangée. Certaines anciennes lignes VT/TEXTURE/HAND-SUPPORT indiquent erronément AER parce qu'elles lisent un ancien indicateur ; le moteur de cette session est bien SFS.

## Mesures locales reproductibles

`KharvoxHandRenderGpuTests.exe <dossier-runtime> --benchmark --extent=3080x3300` utilise le renderer de production, 30 images de chauffe puis 120 mesures GPU. Deux mains sont dessinées dans chaque œil ; les deux copies de profondeur sont incluses. Les effacements de la scène synthétique, DOOM, le compositeur, les attentes CPU et la soumission ne font pas partie du chronométrage. Aucun upload/allocation GPU après la chauffe, aucune fuite à la fermeture.

Après la fin des autres tests GPU, trois essais séquentiels sur RTX 3080 donnent : Praetor **0,791 ms**, anciennes mains avec les mêmes shaders actuels **1,053 ms**, Praetor **0,817 ms**. Ce contrôle ne compare pas l'ancien exécutable complet au nouveau et ne reproduit pas la pression mémoire de DOOM. Il ne démontre pas que les mains sont la cause du ralentissement. Journal : `build/perf-analysis/benchmark-repeated.log`.

## Changements livrés

- `src/vulkan/VirtualTextureDiagnostics.h` : les demandes entièrement acceptées ne parcourent plus les pages lorsqu'aucune attente n'existe. Le traitement des reports, des propriétaires, des doublons et du premier horodatage est conservé. Les tests utilisent aussi des pages mémoire protégées pour détecter un parcours inutile. C'est une surcharge supprimée, sans preuve qu'elle expliquait les fortes lenteurs.
- `src/vulkan/TextureQualityDiagnostics.h` : une seule recherche de l'image par lot d'upload, au lieu d'une par région. Les compteurs de mipmaps et la limite de suivi de 1 024 images conservent leurs résultats ; la capacité native de pages ne change pas.
- `src/openxr/PhysicalCrouchPolicy.h` et `OpenXRBootstrap.cpp` : suppression de la lecture verrouillée de posture lorsqu'il n'existe ni demande physique ni propriété de crouch à restituer. L'état des commandes continue d'être mis à jour systématiquement. Le test différentiel compare les 64 combinaisons d'entrées depuis 4 096 états atteignables avec l'ancien calcul, puis vérifie le retour debout.
- `src/hands/HandSceneDrawPolicy.h`, `HandRenderer.cpp` et `OpenXRBootstrap.cpp` : lorsqu'aucune main n'a de pose visible/valide et qu'aucun laser n'est actif, sortie avant les recherches d'attachments et les transitions de profondeur inutiles. Le laser seul et chaque main indépendante restent traités. Aucun cache Vulkan susceptible de survivre à ses images n'est ajouté.
- `OpenXRBootstrap.cpp` : reset de queries GPU, timestamps et lecture des résultats réservés aux diagnostics étendus ou à la comparaison. Aucun de ces chronométrages n'est soumis en lancement normal lorsque leurs résultats ne sont pas utilisés ; le rendu, ses synchronisations et les diagnostics disponibles sont conservés.
- `src/openxr/HandPerformanceComparison.h`, `PerformanceComparisonLog.h` et `OpenXRBootstrap.cpp` : comparaison optionnelle **affichées / masquées / affichées**, trois fenêtres de 14 secondes de gameplay, dont 2 secondes de chauffe exclues. Pause/cinématique/menu interrompent la fenêtre et imposent une nouvelle chauffe. Seuls le dessin des mains ajoutées et son intégration de profondeur changent. Les entrées, le suivi, la pose de l'arme, la résolution et les commandes sont conservés. Les textures restent résidentes.
- `src/common/PerformanceCpuCounters.h`, `src/camera/CameraHook.cpp`, `src/weapon/WeaponHook.cpp` et `OpenXRBootstrap.cpp` : chronométrages CPU optionnels, sans écriture par image, de la mise à jour native des mains, de la partie ajoutée, de la lecture de posture, des actions et de la pose d'arme. Les portées se chevauchent : leurs durées ne s'additionnent pas. Les compteurs couvrent la durée du processus, y compris les transitions.
- `launcher/KharvoxDesktop/Program.cs` : `KharvoxPerformance.exe` ou l'argument `--performance-comparison` active cette comparaison via une variable d'environnement héritée. `KharvoxDesktop.exe` conserve le lancement normal. Aucun nouveau menu de correctifs.
- Dans le nouveau paquet, `ExtendedLogging=false`. Aucun réglage de qualité du jeu n'est abaissé et les ressources Praetor sont conservées. Les diagnostics détaillés restent disponibles dans le launcher.

## Essai en jeu

1. Lancer `KharvoxDesktop.exe` du nouveau dossier et comparer la fluidité au même endroit. Les mains, crouch et autres commandes restent actifs ; la journalisation détaillée est désactivée.
2. Si le ralentissement persiste, quitter puis lancer `KharvoxPerformance.exe`, et démarrer le jeu normalement. Garder les mains activées. Après le chargement, rester dans une scène calme avec le même point de vue, sans interaction, pendant environ 42 secondes. Les mains disparaissent au milieu du contrôle puis reviennent automatiquement. Le délai peut s'allonger si un menu ou une cinématique intervient.
3. Le fichier `performance_comparison.txt`, dans ce dossier, contient `complete=1` une fois les trois fenêtres terminées : cadence globale, durée XR CPU, temps GPU XR en SFS, compteurs CPU et budget mémoire WDDM. Le contrôle fonctionne aussi en AER ; son compteur GPU XR est alors explicitement indisponible. Les nombres WDDM sont par processus/adaptateur, pas une mesure globale de toute la VRAM système.

Si seule la fenêtre sans mains est fluide, poursuivre sur la voie de rendu/intégration des mains. Si les trois fenêtres restent lentes, les compteurs CPU et le budget mémoire orienteront l'enquête vers les hooks/attentes/rendu du jeu. Si les deux fenêtres avec mains diffèrent fortement, la scène n'était pas assez stable pour conclure. Le contrôle ne libère pas les textures : il ne peut pas exclure une pression mémoire.

## Validation et limites

Les résultats définitifs CMake/CTest et du launcher sont joints dans `VALIDATION.txt` du paquet. Les tests couvrent la sélection des phases, le retour des mains, l'exclusion des transitions, les compteurs CPU activés/désactivés, les deux yeux et droitier/gaucher du renderer existant. Ils ne remplacent pas un essai DOOM au casque. La cause principale des lenteurs n'est pas encore confirmée.

Fichiers de validation modifiés : `CMakeLists.txt`, `tests/hand_performance_comparison_tests.cpp`, `tests/performance_cpu_counters_tests.cpp`, `tests/hand_visibility_policy_tests.cpp`, `tests/physical_crouch_tests.cpp`, `tests/texture_diagnostics_tests.cpp`, `tests/hand_render_gpu_tests.cpp` et `launcher/KharvoxDesktop/DesktopSelfTest.cs`. Le mode benchmark reste optionnel et n'alourdit pas les essais fonctionnels ordinaires. Le script `tests/run-tests.cmd` du paquet joint aussi les nouveaux contrôles.

L'apparence peu intégrée des mains est expliquée par l'éclairage fixe du shader existant, sans les lumières et ombres natives de DOOM ; ce n'est pas une absence totale de shader. Le contact de la main secondaire avec les armes signalées reste incorrect et n'est pas présenté comme corrigé ici. La refonte complète du projectile de grenade reste également en attente.
