# PE-loader fuzz harness

In-process fuzz target for the hostile-byte PE parsing path
(`PeLoader::load_from_memory`). A crash here is a host-severity memory-safety
bug in PE mapping (headers / sections / relocations / imports / TLS /
load-config). Code execution (`execute_native`) is deliberately **not**
reached.

## Build

```bash
cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPAPAYA_ENABLE_FUZZING=ON
cmake --build build-fuzz --target fuzz_pe_parse
```

`PAPAYA_ENABLE_FUZZING` instruments the whole project with ASan + UBSan
(global, matching `PAPAYA_ENABLE_ASAN`).

## Run

- **Clang (libFuzzer)** — corpus-guided:
  ```bash
  mkdir -p tests/fuzz/corpus
  build-fuzz/tests/fuzz/fuzz_pe_parse tests/fuzz/corpus
  ```
- **GCC (self-driver smoke)** — no library engine, deterministic mutation:
  ```bash
  build-fuzz/tests/fuzz/fuzz_pe_parse                      # ~4096 mutated seeds
  build-fuzz/tests/fuzz/fuzz_pe_parse build/guests/msvcrt_boot.exe  # happy-path replay
  ```
  (Needs an X display or `xvfb-run -a`; the loader constructs a full
  `Win32ApiHle` once per process, which initializes the host display path.)

## Scope & limitations

- Parser-only. Import resolution uses a real `Win32ApiHle`; malformed input is
  normally rejected before imports resolve.
- The GCC driver is a coverage-guided-**inspired** smoke loop, not a real
  generational fuzzer. For serious corpus fuzzing build with Clang.
- `execute_native` is intentionally not exercised (it is covered by the guest
  suite in `tests/guests/`).

