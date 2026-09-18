# SFS projection audit, Test 19 / 305016e

Nicht alle Shader sind vollstaendig VR-korrekt. Dieser Audit umfasst den gesamten
lokal vorhandenen Dump, nicht alle moeglichen Shader jeder Map, Treiberversion
oder GPU. Keine neue Headset-Aufnahme lag vor. Test 19 wurde nicht veraendert.

## Umfang und Ergebnis

- 647 Originalmodule: 219 Vertex, 393 Fragment, 35 Compute.
- 75 Profilmodule: 61 Vertex, 12 Fragment, 2 Compute.
- 1.395 Compile-Pruefungen: Original-Stereo, 612 grafische Mono-Varianten,
  Profil-Stereo und 61 Profil-Shadow-Klassifikationen. Alle erfolgreich.
- 257 von 280 Vertex-Pruefungen im Kamera-Modus erhalten Augenprojektion.
  Die 23 anderen geben direkte Clip-/Bildschirmkoordinaten oder Atlaspositionen
  aus oder sind deaktiviert. Das ist kein Beweis ihrer Laufzeit-Passzuordnung.
- Keine zusaetzliche Vertex-Augenprojektion in den 612 Mono-Pruefungen.
- Die Pipeline-Schattenklassifikation bleibt eine Laufzeitvoraussetzung.
  Profil-Shadow-Pruefungen testen beide Optionen, nicht die Verwendung jeder
  Variante in jeder Map. Gemeinsame Licht-Schattenkarten brauchen keine IPD.

## 1. Bestaetigte Korrekturluecke: zehn Lichtshader

`DoomLighting.h` verlangt fuer die Cluster-Korrektur wortwoertlich
`inputs.fragCoord.z`. Diese Shader nutzen `lightInput.fragCoord.z`:

```
14c518d2078875da 173d9b6441c449ef 210565d183f1a3bd 4b379e1cd00db0c8
5fbd14d70b106a99 602560c5529305c2 906a05b56812c74c 959a39cffe62e669
a3f236a490124edc e150f8ecb97fbf23
```

Sie bestimmen Cluster aus Augen-Pixelkoordinaten, greifen aber auf zentrale
Clusterlisten zu, ohne die inverse Augenprojektion anzuwenden. Beispiel:
14c518d2078875da, generierte GLSL Zeilen 582-595. Mehrere dieser Module sind
im vorhandenen Test-16-Log tatsaechlich als `clusters=0` protokolliert.
Das ist ein plausibler Kandidat fuer blick-/distanzabhaengige Beleuchtung,
aber keine bewiesene Zuordnung zur gemeldeten haengenden Kiste.

Vier andere Shader (910831602077697, 9367a9623f586f04, 9cb73473d832dae5,
a16334b8c3ddf8ed) bestimmen Cluster direkt aus Weltposition und zentraler VP.
Dort ist die zentrale Projektion beabsichtigt und darf nicht blind ersetzt werden.

## 2. Bestaetigte Inkonsistenz: Partikel-Tiefenkollision

Compute-Modul `53feb817281c0c4e` berechnet `screenPosition` mit der zentralen
View-Projection (GLSL 554-566). Damit tastet `DoCollisionTest` die auf Array
umgestellte Tiefentextur ab; der nicht duplizierte Compute-Pass waehlt Layer 0.
Dieser Layer enthaelt die Augenprojektion, nicht die zentrale Projektion.
Normalenabfrage und Sichtbarkeitsgrenzen verwenden dieselben zentralen Koordinaten.
Das Modul ist im Test-16-Log mit `stereoCompute=0` vorhanden.

Die Simulation darf nicht einfach zweimal ausgefuehrt werden: sie veraendert
gemeinsame Partikel-/Indirect-Buffer. Eine Korrektur muss ihre einmalige Ausfuehrung
erhalten und Projektion, Bounds und Abtast-Layer konsistent machen.
Dies belegt noch keine Ursache des gemeldeten einseitigen Funken-Effekts.

## 3. SSR nicht VR-fertig, standardmaessig abgeschaltet

Sechs Originalmodule benutzen unkonvertierte packed inverse/forward projection
und vorherige World-to-Window-Matrizen fuer Screen-Space-Reflexionen:

```
4cb086c1fe9d19d2 53d224a2adb64cbf 7a3ba2e56552169
8acbfca81ef106c4 b569af0a92654394 feda92e82d39ba6
```

Zusaetzlich setzt die Profilvariante `7a3ba2e56552169_8be3d2b2be954095`
`scene.world_pos` auf Null. Das ist keine korrekte VR-Rekonstruktion.
Die aktuellen Startup-Kontrollen setzen `r_SSR=0`; daher sind diese Pfade im
vorgesehenen Betrieb deaktiviert. Ein aktivierter SSR-Modus ist nicht freigegeben.

## Weitere gepruefte Pfade

Die vorhandenen Frustum-Rekonstruktionen erhalten die inverse Augenprojektion;
die erkannten vorherigen Projektionen erhalten die entsprechende Rueckprojektion.
SSDO-Packed-Projektion wird in beide Richtungen konvertiert. Die vier gefundenen
Refraction-Matrixfamilien werden auf Augenfensterkoordinaten umgerechnet.
Verbliebene TV-Stereo-Zwischenberechnungen in drei Profil-Fragmentvarianten sind
nach Entfernen ihrer Positionskorrektur unbenutzt; nicht nochmals aufaddieren.

HUD-Familien bleiben ueber Shader-Identitaet und Clip-W klassifiziert. Das ist
keine allgemeine semantische Erkennung jedes HUD-Elements. Die konkrete
Praetor-Einblendung sowie Bodennähte und Waffenflackern sind dadurch nicht bewiesen
behoben. Kameraparameter, Culling, Query-Verhalten, Uniform-Zeitpunkt und dynamische
Passauswahl koennen durch diesen statischen Shader-Audit nicht ausgeschlossen werden.

## Reproduzierbarkeit

`tools/audit_sfs_projection.py` erzeugt `results.json` mit jedem Eingang, Modus,
Shader-Stage, Compile-Status, Projektionsmarker und Pfad zur generierten GLSL.
Die JSON-Datei und alle 1.395 generierten GLSL/SPIR-V-Paare liegen neben diesem
Bericht. Der Audit aendert weder Profilshader noch Laufzeitpakete.
