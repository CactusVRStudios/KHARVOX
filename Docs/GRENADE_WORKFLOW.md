# Grenades VR / Praetor : analyse et atelier Blender

## Sujet mis de côté

Le 16 novembre 2026, l'utilisateur demande de consigner et mettre ce chantier
de côté pour une prochaine séance. La tenue visuelle est validée ; le raccord
du projectile natif depuis la main reste à faire. Préserver le paquet testé
et les sources de pose. Aucun travail supplémentaire sur le lancer n'est
engagé pendant la correction d'espacement du HUD.

## Validation en jeu : premier paquet visuel

Le 16 novembre 2026, après confirmation que la grenade utilise uniquement
la main gauche et que la droite conserve l'arme, l'utilisateur confirme :
« je viens de tester ca fonctionne ».

La tenue visuelle issue de la pose Blender est donc validée en jeu pour le
paquet `build/grenade-workshop/frag/conversion-1/test-visuel`.
Ce retour ne valide pas un nouveau système de lancer : le projectile natif,
son origine, sa vitesse et son délai restent inchangés. Le raccord du lancer
depuis la main reste à traiter. Aucun fichier du paquet testé, aucune pose,
aucun profil d'arme ni réglage HUD n'est modifié par cette consignation.

## Mise à jour : pose validée, premier paquet visuel autorisé

Le 16 novembre 2026 à 19 h 27, l'utilisateur a enregistré la pose et confirmé
que la grenade est bien tenue par les doigts. Les sources `atelier.blend` et
`pose.json` restent intactes. Le JSON porte encore le statut d'auteur initial ;
la validation humaine est consignée ici et dans `conversion-1/conversion.json`.

La conversion compare les 91 articulations et toutes les matrices de scène
au JSON : écart maximal de composante nul. Les contacts artistiques validés
sont conservés, sans ajustement automatique malgré les croisements de
triangles signalés. Les GLB utilisent directement des mètres dans le repère
Grip XR gauche ; la calibration de main libre ne doit pas être réappliquée.

À la question sur le périmètre du premier essai, l'utilisateur choisit
explicitement **« Premier paquet visuel uniquement »**. Cela autorise la
prévisualisation de tenue avant le raccord complet du lancer.

Paquet indépendant :
`build/grenade-workshop/frag/conversion-1/test-visuel/LANCER_TEST_VISUEL.cmd`.
Dans ce dossier, `ACTIVER_APERCU.cmd` et `DESACTIVER_APERCU.cmd` règlent
`grenade_visual_test.cfg` ; quitter DOOM et relancer pour appliquer le choix.
Le fichier contient version, activation et empreintes FNV64 des deux GLB.
Un asset absent ou modifié désactive cet aperçu et conserve les mains existantes.

Le maintien reconnu par `GrenadeThrowPolicy::holding()` pilote le seul état
visuel. En mode droitier, main gauche libre, il sélectionne la main Praetor
figée et la grenade texturée. Le relâchement restaure la main libre ; une
prise à deux mains, un suivi invalide ou un contexte non jouable annulent
le maintien selon les règles existantes. Le mode gaucher conserve son chemin
précédent, sans la nouvelle prévisualisation gauche.

**Limites de cet essai :** sélectionner la grenade fragmentaire manuellement.
L'aperçu ne lit pas encore l'équipement sélectionné ni son cooldown ; il peut
apparaître avec un autre équipement ou une recharge en cours. Le projectile
natif reste distinct. Son délai, son origine et sa vitesse ne sont pas
corrigés par ce paquet. La disparition de l'aperçu au relâchement ne prouve
pas une libération native synchronisée. Le bouton, la consommation, les
collisions et les effets natifs sont inchangés.

Code limité à `GrenadeVisualPolicy.h`, `HandRenderer.cpp/.h`, au choix du
booléen visuel dans `OpenXRBootstrap.cpp`, et aux tests/CMake. Aucun changement
dans `WeaponHook.cpp`, les politiques de lancer, les shaders ou le code HUD.
La main tenue réutilise les atlas Praetor ; seuls les trois atlas de grenade
s'ajoutent. Les fichiers du HUD et les dix profils/poses d'armes sont copiés
sans modification depuis le dernier paquet HUD. Aucun jeu n'est lancé durant
la préparation. Le lancement de test force le traceur GPU arrêté à désactivé.

Les tests et empreintes du paquet figurent dans
`build/grenade-workshop/frag/conversion-1/validation-paquet.json`,
`ctest.log`, `gpu-visual.log`, `gpu-desactive.log` et
`gpu-config-invalide.log`. Les paragraphes ci-dessous documentent les phases
A/B initiales et leurs limites ; ils ne remplacent pas ce périmètre approuvé.

16 novembre 2026. Première livraison limitée aux phases A/B. Le chantier
éclairage Praetor reste mis de côté. Aucun changement du lancer, des armes,
du HUD ou du paquet fonctionnel. Pas de lancement ni capture automatique du jeu.

## Ouvrir et régler

Depuis `KHARVOX_corrections`, ouvrir `ATELIER_GRENADE.cmd`.
La scène est `build/grenade-workshop/frag/atelier.blend` ; le script de panneau
est `KHARVOX/tools/grenade_workshop_blender.py`.

Dans `N > KHARVOX`, section « Grenade / Praetor gauche » :

1. « Déplacer grenade » / « Tourner grenade », puis gizmos ou `G` / `R`.
2. Pour chaque doigt, boutons `1`, `2`, `3`, puis rotation à la souris.
3. « Déplacer la main entière » déplace aussi la grenade attachée.
4. « Vérifier les intersections » compte les croisements de surfaces ; aucune
   optimisation automatique. Un volume entièrement contenu peut lui échapper.
5. « Enregistrer la pose » écrit **dans ce nouvel atelier seulement**
   `atelier.blend` et `pose.json`. « Recharger la pose » restaure les données.
6. Réouvrir par le CMD pour retrouver la scène et enregistrer de nouveaux
   réglages. Ne pas reconstruire la scène pour la réouvrir.

`GRENADE_anchor_left` est l'unique attache de l'équipement, parentée à l'os
`lefthand`. Déplacer `GRENADE_tenue`, pas l'ancrage. Le JSON conserve la matrice
locale par rapport à l'attache, la racine de la main, les transformations du
rig/mesh et les 91 matrices de pose des articulations. Les quinze contrôles de
phalanges sont déverrouillés, les autres articulations sont masquées/verrouillées.

**STOP obligatoire : attendre la validation explicite de cette pose avant
toute conversion de jeu, DLL, intégration du lancer ou paquet de test.**
La proposition initiale est à régler manuellement, pas une pose finale validée.

## Ressources réutilisées

Chemins depuis `KHARVOX_corrections` :

- `build/native-weapons-export/models/md6/objects/weapons/explosives/grenade_frag/assets/mesh/grenade_frag.cast`
  et ses textures `_images/grenade_frag_vmtr_{c,s,g,m}.png`.
- `build/hud-left-workshop/HUD-Praetor-Gauche.blend`, lu seulement pour copier
  la géométrie Praetor gauche, son rig et sa calibration déjà contrôlée.
  Tous les objets HUD sont retirés de **la nouvelle scène en mémoire** ;
  le fichier HUD source et ses réglages ne sont pas modifiés.
- `build/praetor-hands/{BaseColor,Normal,MetallicRoughness}.png` pour la main.
- `tools/manual_praetor_poseur.py::activate` et son opérateur de sélection
  d'articulation ; `tools/build_praetor_hands.py::pose` / `landmarks` ;
  `tools/preview_praetor_weapon_grips.py::mesh_object`, `texture_material`,
  caméra et éclairage de studio. Aucun de ces scripts n'est modifié.
- `tools/praetor_materials.py::decode_material` pour reconstruire les trois
  textures de prévisualisation de grenade dans le nouveau dossier `textures/`.
  C'est l'approximation de matériau Blender existante, pas idTech 6.

Le modèle tenu fragmentaire est confirmé par la déclaration native
`throwable/zion/player/sp/frag_grenade` :
`handsModelMD6 = zion/objects/weapons/explosives/grenade_frag.md6`.

## Échelle et limites de conversion

Le parseur existant `verify_native_skeleton_conversion.py` et le lecteur
d'archives RES existant sont réutilisés en lecture seule. Pour **chacun** des
trois exports, comparaison de tous les noms, parents, positions et rotations
au repos, et vérification des inverse binds. Le facteur mesuré est `0.5`
dans les trois cas ; ce n'est pas une hypothèse commune aux armes. Les
checksums des modèles compilés correspondent à leurs squelettes.

La scène exprime ces unités natives en centimètres avec `100 / 39.3701`,
convention de monde déjà utilisée par KHARVOX. L'échelle est incorporée aux
sommets de la copie de grenade ; le contrôle de pose garde une échelle 1.
Le placement initial est une proposition manuelle basée sur la paume.

**Non vérifié :** la transformation finale du modèle d'équipement tenu par
le moteur (projection/échelle de première personne). La référence conserve
`final_runtime_model_transform_verified: false`. Ne pas transformer cette
scène en export prétendument calibré en jeu sans vérifier cette étape en
phase C. Le projectile de campagne référence en outre un modèle LWO distinct
avec son propre scale : ne pas appliquer ce scale aveuglément au mesh MD6 tenu.

Preuves et empreintes : `build/grenade-workshop/frag/reference-native.json`,
`inputs-sha256.json`, `diagnostic-initial.json` et `verification.json`.

## Phase A : état du code et mécanismes natifs

| Élément | Fait observé |
|---|---|
| Geste | `src/openxr/GrenadeThrowPolicy.h` : maintien, historique de huit vitesses, moyenne récente sur 100 ms, relâchement, conditions de suivi/main libre ; impulsion équipement de 100 ms |
| Conversion de direction | `OpenXRBootstrap.cpp` : vitesse convertie vers le monde, mais seul son axe normalisé est transmis au raccord natif |
| Message natif | `WeaponHook.cpp::handsThrowMessageHook`, constructeur Steam RVA `0xB1FE40`, appel de libération vérifié à retour `0xD6B937`, joueur local et type 5 |
| Origine / vitesse | Arguments `origin` et `initialSpeed` conservés tels quels ; seul l'axe est remplacé |
| Durée de la demande | `GrenadeThrowAim` expire après 1 seconde ; une demande ne peut être consommée qu'une fois |
| Aperçu | `s.handInteraction.grenadeHeld=false` : l'ancien objet procédural est désactivé en gameplay |
| Disponibilité | Aucun lecteur publié du type d'équipement sélectionné, du nombre de charges ou du cooldown des grenades ; les lectures d'ammo existantes concernent les armes |
| Création / consommation | La commande d'équipement laisse DOOM préparer et libérer l'objet ; le constructeur du message est identifié, pas un point d'entrée validé permettant de créer/consommer immédiatement un équipement |

Changer les arguments du constructeur ne prouve pas qu'un appel direct
respecterait inventaire, cooldown, animation et propriétaire du projectile.
Ces contrats restent à vérifier en phase C ; aucun appel direct ni bypass
de consommation n'est ajouté.

## Modèles et types retrouvés

| Export | Identification native |
|---|---|
| `grenade_frag.cast` | Modèle tenu de la grenade fragmentaire campagne ; premier atelier |
| `grenade_siphon.cast` | Modèle tenu de la grenade siphon campagne ; deux meshes, distinct de la frag |
| `grenade.cast` | Modèle générique hérité par les définitions ; pas la preuve d'un troisième équipement sélectionnable |

Les déclarations SP frag et siphon spécifient respectivement un cooldown de
base de 20 000 et 15 000 ms, `removeOnUse=false`, et leurs projectiles natifs.
Ces valeurs déclaratives ne sont pas des lectures de cooldown restant ni la
preuve qu'aucun perk ne les modifie. `striker_grenade` existe aussi mais porte
`EQUIP_NONE` ; ne pas le traiter comme équipement disponible sur cette seule
base. L'hologramme possède des ressources locales, mais son raccord à
l'inventaire de campagne n'est pas établi dans cette analyse. La sélection
réelle en session reste à identifier ; aucune généralisation des modèles.

## Délai de deux secondes : cause non démontrée

Le code du geste n'ajoute pas d'attente de deux secondes après relâchement.
Il envoie l'impulsion d'équipement, puis conserve le déroulement moteur.
Les journaux existants inspectés contiennent onze lignes `[GRENADE-INPUT]`
dans `build/perf-analysis/user-KHARVOX.log`, mais aucune ligne
`[GRENADE-NATIVE]`. Le dernier `%TEMP%/KHARVOX.log` n'en contient pas non plus.
Il est donc impossible d'en déduire la durée entre relâchement et projectile.

Hypothèses restantes : préparation/animation native, activation refusée par
le cooldown, ou demande de direction expirée avant la libération. L'expiration
d'une seconde peut faire perdre l'orientation ; elle ne prouve pas la cause
du délai rapporté. Ni `uncookedDetonationDelay=780` ni les durées de vie de
projectiles ne doivent être assimilés à un retard de départ.

Après validation Blender seulement : examiner le raccord minimal au chemin
natif, origine depuis la grenade tenue et vitesse réelle du geste, continuité
visuelle, respect des charges/cooldown/équipements et bouton classique, puis
tests indépendants une/deux mains et stéréoscopie. Aucune de ces fonctions
n'est livrée comme réalisée dans l'atelier Blender.
