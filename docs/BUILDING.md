# Building from source

## Requirements

- Windows 10/11 x64
- Visual Studio 2022 with the C++ desktop workload (MSVC)
- CMake 3.20 or later
- [vcpkg](https://github.com/microsoft/vcpkg), with the environment variable `VCPKG_ROOT` pointing to the checkout
- For live tests only: SAP GUI for Windows with scripting enabled ([SETUP.md](SETUP.md))

The project uses C++20 and the `x64-windows-static` vcpkg triplet, so the result is a single `fairyfly.exe` without
runtime DLLs. Dependencies are declared in `vcpkg.json` and installed automatically during configuration.

## Build

Configure once, then build only the target you need:

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target fairyfly --parallel
~~~

Run `build\bin\Release\fairyfly.exe`. CMake copies the executable there after linking; this staged copy is the
documented execution path. Visual Studio is a multi-configuration generator and ignores `CMAKE_BUILD_TYPE`: choose
Release or Debug with `--config` (and `-C` for `ctest`). Keep the build directory for fast incremental builds, and use
separate build directories for different generators or toolchains.

## Tests

~~~powershell
cmake --build build --config Release --target unit_tests --parallel
$env:FAIRYFLY_AUDIT = "0"      # keep test runs out of your audit trail
ctest --test-dir build -C Release --output-on-failure
~~~

The Catch2 unit tests in `tests/unit/` use fakes for COM and the operating system and need no SAP system. Tests that
need a live SAP GUI are skipped when none is available.

The scripts in `tests/integration/` run against a live SAP GUI session: the regression suite, the build comparison,
and the MCP stdio and HTTP smoke tests. Each script documents its prerequisites, screen and transaction; see
[tests/integration/README.md](../tests/integration/README.md).

## Makefile shortcuts

The Makefile wraps the CMake commands (`make help` lists all targets):

| Target | Effect |
|---|---|
| `make build` | Configure if needed, build the CLI (Release) |
| `make test` | Build only `unit_tests` and run CTest (does not build the CLI) |
| `make quick` | Same as `test`, reusing the existing build directory |
| `make debug`, `make test-debug` | Debug configuration |
| `make rebuild` | Clean build; only when really needed |

## Build performance

Implementation sources are compiled once into the `fairyfly_core` static library, which the CLI, the unit tests and
the helper tools link. `src/main.cpp` only converts the Windows command line and calls `run_cli` in
`src/cli_entry.cpp`. MSVC builds use `/MP` and precompiled headers for nlohmann JSON, spdlog and fmt. `/MP` together
with `--parallel` can use a lot of memory; lower `--parallel` if a build runs out of RAM. On the development machine
a no-op build takes about 1-3 s and a clean CLI plus unit-test build about 40 s with dependencies already installed.
Dated measurements are in [internal/BUILD_PERFORMANCE.md](internal/BUILD_PERFORMANCE.md).

## Icons

The executable and tray icons in `assets/icons/` are generated from the logo geometry in `assets/logo/`:

~~~powershell
pip install pillow
python assets/generate_icons.py --preview icons-preview.png
~~~

## Versions and releases

fairyfly uses calendar versions (`YYYY.MM.DD`). The version is set in `src/include/version.h`, `CMakeLists.txt` and
`vcpkg.json` and embedded in the executable's version information. Releases are built by
[`release.yml`](../.github/workflows/release.yml) from a `vYYYY.MM.DD` tag; the full procedure, including code signing,
is in [SIGNING.md](SIGNING.md). User-visible changes go into [CHANGELOG.md](../CHANGELOG.md).

## Code layout

See [ARCHITECTURE.md](ARCHITECTURE.md#components). Contributor notes for AI coding agents are in
[CLAUDE.md](../CLAUDE.md).
