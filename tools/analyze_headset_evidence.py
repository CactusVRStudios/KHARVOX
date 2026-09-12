"""Read saved evidence only; never infer visual/headset acceptance from log success."""
import argparse
import csv
import hashlib
import json
import math
import ntpath
from pathlib import Path
import re


def normalized(value):
    return ntpath.normcase(ntpath.normpath(str(value)))


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def finite_vector(value, length):
    return (isinstance(value, list) and len(value) == length
            and all(type(x) in (int, float) and math.isfinite(x) for x in value))


def pair_errors(pair):
    errors = []
    if pair.get('source') != 'native-producer-pair-used-for-openxr-copy':
        errors.append('Unbekannte Capture-Quelle')
    if pair.get('xrProjectionAccepted') is not True or pair.get('gpuCompletionVerified') is not True:
        errors.append('XR-Abgabe/GPU-Fertigstellung nicht bestaetigt')
    frame = pair.get('frame')
    if type(frame) is not int or frame <= 0:
        errors.append('Ungueltige Frame-ID')
    if any(type(pair.get(key)) is not int or pair[key] <= 0 for key in ('generation', 'width', 'height')):
        errors.append('Ressourcengeneration oder Bildabmessungen fehlen/sind ungueltig')
    handles = [pair.get('leftImage'), pair.get('rightImage')]
    if any(type(x) is not int or x <= 0 for x in handles) or handles[0] == handles[1]:
        errors.append('Keine zwei unterschiedlichen Bildhandles')
    if not finite_vector(pair.get('headPosition'), 3) or not finite_vector(pair.get('headOrientation'), 4):
        errors.append('Kopfpose fehlt/enthaelt ungueltige Werte')
    elif abs(sum(x*x for x in pair['headOrientation']) - 1) > 0.02:
        errors.append('Kopfrotation ist nicht normiert')
    eyes = pair.get('eyes')
    if not isinstance(eyes, list) or len(eyes) != 2 or not all(isinstance(x, dict) for x in eyes):
        return errors + ['Augenpaar fehlt']
    for index, eye in enumerate(eyes):
        if eye.get('index') != index or eye.get('frame') != frame:
            errors.append('Augen-/Frame-Zuordnung widerspruechlich')
        # UNDEFINED and PREINITIALIZED cannot describe a completed submitted image.
        layout = eye.get('layout')
        if type(layout) is not int or layout not in (1, 2, 5, 6, 7, 1000001002):
            errors.append('Bildlayout nicht als fertiges Bild ausgewiesen')
        if not finite_vector(eye.get('position'), 3) or not finite_vector(eye.get('fov'), 4):
            errors.append('Augenpose/FOV fehlt oder ist ungueltig')
        elif not (eye['fov'][0] < eye['fov'][1] and eye['fov'][3] < eye['fov'][2]
                  and all(abs(x) < math.pi/2 for x in eye['fov'])):
            errors.append('FOV-Winkel sind widerspruechlich')
    return errors


def analyze(evidence):
    evidence = Path(evidence).resolve()
    context = json.loads((evidence / 'context.json').read_text(encoding='utf-8-sig'))
    runtime = normalized(context['Runtime'])
    result = dict(status='NICHT_PRUEFBAR', evidence=str(evidence), runtime=context['Runtime'],
                  verifiedFiles=0, associatedOwnerLog=False, runtimeName=None,
                  headsetAcceptance='AUSSTEHEND', framesSubmitted=[], capturePairs=0,
                  errors=[], notes=[], sessionStates=[], build=context.get('Build'))
    entries = {}
    with (evidence / 'manifest.csv').open(encoding='utf-8-sig', newline='') as stream:
        for row in csv.DictReader(stream):
            copy = Path(row['Copy']).resolve()
            if not copy.is_relative_to(evidence):
                result['errors'].append('Manifest-Kopie liegt ausserhalb der Sicherung')
                continue
            source = normalized(row['Source'])
            if source in entries:
                result['errors'].append('Doppelter Manifest-Quellpfad')
                continue
            if not copy.is_file() or digest(copy) != row['SHA256'].lower():
                result['errors'].append('Fehlende/geaenderte Sicherungsdatei: ' + row['Source'])
                continue
            entries[source] = copy
            result['verifiedFiles'] += 1
    if result['errors']:
        result['status'] = 'DATEN_UNGUELTIG'
        return result

    def local(name):
        return entries.get(normalized(ntpath.join(runtime, name)))

    def read(path):
        return path.read_text(encoding='utf-8-sig', errors='replace') if path else ''

    owner_candidates = [path for source, path in entries.items() if ntpath.basename(source) == 'kharvox.log']
    owner = ''
    for candidate in owner_candidates:
        text = read(candidate)
        starts = list(re.finditer(r'^\[KHARVOX\]\[LAUNCHER\] build=', text, re.M))
        if starts:
            text = text[starts[-1].start():]
        match = re.search(r'runtimeDir=(.*?) twoHandMode=', text)
        if match and normalized(match.group(1)) == runtime:
            owner = text
            result['associatedOwnerLog'] = True
    if not owner:
        result['notes'].append('Kein KHARVOX.log eindeutig diesem Runtime-Ordner zugeordnet; fremde/alte Logs ignoriert')
        return result
    else:
        names = re.findall(r'OpenXR runtime reported name=(.*?) version=', owner)
        result['runtimeName'] = names[-1] if names else None
        result['sessionStates'] = [int(x) for x in re.findall(r'\[XR\] State -> (\d+)', owner)]
        if result['runtimeName'] and 'simulator' in result['runtimeName'].lower():
            result['notes'].append('Simulator-Lauf: kein echter Headset-Test')

    native = read(local('native_stereo.log'))
    boundaries = list(re.finditer(r'^Native Stereo r\d+ installed;', native, re.M))
    if boundaries:
        native = native[boundaries[-1].start():]
    status = read(local('renderer_status.txt')).strip()
    result['rendererStatus'] = status
    selected = re.search(r'\brenderer=(NATIVE|AER|AFW)\b', owner)
    result['selectedRenderer'] = selected.group(1) if selected else None
    aer = bool(owner and (re.search(r'^Renderer: AER\b', status) or result['selectedRenderer'] == 'AER'))
    failure = 'NATIVE STEREO RESTART AER REQUIRED:' in native or bool(local('native_stereo_restart_aer.txt'))
    result['restartAerRequested'] = failure
    if aer:
        result['status'] = 'AER_RUECKFALL' if failure else 'AER_AKTIV'
        result['notes'].append('AER ist kein Native-Stereo-Nachweis; gespeicherte Native-Historie nicht als aktuellen Lauf bewerten')
        return result
    if not boundaries:
        result['notes'].append('Native-Installation im zugeordneten Runtime-Log nicht belegt')
    if failure:
        result['errors'].append('Native-Sicherheitsabbruch/AER-Neustart angefordert; kein Beweis eines GPU-Crashs')
    if 'VK_ERROR_DEVICE_LOST' in owner:
        result['errors'].append('Vulkan Device-Lost im zugeordneten Lauf protokolliert')
    submissions = re.findall(r'^Native XR submit frame=(\d+) projection=(\w+) views=(\d+) xrEndFrame=(-?\d+) leftImage=(0x[0-9a-fA-F]+) rightImage=(0x[0-9a-fA-F]+)', native, re.M)
    for frame, projection, views, end, left, right in submissions:
        if projection == 'true' and views == '2' and end == '0' and int(frame) > 0 and int(left, 16) and int(right, 16) and int(left, 16) != int(right, 16):
            result['framesSubmitted'].append(int(frame))
        else:
            result['errors'].append('Native-Abgabe mit ungueltigem Augenpaar oder XR-Ergebnis')
    snapshots = re.search(r'Native immutable frame inputs: .*readOnlyStorageBindings=([1-9]\d*) retiredAfterDeviceIdle=true', native)
    result['immutableInputsProven'] = bool(snapshots)
    result['ownerProjectionActiveProven'] = ('Native Stereo frame-root pair ACTIVE' in owner
                                             and 'Native XR projection surface ACTIVE' in owner)

    captures = sorted((source, copy) for source, copy in entries.items()
                      if normalized(ntpath.dirname(source)) == runtime
                      and re.fullmatch(r'native_(sequence_\d+_)?capture\.json', ntpath.basename(source)))
    if len(boundaries) > 1 and captures:
        result['notes'].append('Mehrere Installationen im gleichen Runtime-Ordner: Capture-Zuordnung nicht eindeutig; neue Testkopie verwenden')
        captures = []
    incomplete = False
    previous_frame = None
    for source, copy in captures:
        try:
            pair = json.loads(read(copy))
            if not isinstance(pair, dict):
                raise ValueError('not an object')
        except (ValueError, TypeError):
            incomplete = True
            result['notes'].append('Unvollstaendige/unlesbare Capture-Datei: ' + ntpath.basename(source))
            continue
        errors = pair_errors(pair)
        frame = pair.get('frame')
        if previous_frame is not None and type(frame) is int and frame <= previous_frame:
            errors.append('Capture-Frames nicht streng aufsteigend')
        if type(frame) is int:
            previous_frame = frame
        if errors:
            result['errors'].extend(ntpath.basename(source) + ': ' + message for message in errors)
        else:
            result['capturePairs'] += 1
    result['notes'].append('Bildinhalt, physische Augenreihenfolge, Komfort und Latenz werden nicht automatisch abgenommen')
    if context.get('Processes'):
        result['notes'].append('Live-Sicherung: nach Prozessende erneut sichern')
    if result['errors']:
        result['status'] = 'FEHLERBELEG'
    elif owner and boundaries and result['framesSubmitted'] and snapshots and result['ownerProjectionActiveProven'] and not incomplete:
        result['status'] = 'NATIVE_TECHNISCH_BELEGT'
    elif not result['framesSubmitted']:
        result['notes'].append('Keine passende Native-Abgabe belegt; Menue, fehlende Frames oder Fokusverlust sind moegliche Ursachen, kein automatischer Rendererfehler')
    if not result['ownerProjectionActiveProven']:
        result['notes'].append('Aktuelle KHARVOX-Projektionsmarker fehlen; fruehere Native-Logeintraege allein reichen nicht')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', required=True)
    parser.add_argument('--output', required=True, help='New directory; existing reports are never overwritten')
    args = parser.parse_args()
    result = analyze(args.evidence)
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=False)
    (output / 'report.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    lines = ['# Technische Auswertung', '', '**' + result['status'] + '**', '',
             'Headset-Abnahme: **AUSSTEHEND**. Dies ist keine visuelle Freigabe.', '',
             '- Runtime-Ordner: `' + result['runtime'] + '`',
             '- Verifizierte Dateien: ' + str(result['verifiedFiles']),
             '- Runtime-Name aus zugeordnetem Log: ' + str(result['runtimeName']),
             '- Protokollierte Native-Abgaben: ' + str(len(result['framesSubmitted'])),
             '- Gueltige Capture-Paare: ' + str(result['capturePairs']), '',
             '## Fehlerbelege', '']
    lines += ['- ' + x for x in result['errors']] or ['Keine im ausgewerteten Material festgestellt.']
    lines += ['', '## Grenzen und Hinweise', ''] + ['- ' + x for x in result['notes']]
    (output / 'REPORT.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(json.dumps(dict(status=result['status'], report=str(output / 'REPORT.md')), ensure_ascii=False))


if __name__ == '__main__':
    main()
