"""Analyze consecutive coarse samples without adding nested timing spans."""
import argparse
import csv
import hashlib
import io
import json
import math
import statistics
from pathlib import Path

TIMINGS = ('intervalNs', 'rootNs', 'originalNs', 'doubledNs', 'deviceIdleNs',
           'xrWaitFrameNs', 'xrCopyCompletionNs', 'xrEndFrameNs', 'inputWaitNs',
           'xrQueueWaitNs')
WORK = ('vertexBindings', 'passes', 'memoHits', 'memoMisses', 'copiedBytes',
        'seededBytes', 'seededBuffers')


def correlation(pairs):
    if len(pairs) < 3:
        return None
    a, b = zip(*pairs)
    ma, mb = statistics.mean(a), statistics.mean(b)
    aa = sum((x-ma)**2 for x in a)
    bb = sum((y-mb)**2 for y in b)
    return sum((x-ma)*(y-mb) for x, y in pairs) / math.sqrt(aa*bb) if aa and bb else None


def summary(rows):
    indexed = {r['frame']: r for r in rows}
    if len(indexed) != len(rows):
        raise ValueError('Duplicate frame serials; do not combine processes/runs')
    result = {'rows': len(rows), 'adjacentPairs': sum(r['frame']-1 in indexed for r in rows)}
    result['fields'] = fields = {}
    for key in TIMINGS + WORK:
        if not rows:
            continue
        scale = 1e6 if key.endswith('Ns') else 1
        values = sorted(r[key]/scale for r in rows)
        even = [r[key]/scale for r in rows if r['frame'] % 2 == 0]
        odd = [r[key]/scale for r in rows if r['frame'] % 2]
        fields[key] = {
            'unit': 'ms' if scale == 1e6 else 'count_or_bytes',
            'median': statistics.median(values),
            'p95': values[max(0, math.ceil(.95*len(values))-1)],
            'p99': values[max(0, math.ceil(.99*len(values))-1)],
            'max': values[-1],
            'evenRows': len(even), 'oddRows': len(odd),
            'evenMedian': statistics.median(even) if even else None,
            'oddMedian': statistics.median(odd) if odd else None,
            'lag1': correlation([(indexed[r['frame']-1][key], r[key]) for r in rows if r['frame']-1 in indexed]),
            'lag2': correlation([(indexed[r['frame']-2][key], r[key]) for r in rows if r['frame']-2 in indexed]),
        }
    result['rootVersusVertexBindings'] = correlation([(r['rootNs'], r['vertexBindings']) for r in rows])
    result['queueWaitVersusXrWait'] = correlation([(r['xrQueueWaitNs'], r['xrWaitFrameNs']) for r in rows])
    result['slowestFrames'] = [
        {'frame': r['frame'], 'intervalMs': r['intervalNs']/1e6,
         'deviceIdleMs': r['deviceIdleNs']/1e6, 'inputWaitMs': r['inputWaitNs']/1e6,
         'xrCopyCompletionMs': r['xrCopyCompletionNs']/1e6}
        for r in sorted(rows, key=lambda r: r['intervalNs'], reverse=True)[:10]
    ]
    return result


def analyze(rows):
    if any(b['frame'] <= a['frame'] or b['endNs'] <= a['endNs'] for a, b in zip(rows, rows[1:])):
        raise ValueError('Non-monotonic frame/timestamp sequence')
    indexed = {r['frame']: r for r in rows}
    valid = [r for r in rows if r['intervalNs'] > 0]
    # Existing logs happen after their row timestamp. Their cost belongs to
    # the next interval. Kernel accounting can affect its own/next frame.
    clean = [r for r in valid if not r['threadAccounting']
             and r['frame']-1 in indexed
             and not indexed[r['frame']-1]['threadAccounting']
             and not indexed[r['frame']-1]['logAfterRow']]
    return {'all': summary(valid), 'withoutKnownDiagnosticNeighbors': summary(clean),
            'limitations': ['CPU wall spans are nested; never sum them as GPU workload.',
                            'Lag uses actual serial differences, never row-number adjacency.',
                            'A negative lag1 and positive lag2 suggest alternation, not its cause.',
                            'Runtime name and ASW status must be taken from the matching run; this file does not measure them.']}


def read_rows(raw):
    text = '\n'.join(line for line in raw.decode('utf-8-sig').splitlines() if not line.startswith('#'))
    reader = csv.DictReader(io.StringIO(text), delimiter='\t')
    required = set(TIMINGS + WORK + ('frame', 'endNs', 'threadAccounting', 'logAfterRow'))
    if not required.issubset(reader.fieldnames or []):
        raise ValueError('Expected r157 consecutive sequence TSV, not a sparse pacing log')
    return [{k: int(v) for k, v in row.items()} for row in reader]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sequence', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    raw = args.sequence.read_bytes()
    result = {'source': str(args.sequence.resolve()), 'sha256': hashlib.sha256(raw).hexdigest(),
              **analyze(read_rows(raw))}
    text = json.dumps(result, indent=2)
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    else:
        print(text)


if __name__ == '__main__':
    main()
