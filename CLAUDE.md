# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**fairyfly** is an LLM-powered SAP GUI automation CLI tool written in modern C++ (C++17/20). The project bridges SAP's COM-based GUI Scripting API with LLM tool use patterns via the Model Context Protocol (MCP), enabling natural language control of SAP systems.

This is currently in the **pre-implementation phase** - only design documentation exists (`ideas.md`).

## Technology Stack

- **Language**: Modern C++ (C++17 minimum, C++20 preferred)
- **Build System**: CMake with vcpkg for dependency management, external location in envvar VCPKG_ROOT
- **Platform Support**:
  - Primary: Windows (COM-based SAP GUI Scripting API via `sapfewse.ocx`)
  - Future: Java-based platforms (Mac/Linux) via SAP GUI for Java
- **Key Dependencies** (to be added):
  - CLI11 or Boost.Program_options (argument parsing)
  - nlohmann/json (JSON handling)
  - spdlog (structured logging)
  - For MCP try reuse as much of the flapi implemenation as possible, see `docs\datazoode-flapi-8a5edab282632443.txt`
  - openssl/mbedtls (secure credential storage)
  - sqlite3 (session persistence)

## Architecture

The codebase follows a layered architecture (when implemented):

```
┌─────────────────────────────────────┐
│   CLI Interface / MCP Server        │  ← User interaction layer
├─────────────────────────────────────┤
│   Command Processor / Tool Handler  │  ← Business logic layer
├─────────────────────────────────────┤
│   SAP GUI Automation Engine          │  ← SAP interaction layer
│   - Session Management               │
│   - Element Identification           │
│   - Action Execution                 │
│   - Screen Extraction                │
├─────────────────────────────────────┤
│   SAP GUI Scripting API              │  ← SAP COM/Java API
│   (Windows COM / Java JNI)           │
└─────────────────────────────────────┘
```

## Core Concepts from Design Document

### SAP GUI Scripting Fundamentals
- SAP GUI exposes UI via COM objects with strict hierarchy: GuiApplication → GuiConnection → GuiSession → GuiFrameWindow → Controls
- Element IDs follow pattern: `/app/con[0]/ses[0]/wnd[0]/usr/txtFieldName`
- Every action triggers round-trip to SAP server (50-500ms typical) - must handle asynchronicity
- Sessions limited to 6 per connection (SAP hard limit)
- Element identification prioritizes technical IDs over text (language-independent, stable)
- For details on the COM API look at the official SAP guide at `docs/sap_gui_scripting_api.md`

### LLM Integration via MCP
- Use JSON-RPC 2.0 over stdio or HTTP
- Define 5-10 core tools (connect_sap, execute_transaction, fill_field, click_element, read_field, read_screen, read_table, wait_for, disconnect_sap)
- Support both structured JSON and screenshot-based screen representation
- Rich error responses enable LLM self-correction
- For details on MCP refer to `docs\modelcontextprotocol-modelcontextprotocol-8a5edab282632443.txt`

### CLI Design Patterns
- Command structure: `fairyfly <verb> <noun>` or `fairyfly <noun> <verb>`
- All commands support `--output json|markdown|yaml` for LLM consumption
- Hierarchical configuration: CLI flags → env vars → project config → user config → defaults
- Session management for stateful workflows across multiple LLM requests

## Development Commands

When the project is implemented, these commands will be used:

### Building
```bash
make release

# Run tests
make test
```

### Running
```bash
# CLI mode
./build/fairyfly connect <profile>
./build/fairyfly screen read --output json

# MCP server mode
./build/fairyfly serve --transport stdio
./build/fairyfly serve --transport http --port 8080
```

## Claude Code Build Environment Integration

### Working with Visual Studio C++ Tools in Claude Code's Bash Environment

Claude Code on Windows provides a bash-based interface that requires bridging to Windows-native Visual Studio tools. Visual Studio tools like cl.exe, MSBuild, and nmake need over 20 environment variables (PATH, INCLUDE, LIB, LIBPATH) configured before they function correctly.

**RECOMMENDED APPROACH**: Create Windows batch file wrappers that handle environment setup, then invoke them from Claude Code's bash environment using `cmd.exe`. This keeps all build logic in the native Windows ecosystem while maintaining Claude Code compatibility.

### Recommended Build Workflow

**Option 1: CMake + Ninja (Recommended for Claude Code)**

The optimal approach for C++ development in Claude Code combines CMake with the Ninja build system. Ninja significantly outperforms MSBuild on Windows (often 2-3x faster) and works seamlessly with bash environments.

```bash
# Source Visual Studio environment (once per session)
eval "$(vcvarsall.sh x64)"

# Configure with CMake and Ninja generator
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release

# Build with all CPU cores
cmake --build build --parallel

# Run tests
cd build && ctest --output-on-failure
```

**Option 2: Direct cmd.exe command chaining**

For situations where you need to invoke Visual Studio tools directly:

```bash
# Compile with cl.exe
cmd.exe /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && cl.exe /nologo /EHsc /std:c++17 program.cpp"

# Build with MSBuild
cmd.exe /c "call vcvars64.bat && MSBuild.exe \"C:\Projects\MySolution.sln\" /t:Rebuild /p:Configuration=Release"

# Use nmake
cmd.exe /c "call vcvars64.bat && nmake /f Makefile.vc"
```

The `&&` operator ensures the second command inherits the environment modified by vcvars64.bat.

### Setting Up Visual Studio Environment for Claude Code Sessions

**Environment Capture and Export (Recommended)**

The most powerful technique captures the Visual Studio environment and exports it to your bash session, making all VS tools directly accessible:

```bash
# Capture environment to temporary file
cmd.exe /c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && set > /tmp/vcvars.txt"

# Parse and export critical environment variables
while IFS='=' read -r key value; do
    case "$key" in
        PATH|INCLUDE|LIB|LIBPATH)
            export "$key=$value"
            ;;
    esac
done < /tmp/vcvars.txt

# Now VS tools work directly
cl.exe /nologo /EHsc test.cpp
```

**Using vcvars-bash for Automatic Setup**

The community-developed vcvars-bash project provides production-ready scripts for automatic environment setup:

```bash
# Clone vcvars-bash (one-time setup)
git clone https://github.com/nathan818fr/vcvars-bash.git

# Export environment to current shell
eval "$(vcvars-bash/vcvarsall.sh x64)"

# Now all VS tools work directly
cl.exe /nologo test.cpp
cmake -G "NMake Makefiles" ..
nmake
```

vcvars-bash automatically locates your Visual Studio installation using vswhere.exe and supports all target architectures (x86, x64, arm, arm64).

### Architecture-Specific Environment Setup

Visual Studio provides multiple vcvars batch files for different target architectures:

| Batch File | Host | Target | Use Case |
|------------|------|--------|----------|
| **vcvars64.bat** | x64 | x64 | 64-bit native builds (recommended) |
| **vcvars32.bat** | x86 | x86 | 32-bit native builds |
| **vcvarsamd64_x86.bat** | x64 | x86 | Cross-compile to 32-bit on 64-bit machine |
| **vcvarsamd64_arm64.bat** | x64 | ARM64 | Cross-compile to ARM64 |

### Complete Project Build Script Template

Create executable build scripts that Claude Code can invoke autonomously:

```bash
#!/bin/bash
# build_project.sh - Production-ready build script for Claude Code
set -e  # Exit on error

PROJECT_ROOT="$PWD"
BUILD_DIR="build"
ARCH="x64"
CONFIG="Release"

echo "=== Visual Studio C++ Build Script for Claude Code ==="

# 1. Setup environment using vcvars-bash
echo "Setting up MSVC environment for $ARCH..."
eval "$(vcvarsall.sh $ARCH)"

# 2. Verify compiler is available
if ! command -v cl.exe &> /dev/null; then
    echo "ERROR: cl.exe not found in PATH"
    exit 1
fi
echo "Compiler: $(cl.exe 2>&1 | head -n1)"

# 3. Configure with CMake
echo "Configuring project with CMake..."
cmake -G Ninja \
      -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE="$CONFIG" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# 4. Build
echo "Building project..."
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel

# 5. Run tests (if configured)
if [ -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
    echo "Running tests..."
    cd "$BUILD_DIR"
    ctest --output-on-failure
    cd "$PROJECT_ROOT"
fi

echo "=== Build completed successfully ==="
```

### WSL Path Translation

When working across Windows and WSL filesystems, use `wslpath` for automatic path conversion:

```bash
# Convert WSL path to Windows format
PROJECT_WIN=$(wslpath -w "$PWD")
cmd.exe /c "cd \"$PROJECT_WIN\" && build.bat"

# Convert Windows path to WSL format
wslpath "C:\Users\username\project"
# Output: /mnt/c/Users/username/project
```

### Bash Aliases for Common Operations

Add these to your session or `.bashrc`:

```bash
# Setup function for loading MSVC environment
load_msvc() {
    local arch="${1:-x64}"
    eval "$(vcvarsall.sh $arch)"
    echo "MSVC $arch environment loaded"
}

# Convenient aliases
alias msvc64='load_msvc x64'
alias msvc32='load_msvc x86'
alias cmake-ninja='cmake -G Ninja'
alias build='cmake --build build'
alias rebuild='cmake --build build --target clean && cmake --build build'
```

### Environment Verification Checklist

Before building, verify your environment is correctly configured:

```bash
# Check if cl.exe is accessible
which cl.exe || echo "ERROR: cl.exe not in PATH"

# Verify INCLUDE paths
echo "$INCLUDE" | grep -i "microsoft visual studio" || echo "WARNING: INCLUDE not set"

# Verify LIB paths
echo "$LIB" | grep -i "microsoft visual studio" || echo "WARNING: LIB not set"

# Check Visual Studio version
cmd.exe /c "cl.exe" 2>&1 | head -n1
```

### Claude Code Recommended Development Workflow

1. **Start each Claude Code session** by sourcing vcvars-bash environment:
   ```bash
   eval "$(vcvarsall.sh x64)"
   ```

2. **Use CMake + Ninja** as your build system for maximum compatibility and performance

3. **Create project-specific build scripts** that Claude Code can invoke with simple commands

4. **Structure your project** with clear separation:
   ```
   fairyfly/
   ├── src/           # C++ source files
   ├── include/       # Header files
   ├── build/         # CMake output (gitignored)
   ├── scripts/       # build_project.sh, vcvars-bash
   ├── ai/            # Scratch space to create trial and experiment files
   ├── CMakeLists.txt
   └── .gitignore
   ```

5. **Configure Claude Code permissions** to allow build scripts in project settings

This configuration enables Claude Code to autonomously run build commands while maintaining security for potentially dangerous operations.

## Implementation Priorities

Following a CLI-first approach for practical usability:

### Phase 1: Core SAP Automation (✅ In Progress)

**Phase 1A: Core COM Operations (✅ COMPLETED)**
- ✅ Windows COM integration with SAP GUI Scripting API
- ✅ TraceGuard RAII helper with entry/exit logging
- ✅ Core actions: Press buttons, send virtual keys
- ✅ Session management and error handling
- ✅ All tests passing (14/14)

**Phase 1B: Essential Missing Features (✅ COMPLETED)**
- ✅ Wait mechanisms with polling and timeout
- ✅ Element type detection and classification
- ✅ Type-specific actions (click, select checkboxes)
- ✅ Progressive backoff retry logic
- ✅ All tests passing (14/14)

**Phase 1C: Configuration & Persistence (✅ COMPLETED)**
- ✅ ConfigManager with profile loading/saving (JSON format)
- ✅ SessionManager with JSON persistence and TTL
- ✅ Session state serialization/deserialization
- ✅ Profile listing and management
- ✅ Temporary directory-based sessions
- ✅ All tests passing (16/16)

### Phase 2: Command-Line Interface (✅ COMPLETED)

**Phase 2A: Core CLI Framework (✅ COMPLETED)**
- ✅ CLI11 argument parser with verb-noun command structure
- ✅ Commands: `connect`, `disconnect`, `tcode`, `click`, `fill`, `get`, `screen`, `screenshot`
- ✅ Profile management: `profile list`, `profile add`, `profile remove`
- ✅ Session management: `session create`, `session list`, `session resume`
- ✅ Multiple output formats: JSON, Markdown, plain text
- ✅ Error handling with user-friendly messages and suggestions
- ✅ Session persistence and recovery
- ✅ All tests passing (16/16)

**Phase 2B: Interactive Workflows (⏳ Next)**
- Batch operations for filling multiple fields
- Element caching with TTL for performance
- Table extraction and CSV/Excel export
- Advanced error recovery with fallback strategies
- Windows Credential Manager for secure password storage

**Phase 2C: Validation & Testing**
- Comprehensive test coverage for all CLI commands
- Manual testing with SAP workflows
- Performance optimization and profiling

### Phase 3: LLM Integration (MCP Server)
- MCP JSON-RPC 2.0 server with stdio transport
- Tool schema definitions for Claude/LLM consumption
- Screenshot capture for multimodal context
- Integration with Claude Desktop and Cline

### Phase 4: Advanced Features
- Cross-platform Java support (Mac/Linux)
- Parallel session management
- Enterprise deployment patterns
- Advanced performance optimization

## Critical Design Principles

1. **Reliability over features**: SAP automation must be 100% reliable or it's worse than manual work
2. **Security first**: Never log credentials, use OS credential managers, audit all actions
3. **Performance optimization**: Minimize round-trips, cache element lookups, batch operations
4. **Rich error handling**: Provide actionable error messages with suggestions for recovery
5. **Testing strategy**: Unit tests with mocked SAP, integration tests against test systems

## Code Organization

```
fairyfly/
├── src/
│   ├── include/                    # All public headers
│   │   ├── core.h                  # Core types and Result struct
│   │   ├── automation_engine.h     # AutomationEngine interface
│   │   ├── com_wrapper.h           # SAP COM API wrappers
│   │   ├── com_automation_engine.h # COM-based automation implementation
│   │   ├── trace.h                 # Tracing utilities with TraceGuard
│   │   ├── retry.h                 # Retry and wait utilities
│   │   ├── config.h                # Configuration manager for profiles
│   │   ├── session.h               # Session manager for persistence
│   │   └── cli_handler.h           # CLI command handler with integration layer
│   ├── main.cpp                    # Entry point, CLI argument parsing
│   ├── automation_engine.cpp       # StubAutomationEngine implementation
│   ├── com_wrapper.cpp             # Windows COM abstractions
│   ├── com_automation_engine.cpp   # COM automation implementation
│   ├── trace.cpp                   # TraceGuard implementation
│   ├── config.cpp                  # ConfigManager implementation
│   ├── session.cpp                 # SessionManager implementation
│   └── cli_handler.cpp             # CLI command handler implementation
├── tests/
│   ├── unit/
│   │   ├── test_main.cpp           # Basic JSON tests
│   │   ├── test_com_wrapper.cpp    # COM wrapper unit tests
│   │   └── test_config_session.cpp # Config and session manager tests
│   └── CMakeLists.txt
├── CMakeLists.txt                  # Build configuration
└── vcpkg.json                       # Dependency management
```

### Codebase Status

**Completed Components:**
- Phase 1A: Core COM operations with TraceGuard and enhanced logging
- Phase 1B: Wait mechanisms, element type detection, type-specific actions
- Phase 1C: Configuration management and session persistence
- Phase 2A: Complete command-line interface with all core commands
- Core infrastructure: SAP GUI COM wrapper, result types, error handling

**Current Implementation:**
- StubAutomationEngine: Full stub for testing
- ComGuiApplication: SAP GUI COM interface
- ComGuiSession: Session management with wait_for_element
- ComGuiElement: Type detection and type-specific actions (press, select)
- ConfigManager: Profile loading/saving with JSON format
- SessionManager: Session persistence with JSON storage and TTL-based expiration
- CommandHandler: Integration layer with all CLI commands
- Full CLI: 12+ commands with profile and session management
- Output Formatting: JSON, Markdown, plain text support
- Full test suite: 16 unit tests (100% passing)

**CLI Commands:**
- `connect <profile>` - Establish SAP connection
- `disconnect` - Close connection
- `tcode <code>` - Execute SAP transaction
- `click <element>` - Click UI element
- `fill <element> <value>` - Fill field with value
- `get <element>` - Read field value
- `screen read/capture` - Extract UI structure or screenshot
- `session create/list/resume` - Manage sessions
- `profile list/add/remove` - Manage SAP profiles
- `serve` - Start MCP server (Phase 3)

## Windows COM Development Notes

- Use modern C++ RAII patterns for COM object lifecycle management
- Include `comdef.h` for `_com_ptr_t` smart pointers to prevent leaks
- SAP GUI COM objects accessed via `GetObject("SAPGUI")` or `CreateObject("SAPGUI.ScriptingCtrl.1")`
- Element paths case-sensitive, session indices zero-based
- Check `session.busy` property before executing sequential actions
- Handle connection loss gracefully (sessions can be disconnected by server)

## Error Handling Patterns

Distinguish between:
- **Business exceptions**: Expected SAP errors (material not found, etc.) - log and continue
- **Technical exceptions**: Unexpected failures (connection lost, timeout) - retry with exponential backoff

All error responses should follow this JSON structure:
```json
{
  "status": "error",
  "error": {
    "code": "ELEMENT_NOT_FOUND",
    "message": "Human-readable description",
    "element": "wnd[0]/usr/btn[99]",
    "suggestions": ["Run 'fairyfly screen read'", "Check transaction loaded"]
  }
}
```

## Configuration and State

- Session state stored in `~/.config/fairyfly/sessions/` (or Windows equivalent)
- Configuration in `~/.config/fairyfly/config.yml`
- Credentials never stored in config - use OS credential manager APIs
- Element cache TTL default: 5 minutes (configurable)
- Logging format: JSON Lines for machine parsing

## C++ Development Standards and Best Practices

### Code Organization and Architecture
- **Layered Architecture**: Separate concerns into CLI, business logic, SAP interaction, and API layers
- **Header-only libraries where appropriate**: Use for templates and simple utilities (CLI11, nlohmann/json)
- **Pimpl Pattern**: Use for hiding implementation details in COM/platform-specific code
- **Dependency Injection**: Pass dependencies explicitly rather than using globals or singletons

### SOLID Principles
1. **Single Responsibility**: Each class has one reason to change
   - `AutomationEngine`: Interface for all SAP operations
   - `Result`: Encapsulates operation outcomes
   - `ElementId`, `SessionId`: Value types for type safety

2. **Open/Closed**: Classes open for extension, closed for modification
   - Create `AutomationEngine` interface; implement `StubAutomationEngine`, `ComAutomationEngine` (future)
   - Don't modify existing interfaces; add new implementations

3. **Liskov Substitution**: Derived classes can replace base classes
   - `StubAutomationEngine` correctly implements `AutomationEngine` interface
   - Can swap implementations without client code changes

4. **Interface Segregation**: Client-specific interfaces vs. god objects
   - `AutomationEngine` has cohesive methods for one role
   - Future: Separate `IConnectionManager`, `IElementInteraction` if complexity grows

5. **Dependency Inversion**: Depend on abstractions, not concrete implementations
   - `main.cpp` uses `AutomationEngine` abstract interface
   - Implementation obtained via factory: `AutomationEngine::create()`

Ensure that methods are short, and descriptive. The calls within one methods should be on the same level of abstraction and read like a story.
Try to limit the side-effects in one method (if one method does too much different things, split it up into multiple methods).

### Modern C++ Best Practices

#### Memory Management
- **Smart Pointers Only**: `std::unique_ptr` for exclusive ownership, `std::shared_ptr` for shared
- **RAII Pattern**: Resources acquired in constructor, released in destructor
- **No raw `delete`**: All cleanup automatic via destructors
- **Example**: `std::unique_ptr<AutomationEngine> engine = AutomationEngine::create();`

#### Type Safety
- **Strong Types**: Use `struct ElementId { std::string path; }` instead of bare strings
  - Prevents mixing element IDs with other strings
  - Self-documenting code
  - Compile-time type checking

- **std::optional**: For operations that may not return a value
  - `std::optional<std::string> read_field()` vs. error codes

- **Result Type**: Encapsulate success/error/duration in one type
  - Better than separate error codes and status flags
  - Composable for chaining operations

#### Move Semantics
- **Move constructors/assignment**: For types with dynamic data
- **Perfect forwarding**: When passing template parameters
- **Example**: `Result execute_transaction(std::string&& tcode)`

#### const Correctness
- **const members**: Fields that don't change after construction
- **const methods**: Functions that don't modify state
- **const references**: Parameters that won't be modified
- **Example**: `bool is_connected() const { return connected_; }`

#### Error Handling
- **Exceptions for exceptional conditions**: Use `std::exception` subclasses
- **Result types for expected failures**: Not exceptions (element not found is expected)
- **No error codes**: Prefer structured `Result` type over int/enum status
- **Example**:
  ```cpp
  Result click_element(const ElementId& element) {
      if (!element.is_valid()) {
          Result r;
          r.status = Result::Status::Error;
          r.error["code"] = "INVALID_ELEMENT";
          return r;
      }
      // ... perform operation
  }
  ```

#### Resource Management Patterns
- **RAII for COM objects**: Wrap in smart pointers with custom deleters
  ```cpp
  class ComAutomationEngine {
      std::unique_ptr<IDispatch, ComDeleter> app_;
      // Constructor gets COM object, destructor releases
  };
  ```

### Testing Strategy

#### Unit Testing (Catch2)
- **Test in isolation**: Mock external dependencies
- **One assertion per test when possible**: Clear test intent
- **Test names describe behavior**: `TEST_CASE("connect creates session", "[connect]")`
- **Arrange-Act-Assert pattern**:
  ```cpp
  TEST_CASE("Invalid element returns error") {
      // Arrange
      ElementId invalid("");
      // Act
      Result result = engine->click_element(invalid);
      // Assert
      REQUIRE(result.status == Result::Status::Error);
  }
  ```

#### Integration Testing
- **Against test SAP system only**: Never production systems
- **Test critical workflows**: login → transaction → form fill → submit
- **Error scenarios**: Connection lost, timeout, element not found
- **Performance baseline**: Track action times to detect regressions

#### Test Coverage
- **Target 80%+**: Focus on critical paths and error handling
- **Don't test getters/setters**: Test behavior, not trivial accessors
- **Test edge cases**: Empty inputs, boundary conditions, error states

### Code Style and Conventions

#### Naming
- **Classes/Structs**: PascalCase (`AutomationEngine`, `ElementId`)
- **Methods/Functions**: snake_case (`execute_transaction`, `read_field`)
- **Constants**: SCREAMING_SNAKE_CASE (`MAX_SESSIONS = 6`)
- **Member variables**: snake_case with trailing underscore (`connected_`, `active_sessions_`)
- **Abbreviations**: SAP OK, COM OK, GUI OK (domain terms preserved for clarity)

#### Formatting
- **Indentation**: 4 spaces (no tabs)
- **Line length**: Max 100 characters (wrap at logical points)
- **Braces**: Allman style for class/function definitions, same line for control flow
  ```cpp
  class Example
  {
  public:
      void method() { }  // Inline simple methods

      Result complex_operation()
      {
          if (condition) {
              // Braces on same line for control flow
          }
          return result;
      }
  };
  ```

#### Comments
- **Explain WHY, not WHAT**: Code shows what, comments explain why
- **Document public APIs**: /// doxygen comments for headers
- **TODO comments**: For Phase 2/3/4 work
  ```cpp
  /// Reads field value from SAP GUI
  /// \param element Element ID in format "wnd[0]/usr/..."
  /// \return Result with field value or error
  Result read_field(const ElementId& element);

  // TODO: Implement COM integration in Phase 1.2
  ```

#### Logging
- **Use spdlog for all logging**: Not `std::cout` or `printf`
- **Structured logging**: Include relevant context
  - `spdlog::info("action={} element={} session={}", "click", element.path, session.id)`
- **Log levels**:
  - `debug`: Implementation details, function entry/exit
  - `info`: High-level actions (connect, transaction, click)
  - `warn`: Recoverable errors (retry, fallback)
  - `error`: Unrecoverable errors

## Testing Strategy - Autonomous Operation with Deep Diagnostics

### CRITICAL: NO MOCKS FOR SAP Integration Tests

**All integration tests MUST use real SAP GUI connections. Never use mock objects, stubs, or fake SAP responses.**

Integration tests should:
1. Execute the real `fairyfly.exe` CLI binary
2. Connect to actual SAP systems via COM API
3. Execute real transactions (SM59, SU01, SE38, etc.)
4. Capture detailed diagnostic traces for autonomous debugging
5. Generate timestamped artifacts for analysis

### Diagnostic Tracing for Autonomous Debugging

Every SAP operation MUST include a `diagnostics` field in the Result structure with:

```cpp
result.diagnostics["trace"] = json::array({
    {{"step", "initialize"}, {"message", "Starting operation"}},
    {{"step", "invoke_com_method"}, {"method", "OpenConnection"}, {"hresult", "0x00000000"}},
    {{"step", "get_result"}, {"object_id", "con[0]"}, {"status", "success"}},
    {{"step", "complete"}, {"duration_ms", 847}}
});
result.diagnostics["timing_ms"] = duration.count();
```

This enables Claude to autonomously:
- Identify exact step where operations fail
- Map HRESULT codes to COM errors
- Analyze timing to detect timeouts
- Verify all prerequisites met
- Self-correct based on diagnostic data

### Integration Test Structure

PowerShell-based integration tests execute `fairyfly.exe` with `--verbose` flag:

```powershell
# test_integration.ps1 - Real SAP testing, NO MOCKS
$result = & .\fairyfly.exe launch Bigfox --output json --verbose

# Verify response
$json = $result | ConvertFrom-Json
assert($json.status -eq "success")
assert($json.data.connection_id -ne $null)

# Analyze diagnostics for autonomous debugging
if ($json.diagnostics.trace) {
    foreach ($step in $json.diagnostics.trace) {
        # Claude can analyze trace to understand what happened
        Write-Host "$($step.step): $($step.status)"
    }
}
```

See `tests/integration/README.md` for complete testing guide.

### Expected Test Outputs

Each integration test run produces:
- `integration-test-YYYYMMDD-HHmmss.log` - Full PowerShell transcript
- `test_launch_response.json` - Connection response with diagnostics
- `test_tcode_response.json` - Transaction execution response
- `test_screen_sm59.json` - Screen data extraction with diagnostics
- `test_screen_sm59.md` - Human-readable screen format
- `test_diagnostics_summary.json` - Aggregated diagnostics for analysis

Claude can analyze these files to:
- Identify root causes of failures
- Suggest fixes based on diagnostic traces
- Optimize performance (timing analysis)
- Detect regressions (baseline comparison)
- Self-heal based on error patterns

### Performance Monitoring

Track operation timings from Result.duration and diagnostics:

```json
{
  "diagnostics": {
    "timing_ms": 847,
    "trace": [
      {"step": "get_method_id", "duration_ms": 2},
      {"step": "invoke_open_connection", "duration_ms": 800},
      {"step": "get_connection_object", "duration_ms": 45}
    ]
  }
}
```

Use this to:
- Establish performance baselines
- Detect SAP server slowness
- Optimize operation sequences
- Set realistic timeouts

### Unit Tests vs Integration Tests

- **Unit Tests** (C++, Catch2): Test individual components in isolation
  - Mock external dependencies
  - Fast execution (milliseconds)
  - Located in `tests/unit/`

- **Integration Tests** (PowerShell): Test real SAP operations end-to-end
  - NO MOCKS - use real SAP connections
  - Slower execution (seconds)
  - Located in `tests/integration/`
  - Generate diagnostic artifacts for analysis

### Running Tests

```powershell
# Unit tests (may skip if SAP unavailable)
./build/Release/unit_tests.exe

# Integration tests (requires real SAP)
cd tests/integration
.\Invoke-IntegrationTests.ps1

# Analyze diagnostics
cat test_diagnostics_summary.json | ConvertFrom-Json
```

### Iterative Development Workflow

1. **Write test first** (TDD): Define expected behavior
2. **Minimal implementation**: Make test pass with simplest code
3. **Refactor**: Improve design while tests still pass
4. **Commit**: Small, focused commits with descriptive messages

Example commit cycle:
```
1. Write test_click_element() → RED
2. Implement click_element() → GREEN
3. Extract common validation logic → REFACTOR
4. Run all tests, verify coverage
5. Commit: "feat: implement click_element with validation"
```

### Performance Considerations
- **Profile before optimizing**: Use timers in Result to measure
- **Bottleneck: network, not computation**: SAP server round-trips dominate
- **Cache strategically**: Element lookups (with TTL)
- **Batch operations**: Group fills before submit to reduce round-trips

### Dependency Management with vcpkg
- **Define in vcpkg.json**: Declarative dependency list
- **Use CMakeLists.txt for integration**: `find_package()` and `target_link_libraries()`
- **Platform-specific handling**:
  ```cmake
  if(WIN32)
      target_link_libraries(fairyfly PRIVATE ole32 oleaut32)
  endif()
  ```

## Reference Documentation

The comprehensive technical design document in `ideas.md` covers:
- SAP GUI Scripting API details and COM programming patterns
- LLM tool use best practices and prompt engineering
- RPA vendor patterns (UiPath, Blue Prism, etc.)
- MCP protocol specifications
- Performance optimization strategies
- Security considerations for SAP automation

Refer to `ideas.md` for detailed explanations of architectural decisions, implementation patterns, and the rationale behind technology choices.
