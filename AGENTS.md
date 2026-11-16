# DOOM VR PLUS — Instructions aux agents

## Avant de travailler

Lire `README.md`, `Docs/HANDOFF.md` et les instructions du sous-dossier concerné.
Examiner `git status` et préserver les modifications déjà présentes. Le fork
travaille sur `doom-vr-plus` ; `main` conserve la référence upstream.
Annoncer les chemins exacts avant de modifier des fichiers.

## Principes du projet

- Réutiliser les composants et scripts existants ; changements ciblés, pas de refonte inutile.
- Respecter le style C++/C#/Python du fichier et les conventions de matrices existantes.
- Distinguer les faits observés, les hypothèses et les validations humaines.
- Préserver SFS/AER, les commandes classiques et les mécanismes gauchers.
- Ne jamais modifier une pose Blender, un JSON ou un profil validé sans demande explicite.
- Ne jamais compenser une mauvaise conversion par un offset ou une rotation arbitraire.
- Ne pas supposer un facteur d'échelle commun à toutes les armes : vérifier joints, parents et repères natifs.
- Conserver la taille des mains et leurs transformations locales lors des changements de prise.
- Pas de captures automatiques en jeu, de lancement de DOOM ni d'actions externes sans autorisation.
- Une nouvelle arme se traite seule, puis attend une validation humaine en jeu.
- Conserver un paquet précédent pour le retour arrière.

## Confidentialité et ressources

Ne jamais publier de chemins personnels, préférences utilisateur, journaux,
identifiants, jetons ou dumps mémoire. Les captures publiques doivent utiliser
un chemin vide ou un exemple générique. Utiliser des chemins relatifs au dépôt
et des options configurables ; pas de dépendance au poste du mainteneur.

Les exports Praetor/grenades et les profils de shaders de jeu locaux ne sont
pas présents dans ce dépôt. Ne pas les ajouter aux commits ni aux Releases.
Une licence de code ne relicense pas ces ressources. Si une fixture manque,
indiquer précisément ce qui manque ; ne pas inventer ses valeurs ni prétendre
avoir testé le rendu correspondant.

## Validation

Exécuter les tests pertinents de CMake/CTest et les tests du launcher si ses
bindings, options, langues ou contrôles changent. Une correction de données
HUD utilise l'export et l'audit existants ; pas de rebuild C++ sans nécessité.
Les tests techniques ne remplacent pas la validation dans les deux yeux.
Documenter ce qui a réellement été testé et les limites restantes.

Pour le launcher, vérifier le français par défaut, le changement immédiat en
anglais, la mémorisation et l'absence de changement des options VR. La version
visible vient de l'assembly compilé. Le cadre GIF conserve sa transparence et
ses proportions ; son animation se suspend en jeu.

## Chantiers à préserver

Les dix armes et leur prise à une/deux mains ont été validées localement.
Le dernier placement HUD manuel est validé. La grenade en main gauche est
validée visuellement ; son raccord au projectile natif reste à faire et est
actuellement mis de côté. L'éclairage natif Praetor est également en attente.
Ne pas reprendre ces chantiers sans demande du mainteneur.

Ne pas développer de nouveau traceur/replay GPU générique, de shader
d'éclairage alternatif ou de pipeline de collision automatique par défaut.
Si la demande est limitée à un prototype ou à une durée, respecter cette
limite et signaler les blocages au lieu de multiplier les essais.

## Livraison

Fournir un bilan concis : cause confirmée, fichiers modifiés, tests, limites,
paquet de test et retour arrière. Les publications doivent mentionner la base
KHARVOX/CactusVRStudios et l'assistance IA d'OpenAI Codex. Ne pas publier ni
pousser des modifications supplémentaires sans autorisation explicite.
