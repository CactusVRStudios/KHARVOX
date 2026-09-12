"""CPU pose-chain diagnostics; never claims GPU image provenance from timestamps."""
import argparse
import csv
import json
import math
from collections import Counter
from pathlib import Path


def rotation_error(a, b):
    na = math.sqrt(sum(x*x for x in a))
    nb = math.sqrt(sum(x*x for x in b))
    if na == 0 or nb == 0:
        return None
    dot = abs(sum(x*y for x, y in zip(a, b))/(na*nb))
    return math.degrees(2*math.acos(min(1., dot)))


def rotate_basis(b, yaw, pitch, roll):
    b = [row[:] for row in b]
    for i, j, angle in [(0, 1, -yaw), (0, 2, pitch), (1, 2, -roll)]:
        c, s = math.cos(math.radians(angle)), math.sin(math.radians(angle))
        u, v = b[i][:], b[j][:]
        b[i] = [c*x+s*y for x, y in zip(u, v)]
        b[j] = [-s*x+c*y for x, y in zip(u, v)]
    return b


def analyze(rows, meta):
    counts = Counter()
    producer_status = Counter()
    source_windows, source_resolved = Counter(), Counter()
    weapon_root, weapon_prop = Counter(), Counter()
    weapon_pairs = {}
    weapon_pair_errors = {20: 0., 21: 0.}
    weapon_pair_matches = Counter()
    weapon_missing_cameras = 0
    head, base, anchors, caches, programs, cameras = {}, {}, {}, {}, {}, {}
    quad_groups = {}
    matrix_error = body_offset = submit_position = submit_angle = 0.
    matrix_pairs = cache_candidates = missing_candidates = submit_pairs = missing_caches = 0
    ages = []
    publications = {}
    eye_cameras, eye_baselines = {}, []
    controller_turn_error = controller_pending_delta = 0.
    controller_turn_samples = 0
    draw_status, source_domains = Counter(), Counter()
    draw_matrix_error = draw_camera_error = 0.
    camera_domains, producer_window, taken_window = {}, set(), set()
    cinematic_cache_count = cinematic_eye_mismatch = cinematic_pose_mismatch = 0
    for e in rows:
        k = int(e['kind']); counts[k] += 1
        qpc = int(e['qpc']); eye = int(e['eye']); pid = int(e.get('poseId', 0))
        d = [float(e['d'+str(i)]) for i in range(16)]
        key = (int(e['thread']), int(e['source']), pid)
        if k == 8:
            taken_window, producer_window = producer_window, set()
        elif k == 15 and int(e['status']) == 6:
            producer_window.add((pid, eye, camera_domains.get((pid, eye), -1)))
        elif k == 3 and int(e['flags']) & 4:
            camera_domains[(pid, eye)] = 1 if int(e['flags']) & 2 else 0
        elif k == 4 and len(taken_window) == 1:
            source_pid, source_eye, source_domain = next(iter(taken_window))
            if source_domain == 1:
                cinematic_cache_count += 1
                cinematic_eye_mismatch += source_eye != eye
                cinematic_pose_mismatch += source_pid != pid
        if k == 11:
            publications[pid] = qpc
        elif k == 23:
            controller_turn_samples += 1
            controller_turn_error = max(controller_turn_error, abs(d[0]-d[3]))
            controller_pending_delta = max(controller_pending_delta, abs(d[1]-d[3]))
        elif k == 24:
            draw_status[int(e['status'])] += 1
            draw_matrix_error = max(draw_matrix_error, d[12])
            draw_camera_error = max(draw_camera_error, d[13])
        elif k == 15:
            producer_status[int(e['status'])] += 1
        elif k == 18:
            source_windows[int(e['status'])] += 1
            source_domains[int(e['flags'])] += 1
        elif k == 19:
            source_resolved[int(e['status'])] += 1
        elif k in (20, 21):
            status = int(e['status'])
            (weapon_root if k == 20 else weapon_prop)[status] += 1
            if status in (1, 2):
                if (eye, pid) not in cameras:
                    weapon_missing_cameras += 1
                pair_key = (k, int(e['source']), pid, int(e['revision']), int(e['flags']))
                if status == 1:
                    weapon_pairs[pair_key] = (eye, d[:12])
                elif pair_key in weapon_pairs and weapon_pairs[pair_key][0] != eye:
                    weapon_pair_matches[k] += 1
                    weapon_pair_errors[k] = max(weapon_pair_errors[k], max(abs(a-b) for a,b in zip(weapon_pairs[pair_key][1], d[:12])))
        elif k == 9:
            head[key] = d
        elif k == 10:
            base[key] = d
        elif k == 13:
            anchors[key] = d
        elif k == 2:
            programs[(eye, pid)] = qpc
        elif k == 3 and pid:
            cameras[(eye, pid)] = qpc
            if int(e['flags']) & 4 and eye in (0, 1):
                # r264 explicitly identifies anatomical AER eyes. Older/native
                # traces do not provide that guarantee; do not reinterpret them.
                pair_key = (int(e['source']), pid, int(e['flags']) >> 8)
                pair = eye_cameras.setdefault(pair_key, {})
                pair[eye] = d[:12]
                if len(pair) == 2:
                    left, right = pair[0], pair[1]
                    eye_baselines.append(sum((left[i]-right[i])*left[6+i] for i in range(3)))
                    del eye_cameras[pair_key]
            if pid in publications:
                ages.append(1000*(qpc-publications[pid])/meta['qpcFrequency'])
            if key in head and key in base and int(e['flags']) & 1:
                h, b = head[key], base.pop(key)
                expected = rotate_basis([b[3:6], b[6:9], b[9:12]], -(h[0]+h[6]), h[1], h[2])
                expected = rotate_basis(expected, -b[12], b[13], 0)
                matrix_error = max(matrix_error, max(abs(x-y) for x,y in zip(sum(expected, []), d[3:12])))
                matrix_pairs += 1
                if key in anchors:
                    body_offset = max(body_offset, math.dist(b[:3], anchors[key][:3]))
        elif k == 4:
            caches[(eye, int(e['revision']))] = (pid, d)
            start = programs.get((eye, pid))
            observed = cameras.get((eye, pid))
            if start is not None and observed is not None and start <= observed <= qpc:
                cache_candidates += 1
            else:
                missing_candidates += 1
        elif k == 5 and not int(e['flags']) & 1:
            cached = caches.get((eye, int(e['revision'])))
            if cached and cached[0] == pid and pid:
                submit_pairs += 1
                submit_position = max(submit_position, math.dist(cached[1][:3], d[:3]))
                angle = rotation_error(cached[1][3:7], d[3:7])
                if angle is not None:
                    submit_angle = max(submit_angle, angle)
            else:
                missing_caches += 1
        elif k == 12 and int(e['status']):
            # Separate anchors/recenters instead of interpreting them as drift.
            space = int(e['source'])
            group = quad_groups.setdefault(space, [])
            group.append(d[:7])
    quads = []
    for space, poses in quad_groups.items():
        origin = poses[0]
        quads.append({'space': space, 'samples': len(poses),
                      'max_position_delta_m': max(math.dist(origin[:3], p[:3]) for p in poses),
                      'max_rotation_delta_deg': max(rotation_error(origin[3:7], p[3:7]) or 0 for p in poses),
                      'note': 'Anchor reacquisition/recenter within capture may cause legitimate changes.'})
    return {'schema': 2, 'cpu_only': True, 'gpu_image_provenance_verified': False,
            'dropped': meta.get('dropped'), 'event_counts': dict(counts),
            'aer_anatomical_eye_pairs': len(eye_baselines),
            'aer_eye_baseline_left_axis_units_min': min(eye_baselines) if eye_baselines else None,
            'aer_eye_baseline_left_axis_units_max': max(eye_baselines) if eye_baselines else None,
            'aer_reversed_eye_baseline_pairs': sum(b < -1e-5 for b in eye_baselines),
            'controller_turn_frame_samples': controller_turn_samples,
            'aer_source_domain_counts': dict(source_domains),
            'weapon_draw_model_status_counts': dict(draw_status),
            'cinematic_producer_cache_observations': cinematic_cache_count,
            'cinematic_producer_cache_eye_mismatches': cinematic_eye_mismatch,
            'cinematic_producer_cache_pose_mismatches': cinematic_pose_mismatch,
            'cinematic_producer_cache_limitation': 'CPU producer window grouped at PresentBegin, not independent GPU image provenance.',
            'weapon_draw_cached_matrix_delta_max': draw_matrix_error if draw_status else None,
            'weapon_draw_auxiliary_camera_delta_max': draw_camera_error if draw_status else None,
            'weapon_draw_model_legend': {'1': 'matching placement/matrix', '2': 'call-local source/matrix correction',
                '3': 'matching weapon but draw-camera target missing', '4': 'ambiguous model identity', '5': 'unreadable or invalid model', '6': 'placement derived from recorded controller frames'},
            'controller_vs_head_turn_deg_max': controller_turn_error if controller_turn_samples else None,
            'pending_vs_published_turn_deg_max': controller_pending_delta if controller_turn_samples else None,
            'world_producer_status_counts': dict(producer_status),
            'aer_source_window_status_counts': dict(source_windows),
            'aer_source_resolved_status_counts': dict(source_resolved),
            'aer_source_window_legend': {'0': 'missing', '1': 'unique source', '2': 'ambiguous'},
            'aer_source_resolved_legend': {'0': 'held previous pair / priming', '1': 'bound source inputs'},
            'weapon_source_root_status_counts': dict(weapon_root),
            'weapon_source_prop_status_counts': dict(weapon_prop),
            'weapon_source_status_legend': {'0': 'source unavailable; native transform retained',
                                           '1': 'captured source transform', '2': 'reused matching eye-pair transform'},
            'weapon_source_without_observed_camera': weapon_missing_cameras,
            'weapon_source_matched_pairs': dict(weapon_pair_matches),
            'weapon_source_max_pair_component_error': {k: weapon_pair_errors[k] if weapon_pair_matches[k] else None for k in (20, 21)},
            'world_producer_status_legend': {'0': 'unrecognized source', '1': 'target missing or changed',
                '2': 'already aligned', '3': 'updated and read back', '4': 'ambiguous source',
                '5': 'ineligible', '6': 'completed source observed (r262)', '7': 'producer changed source (r262)',
                '-1': 'not writable', '-2': 'write/readback failed'},
            'camera_matrix_pairs': matrix_pairs, 'max_matrix_component_error': matrix_error if matrix_pairs else None,
            'max_camera_base_vs_stable_body_units': body_offset if matrix_pairs else None,
            'head_publication_to_camera_ms_max': max(ages) if ages else None,
            'cache_with_matching_cpu_camera_candidate': cache_candidates,
            'cache_without_matching_cpu_camera_candidate': missing_candidates,
            'matched_aer_cache_submissions': submit_pairs, 'unmatched_aer_submissions': missing_caches,
            'max_cache_submit_position_delta_m': submit_position if submit_pairs else None,
            'max_cache_submit_rotation_delta_deg': submit_angle if submit_pairs else None,
            'fixed_quads': quads,
            'limitations': 'CPU observations and matching IDs do not prove which image a GPU job rendered. '
                           'Unmatched events may be capture-boundary loss or uninstrumented paths. '
                           'Native center/submission events are recorded; native GPU root provenance is not established.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='Directory containing events.csv and capture.json')
    args = parser.parse_args()
    meta = json.loads((args.capture/'capture.json').read_text())
    if meta.get('schema') != 2:
        parser.error('Requires r255 schema-2 capture; older traces lack pose IDs.')
    with (args.capture/'events.csv').open(newline='') as stream:
        result = analyze(csv.DictReader(stream), meta)
    output = args.capture/'analysis.json'
    output.write_text(json.dumps(result, indent=2)+'\n')
    print(output)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
