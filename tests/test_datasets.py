#!/usr/bin/env python3
"""Phase 1 Gate Verification Suite: SNDlib Topologies, 172 Frozen Streams & SHA-256 Manifest."""

from __future__ import annotations

import csv
import random
import subprocess
import sys
import unittest
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import prepare_datasets as prep


def read_stream(csv_path: Path) -> list[tuple[int, int, int, str, str]]:
    with csv_path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != ["t", "src", "dst", "src_name", "dst_name"]:
            raise AssertionError(
                f"Unexpected CSV header in {csv_path}: {reader.fieldnames}"
            )
        rows = [
            (
                int(r["t"]),
                int(r["src"]),
                int(r["dst"]),
                r["src_name"],
                r["dst_name"],
            )
            for r in reader
        ]
    return rows


class Phase1DatasetGateTest(unittest.TestCase):
    def test_topologies_and_demands_valid(self) -> None:
        nobel = prep.parse_sndlib_xml(
            prep.TOPOLOGIES_DIR / "nobel-eu.xml", expected_nodes=28, expected_spans=41
        )
        germany = prep.parse_sndlib_xml(
            prep.TOPOLOGIES_DIR / "germany50.xml", expected_nodes=50, expected_spans=88
        )
        self.assertEqual(len(nobel.nodes), 28)
        self.assertEqual(len(nobel.spans), 41)
        self.assertEqual(len(germany.nodes), 50)
        self.assertEqual(len(germany.spans), 88)

        for span in (*nobel.spans, *germany.spans):
            self.assertGreater(span.length_km, 0.0)

        _, nobel_dem = prep.parse_sndlib_native_demands(
            prep.DEMANDS_DIR / "nobel-eu.txt", nobel.name_to_id
        )
        self.assertEqual(len(nobel_dem), 28 * 27 // 2)
        self.assertEqual(nobel_dem, nobel.xml_demands)

        daily_files = sorted(
            prep.GERMANY50_1D1M_DIR.glob("demandMatrix-germany50-DFN-1day-*.txt")
        )
        self.assertEqual(len(daily_files), 28)
        for day_file in daily_files:
            _, day_dem = prep.parse_sndlib_native_demands(day_file, germany.name_to_id)
            self.assertGreater(len(day_dem), 1000)
            self.assertGreater(sum(day_dem.values()), 0.0)

    def test_112_base_streams_and_mt19937_reproducibility(self) -> None:
        nobel = prep.parse_sndlib_xml(
            prep.TOPOLOGIES_DIR / "nobel-eu.xml", expected_nodes=28, expected_spans=41
        )
        germany = prep.parse_sndlib_xml(
            prep.TOPOLOGIES_DIR / "germany50.xml", expected_nodes=50, expected_spans=88
        )

        profiles = [
            ("nobel_eu_realistic", nobel),
            ("nobel_eu_uniform", nobel),
            ("germany50_1d1m", germany),
            ("germany50_uniform", germany),
        ]

        for prefix, topo in profiles:
            for seed in range(1, 29):
                path = prep.STREAMS_DIR / f"{prefix}_seed_{seed:02d}.csv"
                self.assertTrue(path.is_file(), f"Missing base stream: {path}")
                rows = read_stream(path)
                self.assertEqual(len(rows), 1000)
                for expected_t, (t, src, dst, src_name, dst_name) in enumerate(rows):
                    self.assertEqual(t, expected_t)
                    self.assertNotEqual(src, dst)
                    self.assertTrue(0 <= src < len(topo.nodes))
                    self.assertTrue(0 <= dst < len(topo.nodes))
                    self.assertEqual(topo.nodes[src].name, src_name)
                    self.assertEqual(topo.nodes[dst].name, dst_name)

        # Verify exact MT19937 uniform reproducibility and bidirectional Nobel-EU realistic flow.
        expected_unif_s1 = prep.sample_uniform_stream(28, 1000, seed=1)
        actual_unif_s1 = [
            (r[1], r[2])
            for r in read_stream(prep.STREAMS_DIR / "nobel_eu_uniform_seed_01.csv")
        ]
        self.assertEqual(actual_unif_s1, expected_unif_s1)

        nobel_real_s1 = [
            (r[1], r[2])
            for r in read_stream(prep.STREAMS_DIR / "nobel_eu_realistic_seed_01.csv")
        ]
        forward_dir = sum(1 for u, v in nobel_real_s1 if u < v)
        reverse_dir = sum(1 for u, v in nobel_real_s1 if u > v)
        self.assertGreater(forward_dir, 400)
        self.assertGreater(reverse_dir, 400)

    def test_60_exp3_permutations_multiset_and_boundary_orders(self) -> None:
        nobel = prep.parse_sndlib_xml(
            prep.TOPOLOGIES_DIR / "nobel-eu.xml", expected_nodes=28, expected_spans=41
        )
        germany = prep.parse_sndlib_xml(
            prep.TOPOLOGIES_DIR / "germany50.xml", expected_nodes=50, expected_spans=88
        )

        configs = [
            ("nobel_eu", "nobel_eu_realistic_seed_01.csv", nobel),
            ("germany50", "germany50_1d1m_seed_01.csv", germany),
        ]

        for prefix, base_csv, topo in configs:
            base_pairs = [
                (r[1], r[2]) for r in read_stream(prep.STREAMS_DIR / base_csv)
            ]
            base_multiset = Counter(base_pairs)
            shortest_km = prep.compute_all_pairs_shortest_km(topo)

            perm_labels = [f"perm_{i:02d}" for i in range(1, 29)] + [
                "perm_shortest_first",
                "perm_longest_first",
            ]
            self.assertEqual(len(perm_labels), 30)

            for idx, label in enumerate(perm_labels, start=1):
                perm_path = prep.EXP3_PERMS_DIR / f"{prefix}_{label}.csv"
                self.assertTrue(
                    perm_path.is_file(), f"Missing EXP-3 permutation: {perm_path}"
                )
                rows = read_stream(perm_path)
                self.assertEqual(len(rows), 1000)
                perm_pairs = [(r[1], r[2]) for r in rows]
                self.assertEqual(
                    Counter(perm_pairs),
                    base_multiset,
                    f"Multiset mismatch for {perm_path.name}",
                )

                if idx <= 28:
                    rng = random.Random(idx)
                    expected_shuffle = list(base_pairs)
                    rng.shuffle(expected_shuffle)
                    self.assertEqual(perm_pairs, expected_shuffle)

            short_pairs = [
                (r[1], r[2])
                for r in read_stream(
                    prep.EXP3_PERMS_DIR / f"{prefix}_perm_shortest_first.csv"
                )
            ]
            short_dists = [shortest_km[p] for p in short_pairs]
            self.assertEqual(short_dists, sorted(short_dists))

            long_pairs = [
                (r[1], r[2])
                for r in read_stream(
                    prep.EXP3_PERMS_DIR / f"{prefix}_perm_longest_first.csv"
                )
            ]
            long_dists = [shortest_km[p] for p in long_pairs]
            self.assertEqual(long_dists, sorted(long_dists, reverse=True))

    def test_sha256_manifest_cli_verification(self) -> None:
        manifest_path = prep.STREAMS_DIR / "manifest.sha256"
        self.assertTrue(manifest_path.is_file())
        lines = manifest_path.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(lines), 172)

        proc = subprocess.run(
            ["sha256sum", "-c", "manifest.sha256"],
            cwd=prep.STREAMS_DIR,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, msg=proc.stderr or proc.stdout)


if __name__ == "__main__":
    unittest.main()
