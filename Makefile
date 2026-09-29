.PHONY: help build release debug clean clean-all test install uninstall rebuild rebuild-debug check-vcpkg _configure-release _configure-debug

# Configuration variables
BUILD_DIR ?= build
CMAKE_FLAGS ?= -DCMAKE_TOOLCHAIN_FILE=$${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
CMAKE_GENERATOR ?= "Visual Studio 17 2022"
VERBOSE ?= 0
# Parallel build settings - auto-detect CPU count or use specified value
PARALLEL ?= $(NUMBER_OF_PROCESSORS)
ifeq ($(PARALLEL),)
	PARALLEL := 8
endif

# Default target - configure and build Release
.DEFAULT_GOAL := build

# Help target
help:
	@echo "fairyfly - LLM-powered SAP GUI automation CLI"
	@echo ""
	@echo "Available targets:"
	@echo "  make              - Build CLI (Release, parallel, auto-configure)"
	@echo "  make release      - Build CLI (Release, parallel, auto-configure)"
	@echo "  make debug        - Build CLI (Debug, parallel, auto-configure)"
	@echo "  make test         - Run tests"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make clean-all    - Clean everything including CMake cache"
	@echo "  make rebuild      - Clean and build (Release)"
	@echo "  make rebuild-debug- Clean and build (Debug)"
	@echo "  make install      - Install to system (requires admin)"
	@echo "  make uninstall    - Uninstall from system"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Build Optimization:"
	@echo "  Default: Parallel builds using all CPU cores (PARALLEL=$(PARALLEL))"
	@echo "  Override: make PARALLEL=4  # Use 4 cores instead"
	@echo ""
	@echo "Examples:"
	@echo "  make                  # Configure and build Release (all cores)"
	@echo "  make PARALLEL=4       # Build with 4 cores"
	@echo "  make debug            # Configure and build Debug (all cores)"
	@echo "  make rebuild && make test  # Clean, build, and test"

# Detect if VCPKG_ROOT is set
check-vcpkg:
	@if "$(VCPKG_ROOT)"=="" (echo Error: VCPKG_ROOT environment variable not set && exit 1)
	@echo Using VCPKG_ROOT: $(VCPKG_ROOT)

# Internal: Configure CMake for Release (only if not already configured)
_configure-release: check-vcpkg
	@if not exist "$(BUILD_DIR)\CMakeCache.txt" (echo Configuring CMake for Release build... && cmake -B $(BUILD_DIR) -G $(CMAKE_GENERATOR) $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=Release) else (echo CMake already configured for Release)

# Internal: Configure CMake for Debug (only if not already configured)
_configure-debug: check-vcpkg
	@if not exist "$(BUILD_DIR)\CMakeCache.txt" (echo Configuring CMake for Debug build... && cmake -B $(BUILD_DIR) -G $(CMAKE_GENERATOR) $(CMAKE_FLAGS) -DCMAKE_BUILD_TYPE=Debug) else (echo CMake already configured for Debug)

# Build targets - auto-configure and build with parallel compilation
build: _configure-release
	@echo Building project in Release mode (parallel=$(PARALLEL))...
	@cmake --build $(BUILD_DIR) --config Release --target fairyfly --parallel $(PARALLEL)
	@echo Build complete. Executable at: $(BUILD_DIR)/bin/Release/fairyfly.exe

release: _configure-release
	@echo Building project in Release mode (parallel=$(PARALLEL))...
	@cmake --build $(BUILD_DIR) --config Release --target fairyfly --parallel $(PARALLEL)
	@echo Build complete. Executable at: $(BUILD_DIR)/bin/Release/fairyfly.exe

debug: _configure-debug
	@echo Building project in Debug mode (parallel=$(PARALLEL))...
	@cmake --build $(BUILD_DIR) --config Debug --target fairyfly --parallel $(PARALLEL)
	@echo Build complete. Executable at: $(BUILD_DIR)/bin/Debug/fairyfly.exe

# Rebuild targets
rebuild: clean release
	@echo ✓ Rebuild complete

rebuild-debug: clean debug
	@echo ✓ Rebuild complete

# Testing
test: _configure-release
	@echo Running tests...
	@cmake --build $(BUILD_DIR) --config Release --target unit_tests --parallel $(PARALLEL)
	@ctest --test-dir $(BUILD_DIR) -C Release --output-on-failure
	@echo ✓ Tests complete!

test-debug: _configure-debug
	@echo Running tests (Debug)...
	@cmake --build $(BUILD_DIR) --config Debug --target unit_tests --parallel $(PARALLEL)
	@ctest --test-dir $(BUILD_DIR) -C Debug --output-on-failure --verbose
	@echo ✓ Tests complete!

test-verbose: build
	@echo Running tests (verbose)...
	@cmake --build $(BUILD_DIR) --config Release --target unit_tests --parallel $(PARALLEL)
	@ctest --test-dir $(BUILD_DIR) -C Release --verbose --output-on-failure

# Cleaning
clean:
	@echo Cleaning build artifacts...
	@if exist $(BUILD_DIR) (cmake -E remove_directory $(BUILD_DIR)) else (echo Build directory does not exist)
	@echo Clean complete!

clean-all: clean
	@echo Removing vcpkg installed directory...
	@if exist vcpkg_installed (cmake -E remove_directory vcpkg_installed) else (echo vcpkg_installed does not exist)
	@echo Full clean complete!

# Installation
install: build
	@echo Installing fairyfly...
	@cmake --install $(BUILD_DIR) --config Release --prefix "C:\Program Files\fairyfly"
	@echo ✓ Installation complete!

uninstall:
	@echo Uninstalling fairyfly...
	@cmake -E remove_directory "C:\Program Files\fairyfly" 2>nul || echo Note: Could not uninstall - you may need admin privileges
	@echo ✓ Uninstall complete!

# Development helpers
run: build
	@echo Running fairyfly...
	@.\$(BUILD_DIR)\Release\fairyfly.exe --help

run-launch: build
	@echo Testing launch command...
	@.\$(BUILD_DIR)\Release\fairyfly.exe launch Bigfox --verbose

run-screen: build
	@echo Testing screen read command...
	@.\$(BUILD_DIR)\Release\fairyfly.exe screen read --output json

# Integration tests
integration-test: build
	@echo Running integration tests...
	@cd tests\integration && python test_integration.py
	@echo Integration tests complete!

# Diagnostics
info:
	@echo Build Information:
	@echo   Build Directory: $(BUILD_DIR)
	@echo   Generator: $(CMAKE_GENERATOR)
	@echo   VCPKG_ROOT: $(VCPKG_ROOT)
	@echo   Parallel Jobs: $(PARALLEL)
	@echo ""
	@echo Build Optimizations Enabled:
	@echo   - Multiprocessor compilation with /MP flag
	@echo   - Parallel CMake builds with --parallel $(PARALLEL)
	@echo   - Static library architecture with fairyfly_core.lib
	@echo   - Reduced Windows header overhead with WIN32_LEAN_AND_MEAN

# Format code (if clang-format available)
format:
	@echo Formatting code with clang-format...
	@for /r src %%F in (*.cpp *.h) do @clang-format -i "%%F" 2>nul
	@for /r include %%F in (*.h) do @clang-format -i "%%F" 2>nul
	@echo ✓ Format complete!

# Static analysis (if clang-tidy available)
lint:
	@echo Running clang-tidy...
	@for /r src %%F in (*.cpp) do @clang-tidy "%%F" 2>nul
	@echo ✓ Lint complete!

# Quick workflow
quick: test
	@echo ✓ Quick build and test cycle complete!

# All in one
all: clean build test
	@echo ✓ Full build cycle complete!

.SILENT: help info
