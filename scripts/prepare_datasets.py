#!/usr/bin/env python3
"""Phase 1: Official SNDlib Dataset Ingestion & Frozen Arrival Stream Generator.

Prepares and validates:
  - data/topologies/{nobel-eu.xml, germany50.xml}
  - data/demands/nobel-eu.txt
  - data/demands/germany50_1d1m/demandMatrix-germany50-DFN-1day-20050201..28.txt
  - data/streams/:
      * 112 base streams (4 profiles x 28 MT19937 seeds, T = 1,000 requests each)
      * 60 EXP-3 permutation streams (2 topologies x [28 shuffles + 2 boundary orders])
      * manifest.sha256 (GNU sha256sum compatible)
"""

from __future__ import annotations

from collections import Counter
from dataclasses import dataclass
import hashlib
import heapq
import math
import os
from pathlib import Path
import random
import shutil
import tarfile
from typing import Dict, Iterable, List, Sequence, Tuple
import urllib.request
import xml.etree.ElementTree as ET

REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR = REPO_ROOT / "data"
TOPOLOGIES_DIR = DATA_DIR / "topologies"
DEMANDS_DIR = DATA_DIR / "demands"
GERMANY50_1D1M_DIR = DEMANDS_DIR / "germany50_1d1m"
STREAMS_DIR = DATA_DIR / "streams"
EXP3_PERMS_DIR = STREAMS_DIR / "exp3_permutations"

EARTH_RADIUS_KM: float = 6371.0
STREAM_LENGTH: int = 1000
NUM_SEEDS: int = 28

SNDLIB_NS = {"s": "http://sndlib.zib.de/network"}

XML_URLS: Dict[str, List[str]] = {
    "nobel-eu.xml": [
        "https://raw.githubusercontent.com/sikorski-as/sndlib/master/xml/nobel-eu/nobel-eu.xml",
        "https://raw.githubusercontent.com/Uriah372-DS/Adjustable-Robust-Optimization-Decomposition-Algorithm/master/sndlib-networks-xml/nobel-eu.xml",
    ],
    "germany50.xml": [
        "https://raw.githubusercontent.com/sikorski-as/sndlib/master/xml/germany50/germany50.xml",
        "https://raw.githubusercontent.com/Uriah372-DS/Adjustable-Robust-Optimization-Decomposition-Algorithm/master/sndlib-networks-xml/germany50.xml",
    ],
}

NOBEL_NATIVE_URLS: List[str] = [
    "https://raw.githubusercontent.com/sikorski-as/sndlib/master/native/nobel-eu/nobel-eu.txt",
]

DEFAULT_LOCAL_NATIVE_ARCHIVES: List[Path] = [
    Path(os.environ.get("SNDLIB_NATIVE_ARCHIVE", "")) if os.environ.get("SNDLIB_NATIVE_ARCHIVE") else Path(),
    REPO_ROOT.parent / "dual_link_failure_2026" / "tuasananh" / "sndlib-networks-native.tgz",
]

DEFAULT_LOCAL_1D1M_ARCHIVES: List[Path] = [
    Path(os.environ.get("SNDLIB_1D1M_ARCHIVE", "")) if os.environ.get("SNDLIB_1D1M_ARCHIVE") else Path(),
    REPO_ROOT.parent
    / "dual_link_failure_2026"
    / "tuasananh"
    / "directed-germany50-DFN-aggregated-1day-over-1month-native.tgz",
]


@dataclass(frozen=True)
class NodeRecord:
    node_id: int
    name: str
    lon_deg: float
    lat_deg: float


@dataclass(frozen=True)
class SpanRecord:
    span_id: int
    name: str
    u: int
    v: int
    length_km: float


@dataclass(frozen=True)
class TopologyData:
    name: str
    nodes: Tuple[NodeRecord, ...]
    spans: Tuple[SpanRecord, ...]
    name_to_id: Dict[str, int]
    xml_demands: Dict[Tuple[int, int], float]


def haversine_km(lon1_deg: float, lat1_deg: float, lon2_deg: float, lat2_deg: float) -> float:
    """Computes symmetric great-circle Haversine distance in kilometers (R = 6371.0 km)."""
    if (lon1_deg, lat1_deg) > (lon2_deg, lat2_deg):
        lon1_deg, lat1_deg, lon2_deg, lat2_deg = lon2_deg, lat2_deg, lon1_deg, lat1_deg
    deg2rad = math.pi / 180.0
    phi1 = lat1_deg * deg2rad
    phi2 = lat2_deg * deg2rad
    dphi = (lat2_deg - lat1_deg) * deg2rad
    dlam = (lon2_deg - lon1_deg) * deg2rad
    sin_dphi = math.sin(0.5 * dphi)
    sin_dlam = math.sin(0.5 * dlam)
    a = sin_dphi * sin_dphi + math.cos(phi1) * math.cos(phi2) * (sin_dlam * sin_dlam)
    return 2.0 * EARTH_RADIUS_KM * math.atan2(math.sqrt(a), math.sqrt(max(0.0, 1.0 - a)))


def _download_first_available(urls: Sequence[str], dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    last_err: Exception | None = None
    for url in urls:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ToN2026-DatasetPrep/1.0"})
            with urllib.request.urlopen(req, timeout=20) as resp:
                payload = resp.read()
            tmp = dest.with_suffix(dest.suffix + ".tmp")
            tmp.write_bytes(payload)
            tmp.replace(dest)
            return
        except Exception as err:  # noqa: BLE001
            last_err = err
    raise RuntimeError(f"Failed to download {dest.name} from configured mirrors: {last_err}")


def ensure_raw_datasets() -> None:
    """Ensures official SNDlib XML topologies and native demand archives exist under data/."""
    TOPOLOGIES_DIR.mkdir(parents=True, exist_ok=True)
    DEMANDS_DIR.mkdir(parents=True, exist_ok=True)
    GERMANY50_1D1M_DIR.mkdir(parents=True, exist_ok=True)

    for xml_name, urls in XML_URLS.items():
        target = TOPOLOGIES_DIR / xml_name
        if not target.is_file() or target.stat().st_size == 0:
            _download_first_available(urls, target)

    nobel_txt = DEMANDS_DIR / "nobel-eu.txt"
    if not nobel_txt.is_file() or nobel_txt.stat().st_size == 0:
        extracted = False
        for archive in DEFAULT_LOCAL_NATIVE_ARCHIVES:
            if archive and archive.is_file():
                with tarfile.open(archive, "r:gz") as tf:
                    matches = [m for m in tf.getmembers() if m.name.endswith("nobel-eu.txt")]
                    if matches:
                        fobj = tf.extractfile(matches[0])
                        if fobj is not None:
                            nobel_txt.write_bytes(fobj.read())
                            extracted = True
                            break
        if not extracted:
            _download_first_available(NOBEL_NATIVE_URLS, nobel_txt)

    existing_1d1m = sorted(GERMANY50_1D1M_DIR.glob("demandMatrix-germany50-DFN-1day-*.txt"))
    if len(existing_1d1m) != NUM_SEEDS:
        extracted = False
        for archive in DEFAULT_LOCAL_1D1M_ARCHIVES:
            if archive and archive.is_file():
                with tarfile.open(archive, "r:gz") as tf:
                    members = sorted(
                        [m for m in tf.getmembers() if m.isfile() and m.name.endswith(".txt")],
                        key=lambda m: m.name,
                    )
                    if len(members) != NUM_SEEDS:
                        raise ValueError(
                            f"Expected {NUM_SEEDS} daily files in {archive}, found {len(members)}"
                        )
                    for member in members:
                        fobj = tf.extractfile(member)
                        if fobj is None:
                            raise RuntimeError(f"Could not extract {member.name} from {archive}")
                        out_path = GERMANY50_1D1M_DIR / Path(member.name).name
                        out_path.write_bytes(fobj.read())
                    extracted = True
                    break
        if not extracted:
            raise FileNotFoundError(
                f"Germany50 1d1m daily demand files not found in {GERMANY50_1D1M_DIR} "
                "and no local SNDlib 1d1m archive found."
            )


def parse_sndlib_xml(xml_path: Path, expected_nodes: int, expected_spans: int) -> TopologyData:
    """Parses and validates an official SNDlib XML network file."""
    root = ET.parse(xml_path).getroot()
    nodes_list: List[NodeRecord] = []
    name_to_id: Dict[str, int] = {}

    for node_elem in root.findall(".//s:networkStructure/s:nodes/s:node", SNDLIB_NS):
        name = node_elem.attrib["id"].strip()
        if not name or name in name_to_id:
            raise ValueError(f"Invalid or duplicate node id '{name}' in {xml_path}")
        x_elem = node_elem.find("s:coordinates/s:x", SNDLIB_NS)
        y_elem = node_elem.find("s:coordinates/s:y", SNDLIB_NS)
        if x_elem is None or y_elem is None or x_elem.text is None or y_elem.text is None:
            raise ValueError(f"Missing geographic coordinates for node '{name}' in {xml_path}")
        lon_deg = float(x_elem.text)
        lat_deg = float(y_elem.text)
        if not (-180.0 <= lon_deg <= 180.0 and -90.0 <= lat_deg <= 90.0):
            raise ValueError(
                f"Out-of-range coordinates ({lon_deg}, {lat_deg}) for node '{name}' in {xml_path}"
            )
        nid = len(nodes_list)
        name_to_id[name] = nid
        nodes_list.append(NodeRecord(node_id=nid, name=name, lon_deg=lon_deg, lat_deg=lat_deg))

    if len(nodes_list) != expected_nodes:
        raise ValueError(
            f"{xml_path.name}: expected {expected_nodes} nodes, parsed {len(nodes_list)}"
        )

    # Confirm canonical alphabetical node ordering in official SNDlib files.
    node_names = [n.name for n in nodes_list]
    if node_names != sorted(node_names):
        raise ValueError(f"{xml_path.name}: node order is not canonically sorted")

    spans_list: List[SpanRecord] = []
    seen_spans: set[Tuple[int, int]] = set()
    for link_elem in root.findall(".//s:networkStructure/s:links/s:link", SNDLIB_NS):
        lid = link_elem.attrib["id"].strip()
        src_elem = link_elem.find("s:source", SNDLIB_NS)
        dst_elem = link_elem.find("s:target", SNDLIB_NS)
        if src_elem is None or dst_elem is None or src_elem.text is None or dst_elem.text is None:
            raise ValueError(f"Malformed link '{lid}' in {xml_path}")
        u_name = src_elem.text.strip()
        v_name = dst_elem.text.strip()
        if u_name not in name_to_id or v_name not in name_to_id:
            raise ValueError(f"Unknown endpoint in link '{lid}': ({u_name}, {v_name})")
        u = name_to_id[u_name]
        v = name_to_id[v_name]
        if u == v:
            raise ValueError(f"Self-loop link '{lid}' on node '{u_name}' in {xml_path}")
        canon = (min(u, v), max(u, v))
        if canon in seen_spans:
            raise ValueError(f"Duplicate physical span {canon} ('{lid}') in {xml_path}")
        seen_spans.add(canon)
        dist_km = haversine_km(
            nodes_list[u].lon_deg,
            nodes_list[u].lat_deg,
            nodes_list[v].lon_deg,
            nodes_list[v].lat_deg,
        )
        if not (dist_km > 0.0 and math.isfinite(dist_km)):
            raise ValueError(f"Non-positive Haversine distance {dist_km} on link '{lid}'")
        spans_list.append(
            SpanRecord(span_id=len(spans_list), name=lid, u=u, v=v, length_km=dist_km)
        )

    if len(spans_list) != expected_spans:
        raise ValueError(
            f"{xml_path.name}: expected {expected_spans} physical spans, parsed {len(spans_list)}"
        )

    xml_demands: Dict[Tuple[int, int], float] = {}
    for dem_elem in root.findall(".//s:demands/s:demand", SNDLIB_NS):
        s_elem = dem_elem.find("s:source", SNDLIB_NS)
        t_elem = dem_elem.find("s:target", SNDLIB_NS)
        v_elem = dem_elem.find("s:demandValue", SNDLIB_NS)
        if (
            s_elem is None
            or t_elem is None
            or v_elem is None
            or s_elem.text is None
            or t_elem.text is None
            or v_elem.text is None
        ):
            raise ValueError(f"Malformed demand element in {xml_path}")
        u = name_to_id[s_elem.text.strip()]
        v = name_to_id[t_elem.text.strip()]
        val = float(v_elem.text.strip())
        if u == v or val < 0.0:
            raise ValueError(f"Invalid demand ({u}, {v}) = {val} in {xml_path}")
        xml_demands[(u, v)] = val

    return TopologyData(
        name=xml_path.stem,
        nodes=tuple(nodes_list),
        spans=tuple(spans_list),
        name_to_id=name_to_id,
        xml_demands=xml_demands,
    )


def parse_sndlib_native_demands(
    txt_path: Path, name_to_id: Dict[str, int]
) -> Tuple[List[str], Dict[Tuple[int, int], float]]:
    """Parses NODES and DEMANDS sections from an SNDlib native format text file."""
    content = txt_path.read_text(encoding="latin-1")
    section: str | None = None
    parsed_nodes: List[str] = []
    demands: Dict[Tuple[int, int], float] = {}

    for raw_line in content.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("?"):
            continue
        if line.startswith("NODES ("):
            section = "NODES"
            continue
        if line.startswith("DEMANDS ("):
            section = "DEMANDS"
            continue
        if line.endswith("(") and any(
            line.startswith(k) for k in ("META", "LINKS", "ADMISSIBLE_PATHS")
        ):
            section = "OTHER"
            continue
        if line == ")":
            section = None
            continue

        if section == "NODES":
            node_name = line.split()[0]
            parsed_nodes.append(node_name)
        elif section == "DEMANDS":
            # Format: <demand_id> ( <source> <target> ) <routing_unit> <demand_value> <max_path_len>
            open_idx = line.find("(")
            close_idx = line.find(")", open_idx + 1)
            if open_idx < 0 or close_idx < 0:
                raise ValueError(f"Malformed DEMANDS entry in {txt_path}: {line}")
            endpoints = line[open_idx + 1 : close_idx].split()
            after_tokens = line[close_idx + 1 :].split()
            if len(endpoints) != 2 or len(after_tokens) < 2:
                raise ValueError(f"Malformed DEMANDS tokens in {txt_path}: {line}")
            src_name, dst_name = endpoints[0], endpoints[1]
            if src_name not in name_to_id or dst_name not in name_to_id:
                raise ValueError(f"Unknown node in {txt_path} demand: ({src_name}, {dst_name})")
            u = name_to_id[src_name]
            v = name_to_id[dst_name]
            val = float(after_tokens[1])
            if u == v or val < 0.0 or not math.isfinite(val):
                raise ValueError(f"Invalid demand value in {txt_path}: ({src_name}, {dst_name})={val}")
            demands[(u, v)] = val

    if parsed_nodes and parsed_nodes != [
        k for k, _ in sorted(name_to_id.items(), key=lambda kv: kv[1])
    ]:
        raise ValueError(f"Node list mismatch between {txt_path.name} and topology XML")

    return parsed_nodes, demands


def compute_all_pairs_shortest_km(topo: TopologyData) -> Dict[Tuple[int, int], float]:
    """Computes exact symmetric all-pairs shortest-path Haversine distance d_G(s, d) in km."""
    num_nodes = len(topo.nodes)
    adj: List[List[Tuple[int, float]]] = [[] for _ in range(num_nodes)]
    for span in topo.spans:
        adj[span.u].append((span.v, span.length_km))
        adj[span.v].append((span.u, span.length_km))
    for u in range(num_nodes):
        adj[u].sort(key=lambda pair: pair[0])

    raw_dist: List[List[float]] = [[math.inf] * num_nodes for _ in range(num_nodes)]
    for src in range(num_nodes):
        raw_dist[src][src] = 0.0
        pq: List[Tuple[float, int]] = [(0.0, src)]
        while pq:
            d_u, u = heapq.heappop(pq)
            if d_u > raw_dist[src][u]:
                continue
            for v, w in adj[u]:
                cand = d_u + w
                if cand < raw_dist[src][v]:
                    raw_dist[src][v] = cand
                    heapq.heappush(pq, (cand, v))

    sym_dist: Dict[Tuple[int, int], float] = {}
    for u in range(num_nodes):
        for v in range(num_nodes):
            if u == v:
                continue
            if not math.isfinite(raw_dist[u][v]):
                raise ValueError(f"Topology {topo.name} is not strongly connected: ({u}, {v})")
            if abs(raw_dist[u][v] - raw_dist[v][u]) > 1e-9:
                raise ValueError(
                    f"Asymmetric shortest path in {topo.name}: d({u},{v})={raw_dist[u][v]} vs "
                    f"d({v},{u})={raw_dist[v][u]}"
                )
            lo, hi = (u, v) if u < v else (v, u)
            sym_dist[(u, v)] = raw_dist[lo][hi]
    return sym_dist


def build_nobel_symmetric_demands(
    raw_demands: Dict[Tuple[int, int], float], num_nodes: int
) -> Dict[Tuple[int, int], float]:
    """Transforms Nobel-EU's 378 undirected city-pair demands into 756 symmetric directed demands."""
    expected_undirected = num_nodes * (num_nodes - 1) // 2
    if len(raw_demands) != expected_undirected:
        raise ValueError(
            f"Expected {expected_undirected} undirected city-pair demands in Nobel-EU, "
            f"got {len(raw_demands)}"
        )
    directed: Dict[Tuple[int, int], float] = {}
    for (u, v), val in raw_demands.items():
        if (v, u) in raw_demands:
            raise ValueError(f"Unexpected reverse pair ({v}, {u}) in undirected Nobel-EU demands")
        directed[(u, v)] = val
        directed[(v, u)] = val
    return directed


def sample_uniform_stream(num_nodes: int, length: int, seed: int) -> List[Tuple[int, int]]:
    """Samples T=(src, dst) requests uniformly over ordered node pairs using MT19937(seed)."""
    pairs: List[Tuple[int, int]] = [
        (u, v) for u in range(num_nodes) for v in range(num_nodes) if u != v
    ]
    rng = random.Random(seed)
    return [rng.choice(pairs) for _ in range(length)]


def sample_weighted_stream(
    directed_demands: Dict[Tuple[int, int], float], length: int, seed: int
) -> List[Tuple[int, int]]:
    """Samples T=(src, dst) requests proportional to D(s, d) using MT19937(seed)."""
    sorted_items = sorted(
        ((pair, val) for pair, val in directed_demands.items() if val > 0.0),
        key=lambda kv: kv[0],
    )
    if not sorted_items:
        raise ValueError("Demand matrix has zero positive entries")
    pairs = [kv[0] for kv in sorted_items]
    weights = [kv[1] for kv in sorted_items]
    rng = random.Random(seed)
    return rng.choices(pairs, weights=weights, k=length)


def generate_exp3_permutations(
    base_stream: Sequence[Tuple[int, int]],
    shortest_km: Dict[Tuple[int, int], float],
) -> Dict[str, List[Tuple[int, int]]]:
    """Generates 28 seeded Fisher-Yates shuffles + 2 deterministic boundary permutations."""
    base_counter = Counter(base_stream)
    perms: Dict[str, List[Tuple[int, int]]] = {}

    for p_idx in range(1, NUM_SEEDS + 1):
        rng = random.Random(p_idx)
        shuffled = list(base_stream)
        rng.shuffle(shuffled)
        perms[f"perm_{p_idx:02d}"] = shuffled

    shortest_ordered = [
        req
        for _, req in sorted(
            enumerate(base_stream),
            key=lambda item: (shortest_km[item[1]], item[0]),
        )
    ]
    longest_ordered = [
        req
        for _, req in sorted(
            enumerate(base_stream),
            key=lambda item: (-shortest_km[item[1]], item[0]),
        )
    ]
    perms["perm_shortest_first"] = shortest_ordered
    perms["perm_longest_first"] = longest_ordered

    if len(perms) != NUM_SEEDS + 2:
        raise AssertionError(f"Expected 30 permutations, generated {len(perms)}")

    for label, seq in perms.items():
        if len(seq) != len(base_stream) or Counter(seq) != base_counter:
            raise AssertionError(f"Multiset mismatch detected in EXP-3 permutation '{label}'")

    return perms


def write_stream_csv(
    csv_path: Path, requests: Sequence[Tuple[int, int]], nodes: Sequence[NodeRecord]
) -> None:
    """Atomically writes a 1,000-request arrival stream CSV with Unix LF endings."""
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    lines = ["t,src,dst,src_name,dst_name\n"]
    for t, (src, dst) in enumerate(requests):
        if src == dst or not (0 <= src < len(nodes) and 0 <= dst < len(nodes)):
            raise ValueError(f"Invalid request ({src}, {dst}) at t={t} for {csv_path}")
        lines.append(f"{t},{src},{dst},{nodes[src].name},{nodes[dst].name}\n")
    tmp = csv_path.with_suffix(".csv.tmp")
    tmp.write_text("".join(lines), encoding="utf-8", newline="\n")
    tmp.replace(csv_path)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    return digest.hexdigest()


def write_and_verify_manifest(streams_dir: Path) -> List[Tuple[str, str]]:
    """Writes GNU sha256sum-compatible manifest.sha256 and verifies all stream digests."""
    csv_files = sorted(
        p.relative_to(streams_dir).as_posix() for p in streams_dir.rglob("*.csv") if p.is_file()
    )
    expected_total = 4 * NUM_SEEDS + 2 * (NUM_SEEDS + 2)
    if len(csv_files) != expected_total:
        raise RuntimeError(f"Expected {expected_total} stream CSVs, found {len(csv_files)}")

    entries: List[Tuple[str, str]] = []
    manifest_lines: List[str] = []
    for rel_posix in csv_files:
        digest = sha256_file(streams_dir / rel_posix)
        entries.append((digest, rel_posix))
        manifest_lines.append(f"{digest}  {rel_posix}\n")

    manifest_path = streams_dir / "manifest.sha256"
    tmp = manifest_path.with_suffix(".sha256.tmp")
    tmp.write_text("".join(manifest_lines), encoding="utf-8", newline="\n")
    tmp.replace(manifest_path)

    # Re-read and verify every entry in manifest.sha256.
    for line in manifest_path.read_text(encoding="utf-8").splitlines():
        expected_hash, rel_posix = line.split("  ", maxsplit=1)
        actual_hash = sha256_file(streams_dir / rel_posix)
        if actual_hash != expected_hash:
            raise RuntimeError(f"SHA-256 verification failed for {rel_posix}")

    return entries


def prepare_all() -> None:
    """Executes full Phase 1 dataset ingestion, validation, stream freezing, and manifest sign-off."""
    ensure_raw_datasets()

    nobel = parse_sndlib_xml(TOPOLOGIES_DIR / "nobel-eu.xml", expected_nodes=28, expected_spans=41)
    germany = parse_sndlib_xml(
        TOPOLOGIES_DIR / "germany50.xml", expected_nodes=50, expected_spans=88
    )

    _, nobel_txt_demands = parse_sndlib_native_demands(
        DEMANDS_DIR / "nobel-eu.txt", nobel.name_to_id
    )
    if nobel_txt_demands != nobel.xml_demands:
        raise ValueError("Nobel-EU native demands do not match Nobel-EU XML demands")
    nobel_directed_demands = build_nobel_symmetric_demands(nobel_txt_demands, len(nobel.nodes))

    daily_files = sorted(GERMANY50_1D1M_DIR.glob("demandMatrix-germany50-DFN-1day-*.txt"))
    if len(daily_files) != NUM_SEEDS:
        raise ValueError(f"Expected {NUM_SEEDS} Germany50 1d1m files, found {len(daily_files)}")

    germany_daily_demands: List[Dict[Tuple[int, int], float]] = []
    for day_idx, day_file in enumerate(daily_files, start=1):
        expected_suffix = f"200502{day_idx:02d}.txt"
        if not day_file.name.endswith(expected_suffix):
            raise ValueError(f"Unexpected 1d1m filename order at day {day_idx}: {day_file.name}")
        _, day_demands = parse_sndlib_native_demands(day_file, germany.name_to_id)
        if sum(day_demands.values()) <= 0.0:
            raise ValueError(f"Non-positive total demand volume in {day_file.name}")
        germany_daily_demands.append(day_demands)

    # Clean stream directories before deterministic generation so stale files never linger.
    if STREAMS_DIR.exists():
        shutil.rmtree(STREAMS_DIR)
    STREAMS_DIR.mkdir(parents=True, exist_ok=True)
    EXP3_PERMS_DIR.mkdir(parents=True, exist_ok=True)

    nobel_seed1_realistic: List[Tuple[int, int]] = []
    germany_seed1_realistic: List[Tuple[int, int]] = []

    for seed in range(1, NUM_SEEDS + 1):
        nobel_real = sample_weighted_stream(nobel_directed_demands, STREAM_LENGTH, seed=seed)
        nobel_unif = sample_uniform_stream(len(nobel.nodes), STREAM_LENGTH, seed=seed)
        germany_real = sample_weighted_stream(
            germany_daily_demands[seed - 1], STREAM_LENGTH, seed=seed
        )
        germany_unif = sample_uniform_stream(len(germany.nodes), STREAM_LENGTH, seed=seed)

        if seed == 1:
            nobel_seed1_realistic = nobel_real
            germany_seed1_realistic = germany_real

        write_stream_csv(
            STREAMS_DIR / f"nobel_eu_realistic_seed_{seed:02d}.csv", nobel_real, nobel.nodes
        )
        write_stream_csv(
            STREAMS_DIR / f"nobel_eu_uniform_seed_{seed:02d}.csv", nobel_unif, nobel.nodes
        )
        write_stream_csv(
            STREAMS_DIR / f"germany50_1d1m_seed_{seed:02d}.csv", germany_real, germany.nodes
        )
        write_stream_csv(
            STREAMS_DIR / f"germany50_uniform_seed_{seed:02d}.csv", germany_unif, germany.nodes
        )

    nobel_shortest_km = compute_all_pairs_shortest_km(nobel)
    germany_shortest_km = compute_all_pairs_shortest_km(germany)

    nobel_perms = generate_exp3_permutations(nobel_seed1_realistic, nobel_shortest_km)
    for label, seq in nobel_perms.items():
        write_stream_csv(EXP3_PERMS_DIR / f"nobel_eu_{label}.csv", seq, nobel.nodes)

    germany_perms = generate_exp3_permutations(germany_seed1_realistic, germany_shortest_km)
    for label, seq in germany_perms.items():
        write_stream_csv(EXP3_PERMS_DIR / f"germany50_{label}.csv", seq, germany.nodes)

    entries = write_and_verify_manifest(STREAMS_DIR)
    print(
        f"[Phase 1 OK] Topologies validated: Nobel-EU ({len(nobel.nodes)}N, {len(nobel.spans)}E, "
        f"{2 * len(nobel.spans)}L), Germany50 ({len(germany.nodes)}N, {len(germany.spans)}E, "
        f"{2 * len(germany.spans)}L)."
    )
    print(
        f"[Phase 1 OK] Frozen {4 * NUM_SEEDS} base streams + {2 * (NUM_SEEDS + 2)} EXP-3 "
        f"permutations ({len(entries)} CSVs total) and verified manifest.sha256."
    )


if __name__ == "__main__":
    prepare_all()
