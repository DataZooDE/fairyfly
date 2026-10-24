// What the person at the desktop approves (libffl): a write call's summary - every step of a batch, its target and
// the names of the fields it fills, never a value or anything read from the screen. The link agent builds it from the
// call it is about to run and shows it in the approval question (link::ApprovalPrompt); the gateway refuses a call
// that needs an approval but is too long to show, before the call is counted or leaves for the desktop.
#pragma once
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ffy::approval {

inline constexpr std::size_t kMaxSummary = 2000;

// One text per step ("start transaction VA01", "fill fields ctxtA, ctxtB, then press f8"); a call that is not a batch
// is one step. An item of a batch without a tool name is "invalid step".
std::vector<std::string> steps(const std::string& tool, const nlohmann::json& args);
// The steps as one line: "click wnd[0]/.../btn[11]", "run 3 steps: 1. start transaction VA01; 2. ...; 3. press enter".
// nullopt when it is longer than kMaxSummary: a call nobody can see in full is not approved at all.
std::optional<std::string> summarize(const std::string& tool, const nlohmann::json& args);

}  // namespace ffy::approval
