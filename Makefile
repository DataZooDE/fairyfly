.PHONY: help build release debug clean clean-all test install uninstall rebuild rebuild-debug check-vcpkg _configure-release _configure-debug

# Configuration variables
BUILD_DIR ?= build
CMAKE_FLAGS ?= -DCMAKE_TOOLCHAIN_FILE=$${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
CMAKE_GENERATOR ?= "Visual Studio 17 2022"
VERBOSE ?= 0

# Default target - configure and build Release
.DEFAULT_GOAL := build

# Help target
help:
	@echo "fairyfly - LLM-powered SAP GUI automation CLI"
	@echo ""
	@echo "Available targets:"
	@echo "  make              - Build project (Release, auto-configure)"
	@echo "  make release      - Build project (Release, auto-configure)"
	@echo "  make debug        - Build project (Debug, auto-configure)"
	@echo "  make test         - Run tests"
	@echo "  make clean        - Clean build artifacts"
	@echo "  make clean-all    - Clean everything including CMake cache"
	@echo "  make rebuild      - Clean and build (Release)"
	@echo "  make rebuild-debug- Clean and build (Debug)"
	@echo "  make install      - Install to system (requires admin)"
	@echo "  make uninstall    - Uninstall from system"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                  # Configure and build Release"
	@echo "  make debug            # Configure and build Debug"
	@echo "  make rebuild && test  # Clean, build, and test"

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

# Build targets - auto-configure and build
build: _configure-release
	@echo Building project in Release mode...
	@cmake --build $(BUILD_DIR) --config Release
	@echo Build complete. Executable at: $(BUILD_DIR)/Release/fairyfly.exe

release: _configure-release
	@echo Building project in Release mode...
	@cmake --build $(BUILD_DIR) --config Release
	@echo Build complete. Executable at: $(BUILD_DIR)/Release/fairyfly.exe

debug: _configure-debug
	@echo Building project in Debug mode...
	@cmake --build $(BUILD_DIR) --config Debug
	@echo Build complete. Executable at: $(BUILD_DIR)/Debug/fairyfly.exe

# Rebuild targets
rebuild: clean release
	@echo ✓ Rebuild complete

rebuild-debug: clean debug
	@echo ✓ Rebuild complete

# Testing
test: build
	@echo Running tests...
	@ctest --test-dir $(BUILD_DIR) -C Release --output-on-failure
	@echo ✓ Tests complete!

test-debug: debug
	@echo Running tests (Debug)...
	@ctest --test-dir $(BUILD_DIR) -C Debug --output-on-failure --verbose
	@echo ✓ Tests complete!

test-verbose: build
	@echo Running tests (verbose)...
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
quick: clean build test
	@echo ✓ Quick build and test cycle complete!

# All in one
all: clean build test
	@echo ✓ Full build cycle complete!

.SILENT: help info
