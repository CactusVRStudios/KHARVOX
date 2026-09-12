"""Offline classification tests; fixtures are retained outside the repository."""
import csv
import hashlib
import json
from pathlib import Path
import sys
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_headset_evidence import analyze

RUNTIME = r'D:\headset-fixture\r139'
OWNER_SOURCE = r'C:\Users\Bernd\AppData\Local\Temp\KHARVOX.log'
OWNER = ('[KHARVOX][LAUNCHER] build=r139 runtimeDir=' + RUNTIME + '\\ twoHandMode=gameplay-test renderer=NATIVE\n'
         '[KHARVOX][XR] OpenXR runtime reported name=OpenXR Simulator Runtime version=1.0.27\n'
         '[KHARVOX][XR] State -> 4\n'
         '[KHARVOX][XR] Native Stereo frame-root pair ACTIVE\n'
         '[KHARVOX][XR] Native XR projection surface ACTIVE\n')
NATIVE = ('Native Stereo r139 installed; experimental\n'
          'Native immutable frame inputs: frame=12 buffers=22 readOnlyStorageBindings=304 retiredAfterDeviceIdle=true\n'
          'Native XR submit frame=12 projection=true views=2 xrEndFrame=0 leftImage=0x11 rightImage=0x22\n')


def pair():
    return dict(source='native-producer-pair-used-for-openxr-copy', frame=12, generation=1, width=3072, height=1728,
                xrProjectionAccepted=True, gpuCompletionVerified=True, leftImage=17, rightImage=34,
                headPosition=[0, 1.7, 0], headOrientation=[0, 0, 0, 1],
                eyes=[dict(index=i, frame=12, layout=1000001002, position=[-.032 if i == 0 else .032, 1.7, 0],
                           fov=[-.9, .9, .9, -.9]) for i in range(2)])


def fixture(owner=OWNER, native=NATIVE, capture=None, extras=None, live=False):
    root = Path(r'D:\KHARVOX-backups\headset-acceptance-r139\analysis-tests') / uuid.uuid4().hex
    root.mkdir(parents=True, exist_ok=False)
    (root / 'context.json').write_text(json.dumps(dict(Runtime=RUNTIME, Processes=[dict(Id=1)] if live else [])), encoding='utf-8')
    files = {OWNER_SOURCE: owner, RUNTIME + '\\native_stereo.log': native}
    if capture is not None:
        files[RUNTIME + '\\native_sequence_0000_capture.json'] = capture if isinstance(capture, str) else json.dumps(capture)
    files.update(extras or {})
    rows = []
    for i, (source, content) in enumerate(files.items()):
        target = root / ('file-' + str(i))
        target.write_text(content, encoding='utf-8')
        rows.append(dict(Source=source, Copy=str(target), SHA256=hashlib.sha256(target.read_bytes()).hexdigest()))
    with (root / 'manifest.csv').open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=['Source', 'Copy', 'SHA256'])
        writer.writeheader()
        writer.writerows(rows)
    return root


class EvidenceClassification(unittest.TestCase):
    def test_success_without_capture_is_technical_only(self):
        r = analyze(fixture())
        self.assertEqual(r['status'], 'NATIVE_TECHNISCH_BELEGT')
        self.assertEqual(r['headsetAcceptance'], 'AUSSTEHEND')
        self.assertEqual(r['capturePairs'], 0)
        self.assertIn('Simulator', ' '.join(r['notes']))

    def test_valid_pair(self):
        r = analyze(fixture(capture=pair()))
        self.assertEqual(r['capturePairs'], 1)
        self.assertEqual(r['status'], 'NATIVE_TECHNISCH_BELEGT')

    def test_tampered_copy_is_invalid_evidence_not_renderer_failure(self):
        root = fixture()
        (root / 'file-0').write_text('tampered')
        self.assertEqual(analyze(root)['status'], 'DATEN_UNGUELTIG')

    def test_unrelated_old_failure_log_is_ignored(self):
        r = analyze(fixture(extras={r'C:\Temp\KHARVOX-failed-attempt-1.log': 'VK_ERROR_DEVICE_LOST'}))
        self.assertEqual(r['status'], 'NATIVE_TECHNISCH_BELEGT')

    def test_wrong_runtime_owner_never_claims_success_or_failure(self):
        r = analyze(fixture(owner=OWNER.replace('headset-fixture', 'another-run'), native=NATIVE + 'NATIVE STEREO RESTART AER REQUIRED: old'))
        self.assertEqual(r['status'], 'NICHT_PRUEFBAR')

    def test_last_owner_launch_must_match(self):
        r = analyze(fixture(owner=OWNER + OWNER.replace('headset-fixture', 'another-run')))
        self.assertEqual(r['status'], 'NICHT_PRUEFBAR')

    def test_focus_loss_without_projection_is_inconclusive(self):
        r = analyze(fixture(native='Native Stereo r139 installed; experimental\n'))
        self.assertEqual(r['status'], 'NICHT_PRUEFBAR')
        self.assertEqual(r['sessionStates'], [4])

    def test_old_native_success_in_reused_folder_needs_current_owner_markers(self):
        owner = OWNER.replace('Native Stereo frame-root pair ACTIVE', 'no current native pair')
        self.assertEqual(analyze(fixture(owner=owner))['status'], 'NICHT_PRUEFBAR')

    def test_safety_refusal_is_not_called_gpu_crash(self):
        r = analyze(fixture(native=NATIVE + 'NATIVE STEREO RESTART AER REQUIRED: missing source layout'))
        self.assertEqual(r['status'], 'FEHLERBELEG')
        self.assertIn('kein Beweis eines GPU-Crashs', r['errors'][0])

    def test_aer_fallback_does_not_count_as_native(self):
        r = analyze(fixture(extras={RUNTIME + '\\renderer_status.txt': 'Renderer: AER restart after Native Stereo refusal',
                                    RUNTIME + '\\native_stereo_restart_aer.txt': 'layout'}))
        self.assertEqual(r['status'], 'AER_RUECKFALL')
        self.assertEqual(r['framesSubmitted'], [])

    def test_manual_aer_ignores_native_history(self):
        r = analyze(fixture(owner=OWNER.replace('renderer=NATIVE', 'renderer=AER')))
        self.assertEqual(r['status'], 'AER_AKTIV')

    def test_identical_image_handles_are_rejected(self):
        p = pair(); p['rightImage'] = p['leftImage']
        self.assertEqual(analyze(fixture(capture=p))['status'], 'FEHLERBELEG')

    def test_handle_string_alias_is_rejected(self):
        native = NATIVE.replace('rightImage=0x22', 'rightImage=0x011')
        self.assertEqual(analyze(fixture(native=native))['status'], 'FEHLERBELEG')

    def test_cross_frame_eye_pair_is_rejected(self):
        p = pair(); p['eyes'][1]['frame'] = 11
        self.assertEqual(analyze(fixture(capture=p))['status'], 'FEHLERBELEG')

    def test_gpu_completion_is_required(self):
        p = pair(); p['gpuCompletionVerified'] = False
        self.assertEqual(analyze(fixture(capture=p))['status'], 'FEHLERBELEG')

    def test_resource_generation_and_dimensions_are_required(self):
        p = pair(); p['generation'] = 0; p['width'] = -1
        self.assertEqual(analyze(fixture(capture=p))['status'], 'FEHLERBELEG')

    def test_eye_order_and_fov_are_checked(self):
        p = pair(); p['eyes'][0]['index'] = 1; p['eyes'][1]['fov'] = [.9, -.9, .9, -.9]
        self.assertEqual(analyze(fixture(capture=p))['status'], 'FEHLERBELEG')

    def test_unknown_layout_and_nan_pose_are_rejected(self):
        p = pair(); p['eyes'][0]['layout'] = 999; p['headPosition'][0] = float('nan')
        self.assertEqual(analyze(fixture(capture=p))['status'], 'FEHLERBELEG')

    def test_incomplete_live_capture_is_not_a_renderer_failure(self):
        r = analyze(fixture(capture='{"frame":', live=True))
        self.assertEqual(r['status'], 'NICHT_PRUEFBAR')
        self.assertFalse(r['errors'])

    def test_old_native_installation_errors_do_not_poison_new_run(self):
        r = analyze(fixture(native=NATIVE + 'NATIVE STEREO RESTART AER REQUIRED: old\n' + NATIVE))
        self.assertEqual(r['status'], 'NATIVE_TECHNISCH_BELEGT')

    def test_multiple_installations_do_not_reuse_ambiguous_captures(self):
        r = analyze(fixture(native=NATIVE + NATIVE, capture=pair()))
        self.assertEqual(r['capturePairs'], 0)
        self.assertIn('Capture-Zuordnung nicht eindeutig', ' '.join(r['notes']))


if __name__ == '__main__':
    unittest.main()
