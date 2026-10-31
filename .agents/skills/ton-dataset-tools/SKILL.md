---
name: ton-dataset-tools
description: >-
  Prepares SNDlib topologies (Nobel-EU, Germany50), generates frozen 28-seed arrival streams, validates multiset permutations for EXP-3, and checks SHA-256 manifests. Use for Phase 1 tasks or whenever dataset validation is needed.
---

# ToN Dataset Preparation & Stream Verification Skill

Use this skill to fetch, validate, and freeze SNDlib topologies and reproducible traffic streams.

## Prerequisites
- Always use the dedicated virtual environment Python:
  ```bash
  .venv/bin/python
  ```

## 1. Execute Dataset Ingestion & Stream Generation
Run the canonical dataset preparation script:
```bash
.venv/bin/python scripts/prepare_datasets.py
```

## 2. Expected Dataset Artifacts
The script must generate and populate `data/`:
```text
data/
├── topologies/
│   ├── nobel-eu.xml
│   └── germany50.xml
├── demands/
│   ├── nobel-eu.txt
│   └── germany50_1d1m/ (28 daily demand files)
└── streams/
    ├── manifest.sha256
    ├── nobel_eu_realistic_seed_01.csv ... seed_28.csv
    ├── nobel_eu_uniform_seed_01.csv ... seed_28.csv
    ├── germany50_1d1m_seed_01.csv ... seed_28.csv
    ├── germany50_uniform_seed_01.csv ... seed_28.csv
    └── exp3_permutations/ (60 permutation files)
```

## 3. Verification Protocol
1. **Multiset Equality:** For `EXP-3`, ensure that all 30 permutations for each topology contain the identical multiset of 1,000 requests.
2. **Deterministic Manifest:** Verify SHA-256 integrity:
   ```bash
   cd data/streams && sha256sum -c manifest.sha256
   ```
3. **Reproducibility:** Seed 1..28 must match exact MT19937 PRNG sequences.

