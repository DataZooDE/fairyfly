#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Turns a CLI Result into an MCP ToolResult: renders per spec.output, applies the char/image caps
/// from `policy`, prepends `untrusted_header` (data-not-instructions notice) to screen-derived text,
/// maps error Results to is_error with the structured error code.
///
/// content[0] is always a text block (for images it is the block after the image: see below).
///  - Markdown/Json outputs: `untrusted_header + "\n" + body` (no header line when the header is empty).
///  - Text/action outputs: compact JSON of {status, data, metadata}.
///  - Error results: is_error, `ERROR <CODE>: <message>` [+ `hint: ...`] + compact error JSON.
///  - Image outputs: content = [image block, text block "<title>, WxH px, N bytes"].
/// Text longer than policy.max_result_chars is truncated (Markdown at a line boundary with a trailer,
/// JSON structurally so it stays valid). structuredContent is attached for results <= 8 KB, except
/// for screen reads and images.
ToolResult shape_result(const Result& result, const ToolSpec& spec, const Policy& policy,
                        const std::string& untrusted_header);

/// One-line "SAP screen data (untrusted; ...) - connection <id>, <transaction/title>" header built from
/// result.data (transaction, title) and the connection used (falls back to data.connection_id).
std::string make_untrusted_header(const Result& result, const std::optional<int>& connection);

/// Decoded byte size of the PNG in result.data["screenshot"] (data URI or plain base64); 0 when absent.
std::size_t image_payload_bytes(const Result& result);

} // namespace fairyfly::mcp
