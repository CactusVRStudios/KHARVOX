# Workflow HUD gauche / Praetor

État consigné le 16 novembre 2026. Les chemins ci-dessous partent du dossier
`KHARVOX_corrections`, sauf ceux des fichiers du dépôt explicitement préfixés
par `KHARVOX/`.

## Retouche utilisateur enregistrée à 20 h 34 : paquet revision-4

**Validation en jeu reçue le 16 novembre 2026 :** l'utilisateur confirme
« c parfait ça fonctionne ». Le placement HUD de `revision-4/test` devient
la référence validée pour la configuration testée par l'utilisateur.
Les rapports générés avant ce retour restent inchangés ; ils décrivent
la préparation du paquet, pas ce retour humain ultérieur. Ne pas déduire
de cette confirmation une validation distincte de tous les casques ou modes.

L'utilisateur a ajusté les blocs dans l'atelier puis enregistré
`build/hud-floating-preview/revision-3/retouche/HUD-Retouche.blend` et
`hud-floating.json` le 16 novembre 2026 à 20 h 34. Cette paire concordante
devient la source du prochain essai. `revision-4` la convertit sans aucune
redistribution automatique ni offset après sauvegarde. La scène et le JSON
sont préservés ; les 519 sources protégées sont également inchangées.

Lancer `build/hud-floating-preview/revision-4/test/LANCER_TEST_HUD.cmd`.
Les trois contrôles sur les configurations passent (transformation HUD,
geste et chaîne dimensionnelle avec dix armes). L'audit ne détecte aucune
intersection des rectangles avec l'armure dans la scène sauvegardée.
La DLL, les profils d'armes et la grenade visuelle sont identiques au paquet
précédent ; aucune recompilation ni capture automatique. L'espacement natif
en VR était encore à valider lors de la préparation ; le retour positif
est consigné ci-dessus. Les distances calculées pour
`revision-3` ci-dessous ne décrivent pas cette nouvelle pose manuelle.
Retour arrière : quitter DOOM et relancer `revision-3/test/LANCER_TEST_HUD.cmd`.

## Retour casque et correction d'espacement du 16 novembre 2026

Le retour utilisateur et sa photo confirment deux chevauchements : munitions
sur vie/armure et dernier rond jaune des options sur vie/armure. Le déplacement
de 3 cm de `revision-2` n'est donc pas validé. La photo identifie le problème,
mais ne constitue pas une calibration métrique de la projection du casque.

`revision-3/espacement-hud.py` réutilise la scène et le JSON de `revision-2`
en lecture seule. La vie/armure et l'ancrage restent fixes. Les munitions
sont centrées sous la vie ; les options sont placées à gauche des deux
canevas. Dimensions, rotations, profondeur normale et geste sont conservés.
Les translations sont calculées sur les enveloppes complètes des canevas,
avec une marge de 1 cm, et non estimées par un nouveau déplacement de 3 cm.

Repère de lecture de la vie, en centimètres :

| Bloc | Centre avant X / Y | Centre après X / Y |
|---|---|---|
| Vie / armure | 0 / 0 | 0 / 0 |
| Munitions | -11,0494 / -2,0276 | 0 / -18,0883 |
| Options | -7,8247 / 2,9353 | -26,0840 / 2,9353 |

339 séparations orthographiques obliques sont contrôlées jusqu'à 55°,
seuil de disparition existant : marge minimale 1 cm. Ce contrôle ne prouve
pas l'absence de chevauchement pour toute perspective ou animation SWF.
La hauteur Options reste provisoire dans Blender et dynamique en jeu.
L'espacement des canevas complets est volontairement conservateur ; les
pixels effectivement visibles peuvent occuper moins de place.

Scène dérivée : `build/hud-floating-preview/revision-3/HUD-Espacement-Corrige.blend`.
Conversion existante et audit dimensionnel réutilisés ; aucune intersection
avec l'armure détectée, écart d'enveloppe Blender/runtime `0,000009824 cm`.
Les 7 tests CTest ciblés et les 3 contrôles sur les configurations du paquet
passent, dont dix armes avec 27 cas synthétiques chacune.

Lancer `build/hud-floating-preview/revision-3/test/LANCER_TEST_HUD.cmd`.
La DLL est celle du paquet visuel grenade validé, sans recompilation ; seules
les translations HUD de `offhand_forearm_left.cfg` changent dans les données
fonctionnelles. La pose grenade et tous les profils d'armes sont conservés.
Les 519 sources protégées sont vérifiées. Le chantier lancer grenade est mis
de côté ; sa tenue visuelle validée reste disponible dans le paquet copié.

La vérification en jeu de `revision-3` reste à faire : consultation main
libre puis à deux mains, lecture et séparation des blocs dans les deux yeux.
Retour arrière : fermer DOOM puis relancer le paquet intact
`build/grenade-workshop/frag/conversion-1/test-visuel/LANCER_TEST_VISUEL.cmd`.
Rapports : `revision-3/diagnostic-espacement.json`, `comparaison.json` et
`validation.json`. Aucun jeu lancé ni capture automatique pour ce correctif.

## Références et validation

| Élément | Référence actuelle | Statut |
|---|---|---|
| Dix armes et poses Praetor à une/deux mains | `artifacts/KHARVOX-VR-Valide/` | Validées en jeu ; référence protégée |
| HUD suspendu, placement enregistré par l'utilisateur | `build/hud-floating-preview/HUD-Suspendu-Praetor.blend` et `hud-floating.json` | Sources originales conservées |
| HUD visible au-dessus du bras | `build/hud-floating-preview/revision-1/test/` | L'utilisateur confirme : « il n'est plus invisible » ; signale le chevauchement munitions/vie |
| Séparation des munitions de 3 cm | `build/hud-floating-preview/revision-2/HUD-Munitions-Separees.blend` et `hud-floating.json` | Insuffisante selon retour casque ; conservée pour historique |
| Dernier paquet HUD validé | `build/hud-floating-preview/revision-4/test/LANCER_TEST_HUD.cmd` | Pose utilisateur de 20 h 34 ; fonctionnement confirmé en jeu |

Ne pas remplacer le paquet des armes validées ni modifier les sources
artistiques pour consigner un nouveau retour. Garder le précédent paquet HUD
jusqu'à validation du suivant. Le dernier paquet préparé et la dernière
référence validée sont deux statuts distincts.

## Corrections et décisions conservées

| Sujet | Fait observé / travail effectué | Limite ou décision |
|---|---|---|
| Ancrage | Réutilisation de l'unique `HUD_anchor_left`, lié à `LeftForeArmRoll2`, avec transformations locales enregistrées | Ne pas ajouter un second ancrage ni un décalage contrôleur |
| Lecture et orientation | La scène a servi à régler la face de lecture et à effectuer la rotation de 180° dans le plan demandée par l'utilisateur | Conserver les matrices enregistrées, sans miroir ni échelle négative ; vérifier la lecture native au casque |
| Échelle / repères | Conversion vérifiée contre la géométrie Praetor et sa calibration existante ; pas de faute de facteur uniforme démontrée en main libre | Aucun coefficient HUD empirique ; dimensions de canevas distinctes des pixels visibles |
| Prise à deux mains | Le raccord HUD devait tenir compte du joint animé et de l'inverse bind employés pour la main rendue | Correction déjà présente dans la DLL HUD réutilisée, pas une nouvelle correction de `revision-2` |
| Consultation | Seuils relatifs au casque, hystérésis, temporisations et fondus existants conservés dans `offhand_hud_gesture.cfg` | Vérifications automatisées disponibles ; ne pas présenter chaque mode de jeu comme validé par ces seuls tests |
| Masquage par le bras | Le HUD natif est déjà dans l'image lorsque les Praetor sont dessinés au Present ; une séparation native simple n'est pas disponible dans ce chemin | Tracé/replay GPU et refonte de composition arrêtés ; aucun changement du pipeline dans les deux derniers paquets |
| Dégagement | Placement suspendu ajusté et enregistré dans Blender, puis converti sans déplacement supplémentaire | Visibilité confirmée par l'utilisateur pour `revision-1` ; ce placement ne garantit pas une absence universelle d'occlusion |
| Chevauchement munitions/vie | `revision-2` abaisse seulement `HUD_Munitions` de 3 cm suivant l'opposé de l'axe vertical local de `HUD_Vie_Armure` | Ajustement de disposition demandé, pas correction de calibration ; espacement natif à confirmer en jeu |

Dans `revision-2`, les lignes vie/armure et options de la configuration sont
strictement identiques à celles de `revision-1`. Dimensions, ancrage,
activation, profils d'armes, shaders et DLL sont conservés. La rotation des
munitions ne diffère que de l'arrondi numérique Blender (tolérance contrôlée
de `2e-7` sur ses composantes), sans rotation supplémentaire intentionnelle.

## Procédure simple

Pour le dernier essai, utiliser la paire scène/JSON de `revision-3/retouche` ; les
exemples `revision-2` ci-dessous documentent la conversion précédente.

1. **Choisir la bonne référence.** Pour reproduire le dernier paquet, utiliser
   ensemble le `.blend` et le JSON de `revision-2`, jamais un mélange de
   révisions. Pour un nouveau réglage, préserver cette paire et créer une
   nouvelle copie avant de travailler.
2. **Régler visuellement dans Blender et faire valider.** Le point d'entrée
   existant est `ATELIER_HUD_SUSPENDU.cmd`. Dans `N > KHARVOX`, sélectionner le
   HUD, utiliser `G` / `R` ou hauteur, déport et inclinaison. Après un réglage
   libre, « Utiliser ce placement comme base » permet de reprendre les
   curseurs. Vérifier la vue joueur, les deux yeux, la supination et le volume
   réel de l'armure. Conserver l'ancrage et les dimensions.
3. **Enregistrer la paire scène/JSON.** « Enregistrer la position HUD » écrit
   les matrices locales et la scène ; contrôler les chemins et le diagnostic
   avant conversion. Les matrices exactes sont la référence, pas les valeurs
   des curseurs reconstruites à la main.
4. **Convertir et contrôler.** Réutiliser `export_forearm_hud.py`, qui appelle
   `audit_hud_scale_chain.py`. Un contrôle de sources, d'ancrage ou d'échelle
   en échec bloque cet export ; ne pas le contourner par un offset.
5. **Préparer une copie indépendante du dernier paquet fonctionnel.** Remplacer
   uniquement les données HUD converties, adapter le chemin de DLL dans
   `KharvoxLayer.json`, et écrire notice, manifeste d'empreintes et rapport de
   validation. Une modification de disposition ne nécessite pas de compiler
   une nouvelle DLL. Vérifier l'identité des armes et des fichiers conservés.
6. **Tester puis consigner le retour humain.** Lancer le launcher de la copie,
   vérifier consultation main libre puis verrouillée, lecture et séparation
   des informations dans les deux yeux. Vérifier SFS/AER et le mode gaucher
   avant de revendiquer leur validation. Aucune capture automatique requise.

**Attention à l'atelier actuel :** `ATELIER_HUD_SUSPENDU.cmd` ouvre encore
`build/hud-floating-preview/HUD-Suspendu-Praetor.blend`. Son bouton
d'enregistrement écrit dans ce même dossier d'origine. Il ne sélectionne
pas automatiquement `revision-2`. La séparation des munitions appartient à
la copie dérivée ; réexporter simplement l'original ferait perdre cet
ajustement. Ne pas ouvrir cette copie avec le script d'enregistrement à
chemins fixes en supposant qu'il écrira dans la copie. Avant un prochain
réglage, préparer explicitement une destination distincte ; aucun changement
de l'atelier n'est effectué par cette mise à jour documentaire.

La conversion et les contrôles ci-dessous existent. La copie du paquet et
son manifeste ont été orchestrés séparément ; il n'existe pas encore de
commande unique d'empaquetage HUD. Ce document ne prétend pas l'ajouter.

## Conversion employée

Dans `export_forearm_hud.py::convert`, en centimètres :

```text
repère_ancrage_Grip_XR = native_to_grip_xr_cm
                      × bone_matrix_native_at_bind
                      × bone_to_anchor_bind
panneau_Grip_XR       = repère_ancrage_Grip_XR × matrix_anchor_local_cm
panneau_Grip_DOOM     = XR_to_DOOM × panneau_Grip_XR
```

Le repère reconstruit doit correspondre à `frame_in_grip_xr_cm`. XR utilise
droite/haut/arrière ; DOOM avant/gauche/haut. Le runtime convertit les
centimètres avec les unités par mètre du monde. L'échelle du modèle vient
des données Praetor existantes, pas d'un facteur d'arme supposé universel.

| Slot natif | Objet Blender | GUI | Dimensions |
|---|---|---|---|
| 0 | `HUD_Munitions` | WeaponInfo | 25 × 14,6484375 cm ; canevas 512 × 300 |
| 1 | `HUD_Vie_Armure` | BottomLeft | 25 × 14,6484375 cm ; canevas 512 × 300 |
| 2 | `HUD_Options` | ProgMeter | Largeur 20 cm ; hauteur native dynamique |

La hauteur Options dans Blender reste provisoire. Dessins, chiffres et icônes
Blender sont une reconstruction ; en jeu, les SWF natifs sont conservés.

Commande réellement utilisée pour `revision-2` depuis `KHARVOX_corrections`
(référence reproductible, **ne pas écraser les rapports archivés** ; utiliser
un nouveau `--out` pour une nouvelle exécution, préalablement créé) :

```powershell
python -B `
  'KHARVOX/tools/export_forearm_hud.py' `
  --source 'build/hud-floating-preview/revision-2/hud-floating.json' `
  --scene 'build/hud-floating-preview/revision-2/HUD-Munitions-Separees.blend' `
  --out 'build/hud-floating-preview/revision-2' `
  --runtime 'build/hud-floating-preview/revision-1/test'
```

Le convertisseur produit `offhand_forearm_left.cfg`,
`offhand_hud_gesture.cfg`, `conversion.json`, `comparaison.json` et
`comparaison.md`. La dérivation des munitions est consignée dans
`revision-2/separer-munitions.py` et `diagnostic-disposition.json` : elle
ouvre l'original sans le modifier et écrit seulement la nouvelle copie.

## Preuves et contrôles avant livraison

Rapport de référence :
`build/hud-floating-preview/revision-2/validation.json`.

- 7 tests CTest ciblés réussis : `hud-layout-policy`, `forearm-hud`,
  `hud-scale-chain`, `wrist-hud-gesture`, `hand-visibility-policy`,
  `weapon-support-policy`, `weapon-support-config`.
- 3 contrôles exécutés sur les configurations du paquet :
  `KharvoxForearmHudTests`, `KharvoxWristHudGestureTests`,
  `KharvoxHudScaleChainTests` ; ce dernier couvre les dix profils et 27 cas
  synthétiques de rotation/échelle/monde par profil.
- Aucun des trois rectangles n'intersecte le maillage d'armure dans la scène
  contrôlée. Écart des enveloppes Blender/runtime : environ `0,000009824 cm`.
  Cela ne mesure pas les limites des pixels SWF ni toutes les poses possibles.
- Vie/armure/options, geste et 512 sources protégées vérifiés ; paquet
  précédent conservé. Voir `source-manifest.json` et `fichiers-prevus.json`.
- DLL identique à `revision-1`, sans recompilation :
  `55830df0fc18bf2206f64573afb777249eee8bd317f44ff4ea54339e33f4756d`.
  Les travaux de traçage GPU arrêtés présents dans les sources ne doivent pas
  être inclus en reconstruisant aveuglément le projet pour un réglage HUD.

Ces résultats décrivent la préparation déjà réalisée, pas de nouveaux tests
en jeu. Le prochain retour attendu concerne seulement la séparation des
munitions et de la vie. Consigner ce retour ici sans réécrire les rapports
historiques qui portaient `game_validated: false` lors de la préparation.

Les empreintes du manifeste décrivent le paquet à sa préparation. Le launcher
peut ensuite réécrire `KharvoxLayer.json` et `two_hand_status.txt` pendant
l'utilisation : contrôler le contenu sémantique du manifeste de couche et
son chemin de DLL séparément des empreintes des fichiers fixes. La présence
de nouveaux journaux ou statuts ne constitue pas une validation visuelle du
HUD et ne justifie pas de réécrire les preuves historiques.

## Retour arrière et code concerné

Quitter DOOM, puis lancer
`build/hud-floating-preview/revision-1/test/KharvoxDesktop.exe` pour revenir
au HUD visible précédemment testé. La référence armes
`artifacts/KHARVOX-VR-Valide/` est également préservée. Ne pas mélanger les
DLL et configurations entre paquets.

Points existants à réutiliser pour la suite :

- `KHARVOX/tools/hud_left_workshop.py` : squelette, ancrage et export local.
- `KHARVOX/tools/hud_context_preview.py` : aperçu du geste et géométrie.
- `KHARVOX/tools/hud_floating_preview.py` : atelier suspendu et sauvegarde.
- `KHARVOX/tools/export_forearm_hud.py` et `audit_hud_scale_chain.py` :
  conversion et vérification de la chaîne dimensionnelle.
- `KHARVOX/src/hud/HudHook.cpp` : lecture des configurations,
  `contextualWristHudAlpha` et application aux GUI natifs.
- `KHARVOX/src/hud/ForearmHudPolicy.h` : `forearmHudTransform`,
  `forearmHudPixelAxis`, matrices et axes des pixels natifs.
- `KHARVOX/src/hud/WristHudGesturePolicy.h` : geste relatif au casque,
  hystérésis et fondus.

L'étude [HUD_RESTORATION.md](HUD_RESTORATION.md) reste un historique de la
restauration initiale ; ses anciens paquets et règles de dégagement ne
doivent pas remplacer les références courantes ci-dessus.
