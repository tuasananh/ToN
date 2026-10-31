---
name: ton-build-verify
description: >-
  Builds, compiles, and runs the C++23 test suite and sanitizers for the ToN optical survivability engine. Use whenever building code, verifying a phase gate, or checking for memory leaks and scenario count assertions.
---

# ToN C++23 Build and Verification Skill

Use this skill to configure, build, and run the ToN C++23 test suite with strict compilation flags and sanitizers.

## Prerequisites
- Working directory: Repository root (`/home/tuasananh/repos/ToN`)
- Toolchain: Clang (`/usr/bin/clang++`) or GCC (`/usr/bin/g++`), CMake 4.4.5

## 1. Configure Build Targets

### Release Mode (Performance & Benchmarking)
```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
```

### Debug & Sanitizer Mode (Phase Gate Verification)
```bash
cmake -B build-sanitize -S . -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ -DENABLE_SANITIZERS=ON
```

## 2. Compile

```bash
cmake --build build -j$(nproc)
```
Ensure build completes with **0 errors and 0 warnings**.

## 3. Run Test Gates

```bash
ctest --test-dir build --output-on-failure
```

Or execute the compiled binary directly:
```bash
./build/tests/ton_tests
```

## 4. Phase Verification Checklist
1. All unit tests pass with zero assertion failures.
2. Exact scenario counts for `Nobel-EU` and `Germany50` are verified against `PROBLEM.md` Section 3.2.
3. Sanitizer run reports zero AddressSanitizer or UndefinedBehaviorSanitizer errors.
4. Git commit is executed upon clean pass.
