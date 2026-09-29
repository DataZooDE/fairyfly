#pragma once
// Logic of `fairyfly mcp token create|list|revoke|rotate` without any CLI11 or process state (unit-testable).

#include <string>
#include <vector>

#include "include/auth/token_store.h"
#include "include/core.h"

namespace fairyfly::auth {

struct TokenCliArgs {
    std::string action;                   ///< create | list | revoke | rotate
    std::string name;
    std::vector<std::string> scopes;      ///< --scope (already split on commas by the CLI layer; split again here to be safe)
    std::vector<std::string> systems;     ///< --system
    std::vector<std::string> tcodes;      ///< --tcode
    std::vector<std::string> ips;         ///< --ip
    int rate = 0;                         ///< --rate
    std::string expires;                  ///< --expires 30d | 2026-12-31
    bool read_only_flag = false;          ///< --read-only
    bool yes = false;                     ///< --yes (confirms a wildcard scope)
};

/// Default scopes of a token created without --scope.
const std::vector<std::string>& default_token_scopes();

/// Splits comma/whitespace separated list values.
std::vector<std::string> split_list(const std::vector<std::string>& values);

/// Runs one token action against `store`. Results never contain a hash; only create/rotate contain the
/// plain token (once, in data.token).
Result run_token_action(const TokenCliArgs& args, TokenStore& store);

} // namespace fairyfly::auth
