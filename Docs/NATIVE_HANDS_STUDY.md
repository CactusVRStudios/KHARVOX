# Mains natives Praetor — version d’essai jouable

> Pour le diagnostic du 16 novembre sur les lumières dynamiques et les
> conditions d'un raccord au renderer natif, voir
> [PRAETOR_NATIVE_LIGHTING.md](PRAETOR_NATIVE_LIGHTING.md). Le présent document
> décrit l'intégration initiale ; ses anciens paquets ne sont pas la référence
> fonctionnelle actuelle.

14 novembre 2026. Paquet : `../../artifacts/KHARVOX-VR-Praetor/KharvoxDesktop.exe`. Garder tous les fichiers ensemble. Les anciennes versions restent dans leurs propres dossiers.

Le modèle et les textures natifs Praetor de DOOM 2016 remplacent les anciennes mains GLB dans ce paquet. Ils sont convertis pour le renderer de mains déjà présent : il ne s’agit pas d’activer directement l’entité native idHands, ni d’utiliser les bras de démembrement du répertoire gore.

## Ressources confirmées

La déclaration locale `generated/decls/entitydef/player.decl` désigne `zion/player/human/base/praetor.md6` pour handsModelDefault et handsModelPraetor. Cette définition hérite de player/fp_hands.md6 et remplace son maillage par `md6/player/human/base/assets/mesh/praetor.md6mesh`. Le modèle générique arms référence au contraire player_naked.

Les archives ont été ouvertes en lecture seule. Le modèle Praetor compilé fait 1 771 440 octets, SHA-256 `db5d341a1bfe5df5a490d34751be6c3c42c998dc4f1acab497247cf7b1c8a1bb`. Le squelette marine.bmd6skl fait 10 633 octets. Le script de recherche reste `../../build/extract-native-hands.py`.

L’utilisateur a exporté le modèle standard avec Revenant 2.3.9 en Cast et PNG. Son Cast a l’empreinte `257c64df2683b799bda4ff74f7bc9baa7f381a3f62ad3108182eeaefff3696fd`. L’export contient 91 articulations, les poids natifs des cinq doigts de chaque côté et le matériau `models/characters/doommarine_playerhands/doommarine_playerhands`. Ses textures font 3840 × 3840 pixels.

L’import utilise le [plugin Cast officiel](https://github.com/dtzxporter/cast/tree/363cb39c0425844e29bf4c2457bbbb1d2b9bb4c6). Script et audit : `../../build/import-praetor-export.py` et `../../build/praetor-export-audit.json`. Les ressources utilisateur restent dans build/native-hands-export ; elles n’écrasent pas les assets de référence du dépôt.

## Matériaux décodés

Les shaders locaux décompilés en lecture seule confirment :

- normale X dans l’alpha de la couleur : sampleNormal.w = sampleDiffuse.w ;
- normale Y dans l’alpha du spéculaire : sampleNormal.y = sampleSpecular.w ;
- Z reconstruit par sqrt(max(0,1-X²-Y²)) ;
- brillant obtenu par transfert sRGB vers linéaire de l’alpha de la troisième texture : DeGamma(sampleLightmap.w).

`tools/praetor_materials.py` conserve la couleur RGB, rend la couleur opaque, reconstruit les normales et convertit le brillant en rugosité pour le shader existant. Le matériau reste diélectrique. Le spéculaire RGB natif et les lumières du moteur ne sont pas intégralement reproduits : la réponse lumineuse est une approximation. Aucun correctif du flou global du jeu n’est déduit de cette conversion.

Preuve locale : `../../build/praetor-material-shaders.glsl`, produit par `../../build/dump-native-material-shaders.cpp`. Les six cas de `tests/praetor_material_tests.py` vérifient couleur, opacité, canaux de normales, reconstruction et transfert du brillant. Ils sont enregistrés dans CTest sous praetor-materials quand Python/NumPy sont disponibles.

## Géométrie et commandes

`tools/build_praetor_hands.py` sépare les côtés à partir des poids natifs et retire les épaules/hauts de bras. Chaque main produite contient 5 679 sommets et 9 339 triangles. Le bas d’avant-bras conservé mesure environ 25,3 cm dans les unités de l’export, avant adaptation de taille.

La pose au repos est courbée en C. Les rotations des articulations natives ferment index, autres doigts et pouce avec les commandes Quest/Touch existantes. La pose GunHolding reste figée pendant une prise verrouillée. Les six fichiers GLB/FINGERS respectent les noms et le format KHFING02 ; leurs empreintes vérifient leur correspondance.

Les repères sont adaptés séparément aux anciens exports Fist et Gun avec les articulations et la largeur de paume. Les calibrations et les neuf profils d’appui restent réutilisés. Les points de prise utilisent les phalanges natives ; les minuscules queues d’os d’affichage du plugin Cast ne servent pas de bouts de doigts.

Les commandes classiques, le gaucher, les règles de présentation et la synchronisation SFS/AER sont conservés. Les mains ajoutées restent masquées quand la présentation confie les mains au jeu.

**Limite :** l’avant-bras suit rigidement le poignet, sans IK du coude ou de l’épaule. Ce paquet n’ajoute pas de corps complet. Contact réel avec les armes, orientation et intersections restent à vérifier au casque.

## Partage des textures

`src/hands/HandRenderer.cpp` partage les images GPU identiques entre les poses. La clé inclut contenu décodé, dimensions et transfert sRGB/linéaire. Les références partagées gardent chaque texture jusqu’au dernier utilisateur ; le cache ne conserve que des références faibles.

Le test GPU impose exactement trois créations d’images de textures pour les quatre poses et contrôle leur libération après destruction du renderer. La résolution de 3840 × 3840 est conservée : environ 169 Mio de texels RGBA pour trois images, contre 675 Mio pour douze copies, hors alignement du pilote. Cela ne décrit pas le budget total de DOOM/OpenXR. La limite de 8192 pages virtuelles n’est pas modifiée.

## Validation exécutée

- MSVC Release, CMake/CTest : **143/143 tests réussis**, 8,48 s, avec KHARVOX_PRAETOR_TEST_RUNTIME dirigé vers le paquet local.
- Les tests précédents utilisent les anciennes mains du dépôt. praetor-hand-assets, praetor-hand-interaction et praetor-hand-render-gpu utilisent les nouveaux fichiers natifs.
- Le vrai renderer Vulkan sur RTX 3080 valide animation des doigts, index indépendant, prise figée, les deux yeux et droitier/gaucher, en overlay sRGB et intégration de scène UNORM. Il contrôle les trois atlas partagés et leur libération.
- Ces tests ne rendent pas les armes et le monde de DOOM. Ils ne prouvent pas le contact visuel de la main avec une arme.
- `../../artifacts/main-praetor-repos.png` et `../../artifacts/main-praetor-prise.png` sont des rendus Blender des GLB et déformations livrés, pas des images inventées ni des captures en jeu.

Les binaires de tests sont joints au paquet. Journaux : build/praetor-build.log, praetor-ctest.log, praetor-generation.log, praetor-import.log et praetor-render.log.

Pour régénérer après un export Cast : importer avec build/import-praetor-export.py, convertir les textures avec tools/praetor_materials.py, puis lancer Blender sur tools/build_praetor_hands.py avec --source, --output et --audit. Utiliser un dossier de sortie séparé des assets de référence.

## Essais restant à effectuer

Tester SFS/AER et droitier/gaucher : taille et orientation, placement du poignet, prise des neuf armes, visibilité dans les deux yeux, intersections des avant-bras, passage main libre/prise et retour après interaction/cinématique. Les raccourcis de calibration existants restent disponibles.

Le crouch, le recentrage et le correctif de déplacement restent ceux du paquet précédent. **La refonte de grenade native demeure inachevée** : ce paquet ne prétend pas supprimer le délai ni libérer un véritable objet tenu. Le faux aperçu reste masqué en gameplay. Le diagnostic optionnel des mains natives reste désactivé sans son marqueur ; le test d’identité de modèle précédemment demandé n’est plus un préalable.
