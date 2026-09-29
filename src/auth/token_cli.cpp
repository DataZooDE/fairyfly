#include "include/auth/token_cli.h"

#include <algorithm>

namespace fairyfly::auth {

namespace {

Result failure(const std::string& code, const std::string& message) {
    Result r;
    r.status = Result::Status::Error;
    r.error = {{"code", code}, {"message", message}};
    return r;
}

json created_json(const CreatedToken& created, const char* note) {
    json j = created.meta.to_public_json();
    j["token"] = created.token;
    j["warning"] = "This token is shown ONCE and cannot be retrieved later. Copy it now and store it in a secret "
                   "manager; if it is lost, rotate it (fairyfly mcp token rotate " + created.meta.name + ").";
    j["note"] = note;
    return j;
}

} // namespace

const std::vector<std::string>& default_token_scopes() {
    static const std::vector<std::string> scopes = {"session", "connection", "screen"};
    return scopes;
}

std::vector<std::string> split_list(const std::vector<std::string>& values) {
    std::vector<std::string> out;
    for (const auto& value : values) {
        std::string current;
        for (char c : value) {
            if (c == ',' || c == ' ' || c == '\t' || c == ';') {
                if (!current.empty()) out.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) out.push_back(current);
    }
    return out;
}

Result run_token_action(const TokenCliArgs& args, TokenStore& store) {
    try {
        Result result;
        result.status = Result::Status::Success;

        if (args.action == "create") {
            NewToken request;
            request.name = args.name;
            request.scopes = split_list(args.scopes);
            const bool scopes_given = !request.scopes.empty();
            if (!scopes_given) request.scopes = default_token_scopes();
            // Safe default: no --scope means the read-only default set. An explicit --scope grants what it names
            // (the server mode remains the ceiling) unless --read-only narrows it.
            request.read_only = args.read_only_flag || !scopes_given;
            request.sap_systems = split_list(args.systems);
            request.tcodes = split_list(args.tcodes);
            request.connections = split_list(args.connections);
            request.allowed_ips = split_list(args.ips);
            request.rate_per_minute = args.rate;
            if (std::find(request.scopes.begin(), request.scopes.end(), "*") != request.scopes.end() && !args.yes)
                return failure("CONFIRMATION_REQUIRED",
                               "a token with scope '*' can use every tool family; repeat with --yes to confirm");
            if (!args.expires.empty()) {
                const auto expiry = parse_expiry(args.expires, store.now());
                if (!expiry) return failure("INVALID_ARGUMENT", "--expires must look like 30d, 12h, 90m or 2026-12-31");
                if (*expiry <= store.now()) return failure("INVALID_ARGUMENT", "--expires lies in the past");
                request.expires = *expiry;
            }
            result.data = created_json(store.create(request), "created");
            return result;
        }

        if (args.action == "rotate") {
            result.data = created_json(store.rotate(args.name), "rotated: the previous token no longer works");
            return result;
        }

        if (args.action == "revoke") {
            if (!store.revoke(args.name)) return failure("TOKEN_NOT_FOUND", "no token named '" + args.name + "'");
            result.data = {{"name", args.name}, {"revoked", true}};
            return result;
        }

        if (args.action == "delete") {
            // Works for revoked and expired tokens too: it removes the record itself, so nothing lingers.
            if (!args.yes)
                return failure("CONFIRMATION_REQUIRED",
                               "deleting a token removes its record for good (revoke keeps it); repeat with --yes to confirm");
            if (!store.remove(args.name)) return failure("TOKEN_NOT_FOUND", "no token named '" + args.name + "'");
            result.data = {{"name", args.name}, {"deleted", true}};
            return result;
        }

        if (args.action == "list") {
            json rows = json::array();
            for (const auto& meta : store.list()) rows.push_back(meta.to_public_json());
            result.data = {{"tokens", rows}, {"count", rows.size()}};
            return result;
        }

        return failure("INVALID_ARGUMENT", "unknown token action '" + args.action + "'");
    } catch (const AuthError& e) {
        return failure(e.code(), e.what());
    } catch (const std::exception& e) {
        return failure("TOKEN_STORE_ERROR", e.what());
    }
}

} // namespace fairyfly::auth
