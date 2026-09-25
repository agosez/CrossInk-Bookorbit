"""Run with python3 -m unittest discover -s test/scripts (monitor dependencies required)."""
import os
import sys
import unittest
from pathlib import Path

os.environ.setdefault("MPLBACKEND", "Agg")
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import debugging_monitor as monitor


class MemoryLogsTest(unittest.TestCase):
    def test_crossink_combined_pools(self):
        line = "[MEM] Periodic: heap free=85000 total=270000 min=30000 maxAlloc=49000 psram free=7000000 total=8388608 min=6000000 maxAlloc=6500000"
        self.assertEqual(monitor.parse_memory_samples(line), [
            ("heap", (85000, 270000, 49000)),
            ("psram", (7000000, 8388608, 6500000)),
        ])

    def test_c3_heap_only(self):
        self.assertEqual(monitor.parse_memory_samples("[MEM] Boot: heap free=1 total=2 min=0 maxAlloc=1"),
                         [("heap", (1, 2, 1))])

    def test_legacy_separate_lines(self):
        stats = "Free: 100 bytes, Total: 200 bytes, Min Free: 50 bytes, MaxAlloc: 80 bytes"
        self.assertEqual(monitor.parse_memory_samples("[MEM] " + stats), [("heap", (100, 200, 80))])
        self.assertEqual(monitor.parse_memory_samples("[MEM] PSRAM: " + stats), [("psram", (100, 200, 80))])

    def test_missing_values_do_not_become_zero(self):
        self.assertEqual(monitor.parse_memory_samples("[MEM] no statistics"), [("heap", (None, None, None))])

    def test_psram_creates_separate_subplot(self):
        monitor.shutdown_event.clear()
        for data in (monitor.time_data, monitor.free_mem_data, monitor.total_mem_data, monitor.max_alloc_data,
                     monitor.psram_time_data, monitor.psram_free_mem_data, monitor.psram_total_mem_data,
                     monitor.psram_max_alloc_data):
            data.clear()
        monitor.time_data.append("12:00:00")
        monitor.free_mem_data.append(85)
        monitor.total_mem_data.append(270)
        monitor.max_alloc_data.append(49)
        monitor.update_graph(0)
        self.assertEqual(len(monitor.plt.gcf().axes), 1)
        monitor.psram_time_data.append("12:00:00")
        monitor.psram_free_mem_data.append(7000)
        monitor.psram_total_mem_data.append(8192)
        monitor.psram_max_alloc_data.append(6500)
        monitor.update_graph(1)
        self.assertEqual(len(monitor.plt.gcf().axes), 2)
        monitor.plt.close("all")
