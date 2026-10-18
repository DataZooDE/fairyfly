// `ffl/1` link protocol: JSON text frames on one WebSocket (subprotocol "ffl.v1").
//
// Keepalive: the DEVICE sends {"t":"ping"} every 20 s and the gateway answers {"t":"pong"}; either side closes after
// 60 s of silence. Application-level pings are used on purpose: WebSocket control frames are written from the
// reading thread by blocking Beast streams and would race with concurrent frame writes.
#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace ffy::proto {

inline constexpr const char* kVersion = "ffl/1";
inline constexpr const char* kSubprotocol = "ffl.v1";

inline constexpr const char* THello = "hello";
inline constexpr const char* TChallenge = "challenge";
inline constexpr const char* TProof = "proof";
inline constexpr const char* TReady = "ready";
inline constexpr const char* TReq = "req";
inline constexpr const char* TRes = "res";
inline constexpr const char* TEvt = "evt";
inline constexpr const char* TCancel = "cancel";
inline constexpr const char* TPing = "ping";
inline constexpr const char* TPong = "pong";
inline constexpr const char* TGoaway = "goaway";
inline constexpr const char* TCatalog = "catalog";

struct Error {
    std::string code, message;
};

struct Frame {
    std::string t;
    std::string id;         // request id "<instance>.<random>"
    std::string device_id;
    std::string tenant;     // hello: the tenant the device was enrolled into; gateway-internal: stamped on replies
    std::string proto, agent_version, nonce_c, nonce_g, sig, session, assertion, kind;
    nlohmann::json rpc;     // MCP JSON-RPC object (req: request, res: response)
    int64_t deadline_ms = 0;
    int64_t retry_after_ms = 0;
    std::optional<Error> error;
    nlohmann::json tools;   // catalog

    std::string dump() const;
    static std::optional<Frame> parse(const std::string& text);  // nullopt on malformed input
};

// What the device signs to prove possession of its enrolled key (bound to tenant and device id).
std::string proof_input(const std::string& nonce_g, const std::string& nonce_c, const std::string& tenant, const std::string& device_id);
std::string new_request_id(const std::string& instance);
std::string instance_of(const std::string& request_id);

}  // namespace ffy::proto
