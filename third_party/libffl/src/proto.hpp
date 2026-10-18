// `ffl/1` link protocol: JSON text frames on one WebSocket (subprotocol "ffl.v1").
//
// Keepalive: the DEVICE sends {"t":"ping"} every 20 s and the gateway answers {"t":"pong"}; either side closes after
// 60 s of silence. Application-level pings are used on purpose: WebSocket control frames are written from the
// reading thread by blocking Beast streams and would race with concurrent frame writes.
#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

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
// Audit trail (docs/AUDIT_TRAIL.md). A desktop that supports it says so in hello.caps; others never see these frames.
inline constexpr const char* TTrail = "trail";  // gateway -> desktop: the trail policy of this desktop changed
inline constexpr const char* TStep = "step";    // desktop -> gateway: the record of one step (after its res)
inline constexpr const char* TShot = "shot";    // desktop -> gateway: one chunk of a screenshot of that step

inline constexpr const char* kCapTrail = "trail/1";           // records steps and screenshots
inline constexpr const char* kCapTrailBlur = "trail-blur/1";  // can blur password and bank fields on screenshots
inline constexpr size_t kShotMaxBytes = 4u << 20;             // one screenshot, encoded (PNG or WebP)
inline constexpr size_t kShotChunkBytes = 512u << 10;         // raw bytes per shot frame (base64 in the frame)
inline constexpr size_t kStepMaxBytes = 128u << 10;           // one step frame
inline constexpr size_t kStepMaxChanges = 200;                // field changes per step
inline constexpr size_t kStepMaxText = 256;                   // per field name, label and value
inline constexpr const char* kMasked = "••••";                // a masked field value in a step record
// Field changes: values of password fields are always masked, of bank fields whenever blurring is on. One list for the
// desktop (libffl) and the gateway, matched case-insensitively on the SAP field id and its label.
bool password_field(const std::string& field, const std::string& label);
bool bank_field(const std::string& field, const std::string& label);

struct Error {
    std::string code, message;
};

// The audit-trail policy of one desktop, as the admin set it. Sent in ready.trail and in trail frames; the desktop
// shows the person at the computer a notice while `on`. Anything malformed reads as off.
struct TrailPolicy {
    bool on = false;
    int64_t since = 0;            // unix seconds, when it was turned on
    std::string mode = "every";   // "every": screenshots at every step; "changes": only for steps that change SAP
    int retention_days = 90;      // 30 | 90 | 365: how long screenshots are kept
    bool blur = true;             // blur password and bank fields on screenshots
    nlohmann::json to_json() const;
    static TrailPolicy from_json(const nlohmann::json& j);
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
    // audit trail: hello.caps (what the desktop supports), ready/trail policy (TrailPolicy) and req.trail (this call:
    // {"v":1,"shots":bool,"blur":bool}), step record, shot chunk (kind before|after, seq from 0, last, base64 data)
    std::vector<std::string> caps;
    nlohmann::json trail, step;
    std::string data;
    int seq = 0;
    bool last = false;

    std::string dump() const;
    static std::optional<Frame> parse(const std::string& text);  // nullopt on malformed input
};

// What the device signs to prove possession of its enrolled key (bound to tenant and device id).
std::string proof_input(const std::string& nonce_g, const std::string& nonce_c, const std::string& tenant, const std::string& device_id);
std::string new_request_id(const std::string& instance);
std::string instance_of(const std::string& request_id);

}  // namespace ffy::proto
