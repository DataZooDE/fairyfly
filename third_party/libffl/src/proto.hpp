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
// gateway -> desktop: the link policy of this desktop changed (LinkPolicy in `policy`; also in ready.policy). Sent to
// every desktop; one from before approval model v2 ignores the frame and the ready field.
inline constexpr const char* TPolicy = "policy";

inline constexpr const char* kCapTrail = "trail/1";           // records steps and screenshots
inline constexpr const char* kCapTrailBlur = "trail-blur/1";  // can blur password and bank fields on screenshots
// The desktop applies the grant's transaction list (grants.tcodes in the assertion) to every call with fairyfly's own
// --tcode rules: the open transaction must be listed, no menus, only keys that stay in the transaction, a re-check after
// the call. The gateway sends calls under a transaction list to screen tools only to desktops that announce it.
inline constexpr const char* kCapTcodes = "tcodes/1";
// A cancel frame stops the call on the desktop (a batch runs no further step) and the desktop still sends the res,
// with the steps that ran; the gateway waits a moment for it after a timeout.
inline constexpr const char* kCapCancel = "cancel/1";
// The desktop reports its approval decision (link::ApprovalResult): res.approval on every reply, and, when the req says
// approval "await", an evt (kind "approval") as soon as the approval is passed, before the call runs (and a cancel that
// crossed it still stops the call). Without that evt by its approval deadline the gateway cancels and, after a short
// grace, answers APPROVAL_EXPIRED (before the assistant's MCP client gives up). res.approval is desktop-reported.
inline constexpr const char* kCapApproval = "approval/1";
// Approval model v2: the desktop applies the link policy (ApprovalPrompt::enforced, refuses anything but a person's yes
// while enforced). An organisation that enforces the question sends changes only to desktops that announce it.
inline constexpr const char* kCapApprovalPolicy = "approval-policy/1";
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

// What the organisation decided for its desktops beyond a single call (approval model v2, 2026-10-25). Sent in
// ready.policy on connect and in a policy frame whenever an admin changes it. Anything malformed reads as the default.
struct LinkPolicy {
    // The organisation's approval is "always" (the admin enforces it): every change asks the person at the desktop; the
    // tray's "Allow changes in SAP" and the window's "Don't ask again" are greyed out. false: the user decides ("user").
    // The gateway also signs it into each call (grants.approval "always"), which decides that call (link::Agent).
    bool approval_enforced = false;
    bool allow_sap_startup = false;  // admin availability only; each call still needs a signed grant
    nlohmann::json to_json() const;  // {"v":1,"approval_enforced":bool}
    static LinkPolicy from_json(const nlohmann::json& j);
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
    nlohmann::json policy;  // ready and policy frames: LinkPolicy
    std::string data;
    // approval (kCapApproval): req "await"; res and evt (kind "approval"): approved|remembered|standing|denied|expired|locked|none
    std::string approval;
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
