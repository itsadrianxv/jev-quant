"""Offline acceptance accounting for the real-Jev cycle runner."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    'cycle_runner', Path(__file__).resolve().parents[2] / 'scripts/simnow_cycle_test.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def console(cycles=20, final=0, failures=0):
    lines = []
    for cycle in range(cycles):
        direction = 1 if cycle % 2 else -1
        lines.extend([f'event=live_position_updated order_id={2 * cycle} position={direction}',
                      f'event=live_position_updated order_id={2 * cycle + 1} position=0'])
    lines.append(f'event=simnow_live_result market_ready=1 decisions=120 executions={2 * cycles} '
                 f'provider_failures={failures} final_position={final}')
    return '\n'.join(lines)


class CycleSummaryTest(unittest.TestCase):
    def test_twenty_filled_cycles_pass(self):
        self.assertTrue(runner.summarize(console(), 0)['passed'])

    def test_choices_without_fills_do_not_count(self):
        data = 'event=live_jev_decision evaluation_id=1 intent=1\n' * 40
        self.assertFalse(runner.summarize(data + console(cycles=0), 0)['passed'])

    def test_short_run_open_position_failure_or_missing_result_fail(self):
        for data, code in [(console(19), 0), (console(final=1), 0),
                           (console(failures=1), 0), (console(), 1), ('', 0)]:
            with self.subTest(data=data[-100:], code=code):
                self.assertFalse(runner.summarize(data, code)['passed'])

    def test_duplicate_fills_do_not_inflate_cycles(self):
        data = 'event=live_position_updated order_id=99 position=0\n' + console()
        self.assertFalse(runner.summarize(data, 0)['passed'])


if __name__ == '__main__':
    unittest.main()
