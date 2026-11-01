#!/usr/bin/env python3
"""Multi-Core Crash-Safe Resumable Experiment Runner (ToN 2026 Phase 5).

Strictly enforces:
  - Thermal-fair P-core pinning via os.sched_setaffinity (CPUs 2, 4, 6... leaving CPU 0/1 free when N < 8).
  - SMT siblings and all 16 Gracemont E-cores (CPUs 16-31) remain completely idle.
  - Seed-bundled work units: all 6 algorithms evaluated in a single C++ execution, memoizing Yen's path pool.
  - Resumable crash-safe ledger (results/manifest.json) with atomic .tmp -> .json replacement.
  - Atomic .tmp -> .csv step trajectory files and thread-safe master summary CSV locking.
"""

from __future__ import annotations

import argparse
import datetime
import json
import multiprocessing as mp
import os
import queue
import resource
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENGINE_BIN = REPO_ROOT / "build" / "ton_engine"
DATA_DIR = REPO_ROOT / "data"
TOPOLOGIES_DIR = DATA_DIR / "topologies"
STREAMS_DIR = DATA_DIR / "streams"
EXP3_PERMS_DIR = STREAMS_DIR / "exp3_permutations"

ALGORITHMS = (
    "SimRDF",
    "SeqRDF",
    "GreedySimRDF",
    "GreedySeqRDF",
    "NoRerouteSimRDF",
    "NoRerouteSeqRDF",
)


def get_pcore_cpu_ids(num_workers: int) -> list[int]:
    """Calculate logical CPU IDs for workers strictly respecting P-core affinity.

    Host architecture: Intel Core i9-13900HX:
      - 8 Raptor Cove P-Cores (Physical 0..7, Logical 0..15)
      - 16 Gracemont E-Cores (Physical 8..23, Logical 16..31)

    When num_workers < 8:
      Assign even CPUs starting from CPU 2: [2, 4, 6, 8, 10, 12, 14][:num_workers].
      Leaves P-Core 0 (CPU 0 & CPU 1) 100% free for OS scheduler, UI, and user.
    When num_workers == 8:
      Assign even CPUs [0, 2, 4, 6, 8, 10, 12, 14] across all 8 P-Cores.
    SMT siblings (odd CPUs 1, 3, 5...) and all 16 E-cores (16..31) remain 100% idle.
    """
    if num_workers < 1 or num_workers > 8:
        raise ValueError(f"--workers must be between 1 and 8, got {num_workers}")
    if num_workers == 8:
        return [0, 2, 4, 6, 8, 10, 12, 14]
    return [2 * (i + 1) for i in range(num_workers)]


@dataclass(frozen=True)
class WorkUnit:
    unit_id: str
    exp: str
    topology: str
    topology_xml: Path
    cut_model: str
    traffic: str
    seq_id: str
    stream_csv: Path
    step_prefix: str

    def expected_step_files(self, raw_steps_dir: Path) -> list[Path]:
        return [
            raw_steps_dir / f"{self.step_prefix}{alg}.csv" for alg in ALGORITHMS
        ]


def build_work_units(experiment_suite: str) -> list[WorkUnit]:
    """Constructs the deterministic list of seed-bundled WorkUnits."""
    nobel_xml = TOPOLOGIES_DIR / "nobel-eu.xml"
    germany_xml = TOPOLOGIES_DIR / "germany50.xml"

    units: list[WorkUnit] = []

    def make_unit(
        exp: str, topo: str, topo_xml: Path, cut: str, traffic: str, seq_id: str, stream_csv: Path
    ) -> WorkUnit:
        unit_id = f"{exp}_{topo}_{cut}_{traffic}_{seq_id}"
        step_prefix = f"{exp}_{topo}_{traffic}_{seq_id}_"
        return WorkUnit(
            unit_id=unit_id,
            exp=exp,
            topology=topo,
            topology_xml=topo_xml,
            cut_model=cut,
            traffic=traffic,
            seq_id=seq_id,
            stream_csv=stream_csv,
            step_prefix=step_prefix,
        )

    if experiment_suite == "pilot":
        # Milestone 5.3: 4 scale combinations on Seed 1 (24 runs total)
        units.append(
            make_unit(
                "exp1",
                "nobel-eu",
                nobel_xml,
                "span",
                "realistic",
                "s01",
                STREAMS_DIR / "nobel_eu_realistic_seed_01.csv",
            )
        )
        units.append(
            make_unit(
                "exp2",
                "nobel-eu",
                nobel_xml,
                "directional",
                "realistic",
                "s01",
                STREAMS_DIR / "nobel_eu_realistic_seed_01.csv",
            )
        )
        units.append(
            make_unit(
                "exp1",
                "germany50",
                germany_xml,
                "span",
                "1d1m",
                "s01",
                STREAMS_DIR / "germany50_1d1m_seed_01.csv",
            )
        )
        units.append(
            make_unit(
                "exp2",
                "germany50",
                germany_xml,
                "directional",
                "1d1m",
                "s01",
                STREAMS_DIR / "germany50_1d1m_seed_01.csv",
            )
        )
        return units

    if experiment_suite in ("all", "exp1"):
        # EXP-1: Dual Span cuts (672 runs = 112 units x 6)
        # 1. Nobel-EU Realistic (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"nobel_eu_realistic_seed_{s:02d}.csv"
            units.append(make_unit("exp1", "nobel-eu", nobel_xml, "span", "realistic", s_str, csv))
        # 2. Nobel-EU Uniform (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"nobel_eu_uniform_seed_{s:02d}.csv"
            units.append(make_unit("exp1", "nobel-eu", nobel_xml, "span", "uniform", s_str, csv))
        # 3. Germany50 1d1m (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"germany50_1d1m_seed_{s:02d}.csv"
            units.append(make_unit("exp1", "germany50", germany_xml, "span", "1d1m", s_str, csv))
        # 4. Germany50 Uniform (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"germany50_uniform_seed_{s:02d}.csv"
            units.append(make_unit("exp1", "germany50", germany_xml, "span", "uniform", s_str, csv))

    if experiment_suite in ("all", "exp2"):
        # EXP-2: Directional cuts (672 runs = 112 units x 6)
        # 1. Nobel-EU Realistic (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"nobel_eu_realistic_seed_{s:02d}.csv"
            units.append(make_unit("exp2", "nobel-eu", nobel_xml, "directional", "realistic", s_str, csv))
        # 2. Nobel-EU Uniform (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"nobel_eu_uniform_seed_{s:02d}.csv"
            units.append(make_unit("exp2", "nobel-eu", nobel_xml, "directional", "uniform", s_str, csv))
        # 3. Germany50 1d1m (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"germany50_1d1m_seed_{s:02d}.csv"
            units.append(make_unit("exp2", "germany50", germany_xml, "directional", "1d1m", s_str, csv))
        # 4. Germany50 Uniform (28 seeds)
        for s in range(1, 29):
            s_str = f"s{s:02d}"
            csv = STREAMS_DIR / f"germany50_uniform_seed_{s:02d}.csv"
            units.append(make_unit("exp2", "germany50", germany_xml, "directional", "uniform", s_str, csv))

    if experiment_suite in ("all", "exp3"):
        # EXP-3: Permutation sensitivity under Span cuts (360 runs = 60 units x 6)
        # Nobel-EU: 28 uniform shuffles + 2 boundary orders
        for p in range(1, 29):
            p_str = f"p{p:02d}"
            csv = EXP3_PERMS_DIR / f"nobel_eu_perm_{p:02d}.csv"
            units.append(make_unit("exp3", "nobel-eu", nobel_xml, "span", "realistic", p_str, csv))
        units.append(
            make_unit(
                "exp3",
                "nobel-eu",
                nobel_xml,
                "span",
                "realistic",
                "p_short",
                EXP3_PERMS_DIR / "nobel_eu_perm_shortest_first.csv",
            )
        )
        units.append(
            make_unit(
                "exp3",
                "nobel-eu",
                nobel_xml,
                "span",
                "realistic",
                "p_long",
                EXP3_PERMS_DIR / "nobel_eu_perm_longest_first.csv",
            )
        )

        # Germany50: 28 uniform shuffles + 2 boundary orders
        for p in range(1, 29):
            p_str = f"p{p:02d}"
            csv = EXP3_PERMS_DIR / f"germany50_perm_{p:02d}.csv"
            units.append(make_unit("exp3", "germany50", germany_xml, "span", "realistic", p_str, csv))
        units.append(
            make_unit(
                "exp3",
                "germany50",
                germany_xml,
                "span",
                "realistic",
                "p_short",
                EXP3_PERMS_DIR / "germany50_perm_shortest_first.csv",
            )
        )
        units.append(
            make_unit(
                "exp3",
                "germany50",
                germany_xml,
                "span",
                "realistic",
                "p_long",
                EXP3_PERMS_DIR / "germany50_perm_longest_first.csv",
            )
        )

    return units


@dataclass
class UnitExecutionResult:
    unit_id: str
    status: str
    cpu_id: int
    duration_s: float
    peak_rss_mib: float
    output_files: list[str]
    invariants_verified: bool
    returncode: int
    error: str | None = None


def check_unit_files_intact(unit: WorkUnit, raw_steps_dir: Path) -> bool:
    """Verifies that all 6 per-step CSV files exist and have exactly 1,001 rows."""
    expected = unit.expected_step_files(raw_steps_dir)
    for p in expected:
        if not p.is_file():
            return False
        # Verify row count: 1 header + 1000 requests = 1001 lines
        try:
            with open(p, "rb") as f:
                line_count = sum(1 for _ in f)
            if line_count != 1001:
                return False
        except OSError:
            return False
    return True


class ManifestLedger:
    """Thread-safe and process-safe manifest manager for zero-loss checkpointing."""

    def __init__(self, manifest_path: Path):
        self.manifest_path = manifest_path
        self.data: dict[str, Any] = {
            "version": "1.0.0",
            "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "updated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "units": {},
        }
        self.load()

    def load(self) -> None:
        if self.manifest_path.is_file():
            try:
                with open(self.manifest_path, "r", encoding="utf-8") as f:
                    self.data = json.load(f)
            except Exception as e:
                print(f"[WARN] Failed to load existing manifest {self.manifest_path}: {e}")

    def save(self) -> None:
        self.data["updated_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        self.manifest_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = self.manifest_path.with_suffix(".tmp")
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(self.data, f, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, self.manifest_path)

    def is_completed(self, unit_id: str) -> bool:
        rec = self.data["units"].get(unit_id)
        return rec is not None and rec.get("status") == "COMPLETED"

    def record_result(self, res: UnitExecutionResult) -> None:
        self.data["units"][res.unit_id] = asdict(res)
        self.save()


def _worker_process_main(
    worker_idx: int,
    cpu_id: int,
    engine_bin: str,
    raw_steps_dir: str,
    summary_csv: str,
    verify_invariants: bool,
    task_queue: mp.Queue,
    result_queue: mp.Queue,
) -> None:
    """Worker process loop pinned to dedicated P-core."""
    # Enforce strict P-core pinning
    try:
        os.sched_setaffinity(0, {cpu_id})
    except Exception as e:
        print(f"[ERROR Worker {worker_idx}] Failed to pin to CPU {cpu_id}: {e}", file=sys.stderr)

    while True:
        item = task_queue.get()
        if item is None:
            # Poison pill
            break

        unit: WorkUnit = item
        unit_id = unit.unit_id

        # Target step prefix
        step_prefix = str(Path(raw_steps_dir) / unit.step_prefix)

        cmd = [
            engine_bin,
            "--topology",
            str(unit.topology_xml),
            "--stream",
            str(unit.stream_csv),
            "--cut-model",
            unit.cut_model,
            "--algorithm",
            "all",
            "--exp",
            unit.exp,
            "--traffic",
            unit.traffic,
            "--seq-id",
            unit.seq_id,
            "--output-step-prefix",
            step_prefix,
            "--output-summary",
            summary_csv,
        ]
        if verify_invariants:
            cmd.append("--verify-invariants")

        t0 = time.perf_counter()
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        peak_rss_kb = 0
        status_file = f"/proc/{proc.pid}/status"
        while proc.poll() is None:
            try:
                with open(status_file, "r") as sf:
                    for line in sf:
                        if line.startswith("VmHWM:"):
                            parts = line.split()
                            if len(parts) >= 2:
                                peak_rss_kb = max(peak_rss_kb, int(parts[1]))
                            break
            except OSError:
                pass
            time.sleep(0.05)

        stdout, stderr = proc.communicate()
        t1 = time.perf_counter()
        duration_s = t1 - t0

        # Child process rusage fallback if /proc polling missed peak
        child_ru = resource.getrusage(resource.RUSAGE_CHILDREN)
        peak_rss_kb = max(peak_rss_kb, child_ru.ru_maxrss)
        peak_rss_mib = peak_rss_kb / 1024.0

        retcode = proc.returncode
        status = "COMPLETED" if retcode == 0 else "FAILED"
        err_msg = stderr if retcode != 0 else None

        output_files = [str(p) for p in unit.expected_step_files(Path(raw_steps_dir))]

        # Verify output files exist and are complete
        if retcode == 0 and not check_unit_files_intact(unit, Path(raw_steps_dir)):
            status = "FAILED"
            err_msg = "Output trajectory CSV files were incomplete or missing."

        res = UnitExecutionResult(
            unit_id=unit_id,
            status=status,
            cpu_id=cpu_id,
            duration_s=round(duration_s, 3),
            peak_rss_mib=round(peak_rss_mib, 2),
            output_files=output_files,
            invariants_verified=(retcode == 0 and verify_invariants),
            returncode=retcode,
            error=err_msg,
        )
        result_queue.put((res, stdout, stderr))


class ExperimentRunner:
    """Manages parallel multi-core dispatch, progress monitoring, and ledger updates."""

    def __init__(
        self,
        suite: str,
        num_workers: int,
        results_dir: Path,
        summary_path: Path | None = None,
        engine_bin: Path = DEFAULT_ENGINE_BIN,
        verify_invariants: bool = True,
        force: bool = False,
        dry_run: bool = False,
    ):
        self.suite = suite
        self.num_workers = num_workers
        self.results_dir = results_dir
        self.engine_bin = engine_bin
        self.verify_invariants = verify_invariants
        self.force = force
        self.dry_run = dry_run

        self.raw_steps_dir = self.results_dir / "raw_steps"
        self.summaries_dir = self.results_dir / "summaries"
        self.manifest_path = self.results_dir / "manifest.json"

        if summary_path is not None:
            self.summary_csv = summary_path
        elif self.suite == "pilot":
            self.summary_csv = self.summaries_dir / "pilot_summary.csv"
        else:
            self.summary_csv = self.summaries_dir / "all_runs_summary.csv"

        self.cpu_ids = get_pcore_cpu_ids(num_workers)

    def run(self) -> int:
        if not self.engine_bin.is_file():
            print(f"[FATAL] ton_engine binary not found at {self.engine_bin}", file=sys.stderr)
            return 1

        self.raw_steps_dir.mkdir(parents=True, exist_ok=True)
        self.summaries_dir.mkdir(parents=True, exist_ok=True)

        all_units = build_work_units(self.suite)
        total_runs = len(all_units) * len(ALGORITHMS)
        print(
            f"=== ToN 2026 Experiment Runner (Suite: {self.suite.upper()} | "
            f"{len(all_units)} Units | {total_runs} Runs) ==="
        )
        print(f"Workers: {self.num_workers} | Pinned P-Cores: {self.cpu_ids}")
        if self.num_workers < 8:
            print("Core-parking: P-Core 0 (CPU 0/1) and all E-cores (16-31) remain 100% idle.")
        print(f"Summary Target: {self.summary_csv}")
        print(f"Manifest Path:  {self.manifest_path}")

        ledger = ManifestLedger(self.manifest_path)

        units_to_run: list[WorkUnit] = []
        skipped_count = 0
        for unit in all_units:
            if not self.force and ledger.is_completed(unit.unit_id):
                if check_unit_files_intact(unit, self.raw_steps_dir):
                    skipped_count += 1
                    continue
            units_to_run.append(unit)

        print(f"Status: {skipped_count}/{len(all_units)} units already completed. To execute: {len(units_to_run)} units.")

        if self.dry_run:
            print("[DRY-RUN] Work units scheduled:")
            for i, u in enumerate(units_to_run, 1):
                print(f"  {i:3d}. [{u.unit_id}] {u.topology} {u.cut_model} {u.traffic} {u.seq_id}")
            return 0

        if not units_to_run:
            print("[SUCCESS] All units are already completed and verified.")
            return 0

        task_queue: mp.Queue = mp.Queue()
        result_queue: mp.Queue = mp.Queue()

        for unit in units_to_run:
            task_queue.put(unit)
        for _ in range(self.num_workers):
            task_queue.put(None)

        workers: list[mp.Process] = []
        for w_idx, cpu_id in enumerate(self.cpu_ids):
            p = mp.Process(
                target=_worker_process_main,
                args=(
                    w_idx,
                    cpu_id,
                    str(self.engine_bin),
                    str(self.raw_steps_dir),
                    str(self.summary_csv),
                    self.verify_invariants,
                    task_queue,
                    result_queue,
                ),
            )
            p.daemon = True
            p.start()
            workers.append(p)

        completed_units = 0
        failed_units = 0
        total_to_run = len(units_to_run)
        start_time = time.perf_counter()

        def handle_signal(sig, frame):
            print("\n[ABORT] Interrupted by user. Terminating workers...")
            for w in workers:
                if w.is_alive():
                    w.terminate()
            sys.exit(130)

        signal.signal(signal.SIGINT, handle_signal)
        signal.signal(signal.SIGTERM, handle_signal)

        while completed_units + failed_units < total_to_run:
            try:
                res, stdout, stderr = result_queue.get(timeout=1.0)
            except queue.Empty:
                # Check if any worker died unexpectedly
                for w in workers:
                    if not w.is_alive() and w.exitcode not in (0, None):
                        print(f"[FATAL] Worker died with exitcode {w.exitcode}", file=sys.stderr)
                        for other in workers:
                            if other.is_alive():
                                other.terminate()
                        return 1
                continue

            ledger.record_result(res)

            elapsed = time.perf_counter() - start_time
            if res.status == "COMPLETED":
                completed_units += 1
                rate = completed_units / elapsed if elapsed > 0 else 0.0
                remaining = (total_to_run - completed_units) / rate if rate > 0 else 0.0
                eta_str = str(datetime.timedelta(seconds=int(remaining)))
                print(
                    f"[{completed_units:3d}/{total_to_run:3d}] "
                    f"OK ({res.unit_id}) on CPU {res.cpu_id:2d} | "
                    f"{res.duration_s:6.2f}s | RSS: {res.peak_rss_mib:5.1f} MiB | "
                    f"ETA: {eta_str}"
                )
            else:
                failed_units += 1
                print(
                    f"[{completed_units + failed_units:3d}/{total_to_run:3d}] "
                    f"[FAILED] ({res.unit_id}) on CPU {res.cpu_id:2d}: {res.error}",
                    file=sys.stderr,
                )
                if stderr:
                    print(f"Stderr:\n{stderr}", file=sys.stderr)

        for w in workers:
            w.join()

        total_elapsed = time.perf_counter() - start_time
        print(f"\nExecution finished in {total_elapsed:.2f}s.")
        print(f"Completed: {completed_units}/{total_to_run} units ({completed_units * 6} runs).")

        if failed_units > 0:
            print(f"[ERROR] {failed_units} units failed. Check manifest for details.", file=sys.stderr)
            return 1

        return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="ToN 2026 High-Performance Multi-Core Optical Experiment Runner",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--experiments",
        choices=["all", "pilot", "exp1", "exp2", "exp3"],
        default="pilot",
        help="Experiment suite to execute",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=4,
        help="Concurrency level (1 <= N <= 8, pinned exclusively to P-cores)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=REPO_ROOT / "results",
        help="Root directory for experimental outputs",
    )
    parser.add_argument(
        "--summary-path",
        type=Path,
        default=None,
        help="Custom path for master summary CSV output",
    )
    parser.add_argument(
        "--engine-bin",
        type=Path,
        default=DEFAULT_ENGINE_BIN,
        help="Path to compiled ton_engine C++ binary",
    )
    parser.add_argument(
        "--no-verify-invariants",
        action="store_true",
        help="Disable on-the-fly InvariantAuditor checks (not recommended for production)",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force re-execution of work units even if marked COMPLETED in manifest",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print execution plan and core affinity mapping without running",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    runner = ExperimentRunner(
        suite=args.experiments,
        num_workers=args.workers,
        results_dir=args.results_dir,
        summary_path=args.summary_path,
        engine_bin=args.engine_bin,
        verify_invariants=not args.no_verify_invariants,
        force=args.force,
        dry_run=args.dry_run,
    )
    return runner.run()


if __name__ == "__main__":
    sys.exit(main())
