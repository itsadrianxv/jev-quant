import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('summary', Path(__file__).resolve().parents[2] / 'scripts/profiling/summarize_simnow.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class SummaryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.run_mode = None
        self.meta = {'healthy': True, 'overflow': 0, 'window_completed': True,
                     'samples': 1, 'new_samples': 1, 'start_ns': 100, 'window_ns': 100, 'grace_ns': 100}
        self.header = 'type,offset,order_id,ticker_id,source_id,evaluation_id,t1,t2,t3,t4,t5,t6,t7,t8\n'
        self.row = 'NEW,OPEN,1,0,1,1,110,120,130,140,150,160,170,180\n'

    def tearDown(self):
        self.temp.cleanup()

    def run_summary(self):
        (self.root / 'capture.json').write_text(json.dumps(self.meta))
        if self.run_mode:
            (self.root / 'run.json').write_text(json.dumps({'mode': self.run_mode}))
        (self.root / 'samples.csv').write_text(self.header + self.row)
        return module.summarize(self.root)

    def test_total_and_adjacent_metrics(self):
        metrics = self.run_summary()['groups']['NEW/OPEN']
        self.assertEqual(metrics['t8-t1']['p99'], 70)
        self.assertEqual(metrics['t5-t4']['p50'], 10)
        self.assertTrue(metrics['t8-t1']['tail_sample_warning'])

    def test_rejects_callgrind_overflow_and_incomplete_runs(self):
        for key, value in [('overflow', 1), ('window_completed', False), ('healthy', False)]:
            with self.subTest(key=key):
                saved = self.meta[key]
                self.meta[key] = value
                with self.assertRaises(ValueError): self.run_summary()
                self.meta[key] = saved
        self.run_mode = 'callgrind'
        with self.assertRaisesRegex(ValueError, 'Callgrind'):
            self.run_summary()

    def test_empty_capture_is_pending(self):
        self.row = ''
        self.meta.update(samples=0, new_samples=0)
        with self.assertRaisesRegex(ValueError, 'pending'): self.run_summary()

    def test_reversed_or_missing_boundaries_fail(self):
        self.row = self.row.replace('130,140', '0,140')
        with self.assertRaises(ValueError): self.run_summary()

    def test_duplicate_submissions_fail(self):
        self.row *= 2
        self.meta.update(samples=2, new_samples=2)
        with self.assertRaisesRegex(ValueError, 'Duplicate'): self.run_summary()


if __name__ == '__main__':
    unittest.main()
