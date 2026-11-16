# Praetor : diagnostic du rendu natif et des lumières dynamiques

16 novembre 2026. Analyse locale du dépôt, des ressources déjà extraites et
du shader effectivement livré. Aucune pose, configuration, capture en jeu,
compilation ou modification du renderer effectuée pour ce diagnostic.
Les chemins de ressources et de code sont relatifs à la racine du dépôt
`KHARVOX/` ; `../build/` désigne donc le dossier de travail voisin.

## Statut : mis de côté à la demande de l'utilisateur

Le 16 novembre 2026, l'utilisateur demande de consigner ce sujet et de passer
aux grenades. Suspendre le chantier éclairage Praetor, y compris la proposition
de flash approximatif : aucune de ces pistes n'a été implémentée. Reprendre
uniquement sur demande. Aucun nouveau build, shader, modèle ou profil modifié.

### Derniers résultats vérifiés avant mise de côté

- Le parseur existant `tools/verify_native_skeleton_conversion.py` accepte le
  `marine.bmd6skl` déjà extrait. Les 91 noms et parents correspondent exactement
  au squelette du `praetor.cast` utilisateur.
- Le facteur Cast → unités natives mesuré sur ce squelette est `0.5`, avec
  erreur locale de position nulle, erreur globale maximale d'environ
  `1.9186e-5` unité native, rotations identiques et erreur de vérification des
  inverse binds d'environ `1.4547e-5`. Ce facteur décrit ces ressources ; il
  ne remplace pas la calibration de taille physique des mains.
- `../build/shotgun-native-integration/pose-source.json` et
  `Praetor-Shotgun-source.blend` correspondent aux empreintes de
  `../artifacts/KHARVOX-VR-Valide/manual-pose-provenance.json`. Le GLB
  `PRIMARY_SHOTGUN_RIGHT.glb` correspond aussi à cette provenance. Les poses
  complètes d'articulations sont conservées : le workflow Blender est
  réutilisable, sans refaire le placement artistique.
- Hiérarchie vérifiée : `origin → righthandattach → righthand`, puis doigts,
  `righthandprop` et chaîne d'avant-bras. Point important : le shotgun est
  déclaré attaché à **`righthandattach`**, pas à `righthandprop`, dans
  `fp_hands.md6.decl`. Conserver cette attache tout en pilotant la branche du
  poignet est donc structurellement envisageable pour le shotgun. Cela ne
  démontre pas encore le fonctionnement du pilotage natif en jeu.
- Inspection statique ciblée de l'exécutable installé, sans exécution :
  `GetJointTransform` à RVA `0x15EE6C0` lit l'animateur via `model+0xE98`, puis
  appelle RVA `0x16FBE40`. Cette fonction sélectionne les tableaux à
  `animator+0x428` ou `+0x430` et lit des matrices de 48 octets par joint.
  Cette localisation prouve un chemin de lecture, **pas** un contrat sûr
  d'écriture, de recalcul des caches ou de publication au renderer.
- L'export contient une surface de mains commune aux deux côtés, avec les
  surfaces de corps distinctes. Le masquage actuellement raccordé est par
  surface ; l'affichage de la seule main droite n'est pas établi.

### Point précis de reprise

Deux verrous restent ouverts : publier une pose après l'animation et avant
sa consommation par le rendu, sans course ni écrasement ; afficher uniquement
le côté droit avec le matériau natif sans faire apparaître le bras gauche
natif en plus du GLB. Ne pas tenter une écriture mémoire sur la seule base
des offsets localisés.

Si ces deux points sont vérifiés, POC limité au shotgun classique et à la
main droite, avec la pose validée existante, `righthandattach` inchangé et le
renderer GLB conservé comme repli. Comparaison native/GLB et synchronisation
SFS/AER restant à réaliser. Aucun POC ni test en jeu n'a été effectué.

Le rapport machine `../build/praetor-lighting-audit/diagnostic.json` conserve
l'état de l'analyse initiale ; les résultats de reprise ci-dessus le
complètent sans réécrire cette preuve historique.

## Cause confirmée

Le shader `src/hands/HandPbr.frag` éclaire les modèles importés avec une
direction constante `normalize(vec3(-0.35, 0.75, 0.55))`, un terme ambiant
constant et une direction de vue spéculaire constante. Il ne reçoit ni les
lumières de DOOM, ni ses sondes d'environnement, ni ses ombres. Un flash de
tir du moteur ne peut donc pas modifier cet éclairage.

Ce constat concerne aussi le binaire livré, pas seulement un fichier source :
le fragment SPIR-V du dernier paquet HUD et celui du paquet armes validées
sont identiques à `src/hands/HandPbr.frag.spv`, SHA-256 :

```text
eb6ddd6ffd4d3151986ff3570223a502347699b9dafbad35a9911bb55ff7f1dd
```

La lecture de ses déclarations SPIR-V confirme exactement trois ressources,
`set=0`, bindings `0/1/2` : couleur, normale, métallique/rugosité. Aucune
ressource de lumière et aucun push constant fragment. Le vertex shader reçoit
la matrice modèle, la projection et l'ouverture des doigts, sans données de
lumière. `HandRenderer::initialize` annonce également explicitement cet
éclairage constant sans lumières ni ombres idTech 6.

Les mains ont donc bien des shaders. Leur réponse aux normales existe ; le
défaut principal est l'absence de raccord à l'éclairage du moteur.

## Ressources natives retrouvées

Sources locales conservées dans `../build/native-hands-extracted/` :

| Ressource | Donnée observée |
|---|---|
| `player.entity.decl`, lignes 106–107 | `handsModelDefault` et `handsModelPraetor` désignent `zion/player/human/base/praetor.md6` |
| `praetor.md6.decl` | Hérite de `player/fp_hands.md6` et remplace le mesh par `md6/player/human/base/assets/mesh/praetor.md6mesh` |
| `fp_hands.animweb.decl` | Modèle racine `player/fp_hands.md6`, armes en modèles distincts de l'anim-web |
| `fp_hands.md6.decl` | Animations d'interaction GUI, dont `fists_virtualgui/point_center` et `swipe_left_center` |
| `doommarine_playerhands.material.decl` | Diffuse, spéculaire, puissance/brillant, normale et textures virtuelles moteur |
| `marine.bmd6skl` et `praetor.bmd6model` | Squelette et modèle compilés déjà présents |

L'export utilisateur Praetor référence bien le matériau
`models/characters/doommarine_playerhands/doommarine_playerhands`, avec 91
articulations dans l'export complet et les textures couleur, spéculaire et
brillant présentes. Voir `../build/praetor-export-audit.json`.

Ces données identifient le modèle par défaut et ses animations. Elles ne
constituent pas une observation de l'entité et de la variante de shader
effectivement choisies pendant chaque interaction particulière en jeu.

Le matériau natif utilise les tables de pages et les pages physiques de
textures virtuelles du moteur. Les shaders locaux décompilés dans
`../build/praetor-material-shaders.glsl` montrent des chemins d'éclairage avec
listes de lumières par cluster, paramètres des lumières, sondes et atlas
d'ombres. Les bindings observés incluent `10/11/12` pour les paramètres,
`18` pour les sondes, `20` pour l'atlas d'ombres et `26/27` pour les clusters.
Les noms des variables attestent ces dépendances, mais **l'attribution exacte
d'une variante compilée à un dessin des bras d'interaction reste non établie**.
Le fichier regroupe plusieurs shaders locaux ; ne pas assimiler toute la
collection à un shader exclusif des mains.

## Différences avec le renderer KHARVOX

| Élément | Natif | Praetor importés |
|---|---|---|
| Géométrie | Modèle moteur, squelette et surfaces natives | GLB convertis ; poses validées, déformations des doigts |
| Matériau | Spéculaire RGB et brillant natifs, textures virtuelles | Trois atlas GLB ; approximation diélectrique métal/rugosité |
| Normales | Chemin matériau moteur | Normale tangente reconstruite, échantillonnée par le shader existant |
| Éclairage | Ressources et traitement du moteur | Direction, ambiant et vue spéculaire constants |
| Insertion | Soumission du modèle au moteur | Passe KHARVOX sur image achevée ou overlay ; profondeur partagée/copiée selon le chemin |

`tools/praetor_materials.py::decode_material` reconstruit X depuis l'alpha de
la couleur, Y depuis l'alpha du spéculaire, puis Z. Le brillant linéarisé
devient la rugosité. Le spéculaire RGB natif n'est pas préservé comme entrée
spéculaire indépendante. Il y a donc aussi une différence de matériau,
distincte du manque de lumière dynamique.

`HandRenderer::loadAsset` calcule éventuellement le skinning au chargement
avant de créer ses buffers. Les poses par arme exportées sont des géométries
préparées pour ce renderer, pas des instances natives pilotées par ces mêmes
matrices. Un nom de matériau natif ajouté au GLB ne créerait pas le raccord.

`OpenXRBootstrap.cpp` appelle `HandRenderer::recordSceneIntegrated` sur la
source finale, puis conserve son chemin de repli. La passe intégrée emprunte
couleur et profondeur ; elle ne récupère pas les ressources d'éclairage. Le
nom « integrated » ne signifie donc pas que le shader reçoit les lumières
natives. Déplacer cette passe seul ne change pas les bindings du shader.

## Faisabilité de la correction native

Le raccord natif existant dans `src/weapon/WeaponHook.cpp` connaît :

- `getHandsModel` : accès au modèle ;
- `getJointTransform` / `getNamedJointTransform` : lecture d'articulations ;
- `hideRenderModelMesh` : masquage des surfaces ;
- `hideAnimatedPlayerRootModel` : masque le modèle racine natif en gameplay
  VR, tout en laissant les armes, leur animateur et leurs repères actifs.

Ce masquage est intentionnel. Le retirer afficherait le modèle complet animé
par DOOM, pas automatiquement nos deux mains aux positions enregistrées. Les
armes utilisent aussi des repères de cette animation : modifier globalement
ce squelette sans séparation pourrait affecter leurs transformations.

**Interface manquante dans le raccord actuel :** un chemin vérifié permettant
de soumettre le maillage des avant-bras avec le matériau natif et nos poses
finales indépendantes, sans modifier les joints qui pilotent les armes. Ses
règles de durée de vie, synchronisation CPU/rendu, masquage par surface et
participation SFS/AER doivent également être établies. La présence du mesh,
des textures et d'une fonction de lecture de joints ne suffit pas à établir
ce chemin d'écriture/soumission.

La correction à privilégier est un adaptateur limité à la soumission des
bras natifs, alimenté par les poses actuelles, avec le renderer GLB conservé
comme repli. Sa faisabilité exacte n'est pas encore démontrée. Avant toute
implémentation, identifier et vérifier cette interface sur l'exécutable
installé, puis prouver sur une main qu'elle conserve les sommets et les
transformations validés. Ne pas généraliser ni modifier les armes pour
compenser un raccord incomplet.

Remplacer simplement le fragment shader par un SPIR-V natif n'est pas
compatible : formats de sommets, entrées inter-étages, descripteurs,
textures virtuelles et paramètres de vue diffèrent. Reconstruire toutes ces
dépendances au Present constituerait un autre backend de rendu, dont la
nécessité n'est pas démontrée. Aucun tel chantier ni traceur GPU n'est lancé.

Il n'y a donc **pas encore de correctif natif minimal vérifié ni de nouveau
paquet de test**. Ce résultat identifie le blocage ; il ne démontre pas que
l'intégration native est impossible. Une approximation de flash ne serait
pas une validation de l'objectif de rendu natif demandé.

## Vérifications effectuées et limites

- Six tests existants de conversion des matériaux réussis : couleur,
  opacité, normales, transfert du brillant, contrôles de format/dimensions.
- Bindings du fragment SPIR-V livré inspectés directement et empreintes
  comparées entre source, paquet armes et dernier paquet HUD.
- Ressources natives et raccords de lecture/masquage examinés en lecture
  seule. Aucun offset mémoire ni point d'appel supposé ajouté.
- Aucun test de flash, comparaison native/importée, mesure de performance ou
  nouvelle validation stéréoscopique en jeu effectué. Les tests de matériaux
  ne prouvent pas une réponse lumineuse native ni l'absence d'un défaut
  secondaire de normales.

Une fois le raccord prouvé, le paquet indépendant devra vérifier tirs,
changements d'environnement, main libre, une/deux/une main, interactions,
gauche/droite, SFS/AER et coût CPU/GPU. Les poses, profils d'armes, visée,
accroche et placement du HUD doivent rester identiques. La référence
fonctionnelle actuelle reste préservée.

Rapport machine : `../build/praetor-lighting-audit/diagnostic.json`, depuis
la racine `KHARVOX/` du dépôt.
