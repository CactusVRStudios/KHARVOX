import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('pacing', Path(__file__).parents[1]/'tools/analyze_native_pacing_sequence.py')
pacing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pacing)


def row(frame, value):
    r = {k: value for k in pacing.TIMINGS + pacing.WORK}
    r.update(frame=frame, endNs=frame*20000000, threadAccounting=0, logAfterRow=0)
    return r


class SequenceTests(unittest.TestCase):
    def test_alternation_and_sparse_alias(self):
        rows = [row(f, 10000000 if f % 2 else 2000000) for f in range(1, 481)]
        result = pacing.analyze(rows)['all']
        self.assertAlmostEqual(result['fields']['rootNs']['lag1'], -1)
        self.assertAlmostEqual(result['fields']['rootNs']['lag2'], 1)
        sparse = pacing.analyze(rows[::120])['all']
        self.assertEqual(sparse['adjacentPairs'], 0)
        self.assertIsNone(sparse['fields']['rootNs']['lag1'])
        self.assertIsNone(sparse['fields']['rootNs']['evenMedian'])

    def test_stable_and_gap(self):
        result = pacing.analyze([row(1, 3), row(2, 3), row(9, 3)])['all']
        self.assertEqual(result['adjacentPairs'], 1)
        self.assertIsNone(result['fields']['rootNs']['lag1'])

    def test_excludes_known_overhead_neighbors(self):
        rows = [row(f, 1) for f in range(1, 7)]
        rows[1]['threadAccounting'] = 1
        rows[3]['logAfterRow'] = 1
        clean = pacing.analyze(rows)['withoutKnownDiagnosticNeighbors']
        self.assertEqual(clean['rows'], 2)  # frames4 and6 only

    def test_rejects_mixed_or_sparse_format(self):
        with self.assertRaises(ValueError):
            pacing.analyze([row(2, 1), row(2, 2)])
        with self.assertRaises(ValueError):
            pacing.read_rows(b'Native frame pacing frame=123 rootMs=5')

    def test_wait_tail_percentiles_and_slowest_frames(self):
        rows = [row(f, f*1000000) for f in range(1, 101)]
        original = [dict(r) for r in rows]
        result = pacing.analyze(rows)['all']
        for name in ('deviceIdleNs', 'inputWaitNs', 'xrCopyCompletionNs'):
            self.assertEqual(result['fields'][name]['median'], 50.5)
            self.assertEqual(result['fields'][name]['p95'], 95)
            self.assertEqual(result['fields'][name]['p99'], 99)
            self.assertEqual(result['fields'][name]['max'], 100)
        self.assertEqual([r['frame'] for r in result['slowestFrames']], list(range(100, 90, -1)))
        self.assertEqual(result['slowestFrames'][0], {
            'frame': 100, 'intervalMs': 100, 'deviceIdleMs': 100,
            'inputWaitMs': 100, 'xrCopyCompletionMs': 100})
        self.assertEqual(rows, original)

    def test_wait_tail_excludes_diagnostic_neighbors(self):
        rows = [row(f, 1000000) for f in range(1, 5)]
        rows[1]['logAfterRow'] = 1
        rows[2]['intervalNs'] = rows[2]['deviceIdleNs'] = 100000000
        result = pacing.analyze(rows)
        self.assertEqual(result['all']['fields']['deviceIdleNs']['p99'], 100)
        clean = result['withoutKnownDiagnosticNeighbors']
        self.assertEqual(clean['fields']['deviceIdleNs']['p99'], 1)
        self.assertNotIn(3, [r['frame'] for r in clean['slowestFrames']])
        self.assertEqual(pacing.analyze([])['all']['slowestFrames'], [])
        single = pacing.analyze([row(1, 1000000)])['all']['fields']['deviceIdleNs']
        self.assertEqual(single['median'], single['p99'])


if __name__ == '__main__':
    unittest.main()
