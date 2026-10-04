# Build performance

The current CMake build shares implementation sources through the fairyfly_core static library. The CLI, Catch2 unit tests, and VBScript comparison helper link that library, so those targets do not compile the core sources separately. `run_cli` now lives in `src/cli_entry.cpp` inside that library; `src/main.cpp` only converts the Windows command line and calls it. This avoids compiling the heavy CLI implementation once for the executable and again for unit tests. MSVC builds use /MP, and the build commands can run independent targets in parallel.

## Routine commands

Configure once:

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
~~~

Build only what you need:

~~~powershell
cmake --build build --config Release --target fairyfly --parallel
cmake --build build --config Release --target unit_tests --parallel
ctest --test-dir build -C Release --output-on-failure
~~~

The Makefile `build` target builds the CLI. `make test` configures if needed, then builds only `unit_tests` and its dependencies before running CTest; it does not build the CLI. `make quick` reuses the existing build directory. Use `make rebuild` only when a clean build is required.

After a Release build, run `build/bin/Release/fairyfly.exe`. CMake stages it with `copy_if_different` after linking. An older direct `build/Release/fairyfly.exe` artifact failed to attach to SAP GUI on this workstation; refreshing that artifact and relinking restored attachment. The staged path remains the documented execution path.

Visual Studio ignores CMAKE_BUILD_TYPE. Select Release or Debug at build and test time with --config or -C. Keep separate build directories if you use different generators or toolchains.

## What is and is not optimized

- fairyfly_core removes duplicate core and CLI-entry implementation compilation across the CLI and test executables.
- /MP allows MSVC to compile translation units concurrently. CMake --parallel allows independent projects to run concurrently. Together they can use substantial memory; set a lower --parallel value if builds exhaust RAM.
- CMake searches for ccache, but does not detect clcache. Compiler caching with the Visual Studio generator has not been verified here.
- MSVC now precompiles the common nlohmann JSON, spdlog, and fmt headers for `fairyfly_core`. Unity builds remain disabled. A local matched clean-build comparison below favors PCH, but CI cache impact has not been measured.
- Measured on 2026-09-26: three no-op Release CLI builds took 1.13-1.19 s; two fresh configure runs took 9.94-10.94 s; clean CLI plus unit-test builds took 39.06 s with `--parallel 4` and 39.29 s with `--parallel 1`, using already installed dependencies. Later single-source rebuilds took 8.93 s for `main.cpp`, 14.07 s for `com_automation_engine.cpp`, and 7.19 s for the extracted response filter; a subsequent CLI-only no-op took 1.50 s. On 2026-09-27, three no-op builds of both `fairyfly` and `unit_tests` took 3.17, 3.13, and 3.11 s. These target scopes differ, so their no-op times should not be compared as a speedup. Older speedup percentages should not be treated as measurements.
- Before the CLI-entry split, touching the combined `main.cpp` and rebuilding both Release targets took 17.07 s. After the split, touching the same shared CLI implementation in `cli_entry.cpp` and rebuilding both targets took 11.79 s, a 5.28 s reduction in this one incremental comparison. The CMake-regeneration build was excluded from that comparison. All 125 CTest cases passed, and a six-check live Bigfox SM59 run passed with targeted session cleanup.
- On 2026-09-27, removing the unnecessary CLI prerequisite from `make test` reduced three no-op test runs from 6.85, 6.44, and 6.61 s to 5.25, 5.15, and 5.24 s on the same machine. The debug test target uses the corresponding configure-only prerequisite. A fresh `make test` run passed all 139 registered CTest cases without invoking the CLI build target. These are warm-build measurements; clean compilation time is unchanged.
- On 2026-09-27, with both Release targets requested and `--parallel 4`, a no-op build took 3.69 s before the PCH trial and 3.54 s afterward. Touching only `cli_handler.cpp` took 13.98 s before and 10.88 s afterward; touching only `com/element.cpp` took 11.36 s before and 8.64 s afterward. Each is one same-machine sample, including relinks. The first PCH-enabled build recompiled the core and took 33.19 s, including CMake regeneration, so it is not a matched clean-build comparison. All 147 CTest cases passed with five SAP-dependent skips, and a seven-check live SM59 integration run passed and closed its session.
- On 2026-09-27, two freshly configured Visual Studio 2022 build directories used the same x64-windows-static vcpkg toolchain and already-installed dependencies. Both built Release `fairyfly` and `unit_tests` with `--parallel 4`; the only CMake option changed was `CMAKE_DISABLE_PRECOMPILE_HEADERS` (OFF versus ON). In the fresh-build pair, PCH enabled ran first and took 41.15 s; PCH disabled took 47.22 s. The generated `.pch` was 209.9 MiB. The observed peak sum of `cl`, `MSBuild`, `link`, and `mspdbsrv` working sets, sampled every 100 ms, was 4057.1 MiB with PCH and 5992.5 MiB without. A second `--clean-first` pair reversed the order: PCH disabled took 48.67 s, then PCH enabled took 42.19 s. Focused `[screen][tabs]` tests passed in both variants after each build. PCH was 6.07–6.48 s faster across these two pairs, so simple run order does not explain the local difference. The second pair reused configured build directories, the process sum is not whole-machine peak memory, and neither pair predicts CI cache transfer cost. Configure times for the fresh pair were 12.6 s and 12.3 s respectively and are excluded from build times.

To measure a change on the same machine, time one no-op build, one single-source edit build, and one clean build separately. Include configuration and dependency installation only when measuring those phases.

The pending measurements and experiments are tracked in [open work](OPEN_WORK.md).
