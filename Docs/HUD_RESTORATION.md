# HUD original et avant-bras Praetor

> Historique de la restauration initiale. Pour le HUD suspendu actuel, les
> corrections Blender/conversion, les paquets à conserver et le statut de
> validation, suivre [le workflow HUD](HUD_WORKFLOW.md). Les descriptions et
> paquets ci-dessous correspondent à une étape antérieure.

## Comparaison observee

References locales : depot KHARVOX, commit `76d728e`, fichiers
`config/offhand_hud_default.cfg`, `src/hud/HudHook.cpp`,
`src/hud/OffhandHudPolicy.h`. Le paquet original voisin
`../KHARVOX-v1.4a/offhand_hud.cfg` confirme les six calibrations.

| Parametre | Original | Paquet valide avant ce chantier |
|---|---|---|
| Life, Ammo, ProgMeter, deux lateralites | Six positions, rotations et echelles | Identiques |
| Echelle des six panneaux | 0.4 | 0.4 |
| Canevas natifs Life / Ammo | 512 x 300, largeur physique 25 cm | Identiques |
| ProgMeter | Largeur du contenu 20 cm, hauteur selon contenu natif | Identique |
| Ancrage | Grip de la main secondaire, espace monde publie au rendu | Grip libre ou poignet Praetor verrouille |
| Visibilite | Gameplay, suivi valide ; masque pendant sequences ; bord proche >= 4 cm, hysteresis 0.5 cm | Regles conservees |
| Degagement Praetor | Aucun | Enveloppes du modele + marge 2 cm |
| Orientation supplementaire | Aucune | Degagement vers le casque et retournement horizontal selon la face |

Le HUD utilise les dessins, materiaux, informations et animations SWF natifs.
Les positions sont en centimetres dans le repere Grip DOOM (avant/gauche/haut),
les rotations sont pitch/yaw/roll intrinseques. Le calcul original ajoute
14 cm sur l'axe gauche du Grip pour Life, -14 cm pour Ammo, 0 pour ProgMeter.
Le pivot graphique est le centre du canevas, calcule apres ses dimensions.
Les calibrations du HUD plat et de ses autres profils restent inchangees.

## Correction ciblee

`clearOriginalOffhandHud` conserve exactement les axes des pixels et leur
echelle. Le signe de la face du GUI natif, mesure avant son rattachement a la
main, determine un cote de lecture stable dans le repere du poignet. Seule
la translation normale necessaire pour sortir de l'enveloppe Praetor avec
la marge existante de 2 cm est appliquee. Le cote ne change plus quand le
casque passe de l'autre cote du bras. Aucun retournement horizontal n'est
applique au mode original, y compris au ProgMeter detache.

Les huit sections d'enveloppe deja calculees au chargement limitent le
degagement au volume sous le panneau. Le chemin conservateur par enveloppe
globale reste disponible sans ces donnees. Le suivi du poignet natif en
prise a deux mains reste celui valide precedemment. Les anciens fichiers
avec mode montre explicite gardent leur compatibilite ; le paquet de test
utilise les calibrations d'origine et le mode original deja actif.

## Verification et limites

Les tests couvrent les six calibrations, les deux lateralites, cinq
orientations du poignet, le Grip libre et le poignet natif verrouille,
les deux signes de face native : conservation des axes, translation
perpendiculaire minimale, separation des enveloppes, idempotence et
geometrie commune aux deux yeux. Le cas des sections distantes est teste.
Les profils d'armes, scenes Blender, GLB, morphs et shaders ne sont pas modifies.

Ces preuves portent sur les enveloppes geometriques et le code. Elles ne
remplacent pas la verification de lisibilite dans le casque : les pixels
opaques des SWF et le volume exact des armes animees ne sont pas disponibles
dans ce test isole. Aucune absence universelle d'occlusion par une arme
n'est affirmee. L'original reste consultable du cote de lecture de la main ;
son dos n'est pas automatiquement transforme en nouvel affichage.

Paquet valide conserve : `artifacts/KHARVOX-VR-Valide/`.
Paquet de test independant : `build/hud-restoration/test/`.
Retour arriere : quitter le jeu et relancer le launcher du paquet valide.
Ne pas melanger leurs DLL et configurations.

Validation humaine restante : Shotgun a une puis deux mains, relachement,
rotation paume/dos, verification dans chaque oeil et dans les modes SFS/AER.
Relever tout masquage par le fusil ou l'avant-bras. Aucun lancement de jeu
ni capture automatique ne fait partie de ce chantier.

Resultats du 15 novembre 2026 : compilation reussie, 153/153 tests CTest,
120 combinaisons nouvelles de calibration/orientation/ancrage/face,
controle GPU des transitions une/deux/une main pour les dix armes,
les deux yeux et les lateralites : reussis. Les six calibrations sont
identiques au paquet original local. Les 91 sources artistiques et les
202 fichiers non transitoires du paquet valide sont inchanges. Le fichier
`two_hand_status.txt` de ce dernier evolue avec la session DOOM deja ouverte.
Le paquet de test contient 198 fichiers strictement identiques ; seuls
la DLL, son chemin de chargement, son empreinte, le manifeste et la notice
changent. Aucun test du nouveau paquet en jeu n'a ete effectue ici.
