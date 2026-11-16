import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "compare_textures", Path(__file__).parents[1] / "tools" / "compare_texture_diagnostics.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CaptureTests(unittest.TestCase):
    def capture(self, renderer, pid, requests):
        return (f"[VT-STATS] renderer={renderer} pid={pid} request_observations={requests} "
                "elapsed_ms=2000 observed_retries=2 retry_wait_ms_total=150\n"
                f"[TEXTURE-STATS] renderer={renderer} pid={pid} uploaded_mips=0:10,2:3,\n"
                f"[TEXTURE-CONTEXT] renderer={renderer} pid={pid} fsr_used=0\n")

    def test_latest_process_and_renderer(self):
        text = self.capture("SFS", 1, 100) + self.capture("AER", 2, 900)
        text += self.capture("SFS", 3, 20)
        capture = module.latest_capture(text, "SFS")
        self.assertEqual(capture["VT-STATS"]["request_observations"], "20")
        self.assertEqual(module.retry_mean(capture), "75.00")

    def test_incomplete_capture_is_explicit(self):
        with self.assertRaises(ValueError):
            module.latest_capture("", "AER")
        with self.assertRaises(ValueError):
            module.latest_capture("[TEXTURE-CONTEXT] renderer=SFS pid=3", "SFS")

    def test_no_retry_is_not_zero_latency(self):
        capture = module.latest_capture(self.capture("SFS", 3, 20), "SFS")
        capture["VT-STATS"]["observed_retries"] = "0"
        self.assertEqual(module.retry_mean(capture), "unavailable")
        output = module.comparison(capture, capture)
        self.assertIn("GPU residency", output)
        self.assertIn("unavailable", output)


if __name__ == "__main__":
    unittest.main()
