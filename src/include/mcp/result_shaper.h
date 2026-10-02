#pragma once
#include <cstddef>
#include <optional>
#include <set>
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
///
/// `visible_tools` (null = unknown, rewrite nothing) is the set of tools the caller can call: the CLI usage
/// hints the Markdown formatter prints ("fairyfly element click '<id>'") are rewritten to tool-call form for
/// visible tools and dropped for hidden ones, see adapt_cli_hints().
ToolResult shape_result(const Result& result, const ToolSpec& spec, const Policy& policy,
                        const std::string& untrusted_header, const std::set<std::string>* visible_tools = nullptr);

/// Post-processes formatter output for MCP clients: lines that show a CLI call of `element click|fill|get`
/// become `gui_element_<verb>(element="<id>", ...)` when that tool is in `visible_tools` and are removed (with
/// their `# comment` line and any code block / "Usage Examples" header left empty) when it is not. A `get`
/// hint with --row/--column has no tool form and is dropped. Pure function.
std::string adapt_cli_hints(const std::string& text, const std::set<std::string>& visible_tools);

/// One-line "SAP screen data (untrusted; ...) - connection <id>, <transaction/title>" header built from
/// result.data (transaction, title) and the connection used (falls back to data.connection_id).
std::string make_untrusted_header(const Result& result, const std::optional<int>& connection);

/// Decoded byte size of the PNG in result.data["screenshot"] (data URI or plain base64); 0 when absent.
std::size_t image_payload_bytes(const Result& result);

} // namespace fairyfly::mcp
