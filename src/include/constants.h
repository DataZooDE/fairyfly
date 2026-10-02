#pragma once

#include <chrono>

namespace fairyfly {
namespace constants {

// ============================================================================
// Grid and Table Limits
// ============================================================================

/// Maximum number of rows to extract from a table/grid
constexpr int MAX_TABLE_ROWS = 20;

/// Upper bound for an explicitly requested screen-read row limit
constexpr int MAX_REQUESTED_TABLE_ROWS = 200;

/// Maximum depth to traverse in tree structures
constexpr int MAX_TREE_DEPTH = 10;

/// Maximum depth for element metadata extraction recursion
constexpr int MAX_ELEMENT_DEPTH = 15;

/// Maximum number of children to process during forced enumeration
constexpr int MAX_CHILDREN_TO_PROCESS = 50;

/// Maximum row index for grid probing operations
constexpr int MAX_GRID_ROW = 15;

/// Maximum column index for grid probing operations
constexpr int MAX_GRID_COL = 15;

/// Maximum consecutive misses before stopping grid probing
constexpr int MAX_CONSECUTIVE_MISSES = 10;

/// Minimum number of header-like cells needed to identify a header column
constexpr int MIN_HEADER_LIKE_CELLS = 3;

// ============================================================================
// Timing and Delays
// ============================================================================

/// Default polling interval in milliseconds
constexpr int POLL_INTERVAL_MS = 100;

/// Default retry delay between attempts in milliseconds
constexpr int RETRY_DELAY_MS = 100;

/// Maximum retry delay in milliseconds (for progressive backoff)
constexpr int MAX_RETRY_DELAY_MS = 500;

/// Default wait interval for session operations in milliseconds
constexpr int SESSION_WAIT_INTERVAL_MS = 100;

/// Polling interval while a session reports Busy in milliseconds
constexpr int SESSION_POLL_INTERVAL_MS = 20;

/// Default polling interval for connection wait operations in milliseconds
constexpr int CONNECTION_POLL_INTERVAL_MS = 500;

/// Default timeout for session creation wait in seconds
constexpr int SESSION_CREATION_TIMEOUT_SEC = 10;

/// Small timeout for Windows event wait operations in milliseconds
constexpr int EVENT_CHECK_TIMEOUT_MS = 10;

// ============================================================================
// Image Processing Constants
// ============================================================================

/// PNG image type value for SAP GUI HardCopy operations
constexpr int IMAGE_TYPE_PNG = 2;

/// Lanczos interpolation method for image resizing (CImg constant)
constexpr int INTERPOLATION_LANCZOS = 5;

/// Maximum scale factor for image operations
constexpr float MAX_SCALE_FACTOR = 10.0f;

/// Minimum scale factor for image operations
constexpr float MIN_SCALE_FACTOR = 0.0f;

// ============================================================================
// COM and Windows Constants
// ============================================================================

/// CImg display type for Windows GDI (not X11)
constexpr int CIMG_DISPLAY_GDI = 2;

/// Maximum number of shellcont children to probe
constexpr int MAX_SHELLCONT_CHILDREN = 4;

/// Maximum number of subscreen containers to probe (SE16/selection screens)
constexpr int MAX_SUB_CONTAINERS = 4;

// ============================================================================
// Type Aliases for Better Readability
// ============================================================================

using Milliseconds = std::chrono::milliseconds;
using Seconds = std::chrono::seconds;

} // namespace constants
} // namespace fairyfly

