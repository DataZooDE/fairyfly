// Desktop link agent. Dials the gateway (outbound only), verifies each gateway-signed assertion offline, enforces
// grants and the write gate, and relays accepted calls to the local, unmodified fairyfly MCP endpoint on loopback.
#pragma once
#include <condition_variable>
#include <functional>
#include <vector>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "approver.hpp"
#include "assertion.hpp"
#include "link_state.hpp"
#include "proto.hpp"

namespace ffy::link {

// What one call to local fairyfly is about (fairyfly audits it with audit_source "link" and applies the grant).
struct CallInfo {
    std::string tenant, user, client, request_id;
    bool write_granted = false;
    // the grant's transaction list (empty: unrestricted); fairyfly applies its --tcode rules with it (kCapTcodes)
    std::vector<std::string> tcodes;
    // set once the gateway cancelled the call (timeout, client gone); null: never. A batch stops before its next step.
    std::shared_ptr<const std::atomic<bool>> cancel;
    bool cancelled() const { return cancel && cancel->load(); }
};

// The tools on this desktop: fairyfly's MCP endpoint over loopback HTTP (ffy-link), or fairyfly's own dispatcher
// in-process (the `fairyfly link` noun, D10). rpc() returns the JSON-RPC response object; throws ffy::Error when
// the tools are unreachable.
class LocalTools {
public:
    virtual ~LocalTools() = default;
    virtual nlohmann::json rpc(const std::string& method, const nlohmann::json& params, int timeout_ms, const CallInfo& info) = 0;
    // true if read-only is enforced per call (in-process); false if it depends on how local fairyfly was started
    virtual bool per_call_read_only() const { return false; }
};
std::shared_ptr<LocalTools> http_local_tools(std::string url, std::string bearer_token);

// ---- audit trail (docs/AUDIT_TRAIL.md) -------------------------------------------------------------------------------
// The desktop records a step only when (1) the gateway's policy with the trail on has reached it, so the person at
// the computer sees the notice (Recorder::policy), and (2) the call itself asks for it (req.trail). After the res
// frame the agent sends one step frame and the screenshots in shot frames.

// Calls one local tool for the recorder (same connection limit as the calls it records). Throws on failure.
using LocalCall = std::function<nlohmann::json(const std::string& tool, const nlohmann::json& args)>;

struct StepRequest {
    std::string request_id, tool;
    nlohmann::json args;
    bool shots = false;  // screenshots before and after (the admin's mode decides per call)
    bool blur = true;    // blur password and bank fields on them
    CallInfo info;
    LocalCall local;
};
struct Screenshot {
    std::string kind;   // "before" | "after"
    std::string mime;   // "image/png" | "image/webp"
    std::string bytes;
};
struct FieldChange {
    std::string field, label, old_value, new_value;  // SAP field id, its label on the screen, the values
};
struct StepRecord {
    std::string description;  // one line, e.g. "Changed quantity and delivery date of item 10"
    std::vector<FieldChange> changes;
    int blurred = 0;          // fields blurred on the screenshots of this step
    std::vector<Screenshot> shots;
    std::string withheld;     // why screenshots were not sent (set by the agent: blur_unsupported, too_large)
};

// What the desktop implements: fairyfly (SAP GUI scripting) captures, diffs and blurs; ffy-link uses mcp_recorder().
class Recorder {
public:
    virtual ~Recorder() = default;
    // The policy changed (connect, admin change). Show the notice while p.on; remove it when off.
    virtual void policy(const proto::TrailPolicy& p) { (void)p; }
    // true if blurring is implemented; otherwise screenshots are withheld while the admin asks for blurring
    virtual bool can_blur() const { return false; }
    virtual void before(const StepRequest& s) { (void)s; }
    virtual StepRecord after(const StepRequest& s, const nlohmann::json& result) = 0;
};
// For unmodified fairyfly over loopback: screenshots from its gui_screen_capture tool (MCP image content), field
// changes from a tools/call result's _meta "fairyfly/changes" ([{field,label,old,new}]). Cannot blur. Prints the notice
// to the log.
std::shared_ptr<Recorder> mcp_recorder();

struct AgentConfig {
    std::string local_url = "http://127.0.0.1:8383/mcp";
    std::string local_token;
    std::shared_ptr<LocalTools> local;  // set: used instead of local_url/local_token
    bool publish_catalog = false;
    bool allow_local_write_mode = false;  // serve read-only grants even if local fairyfly runs with --allow-write
    std::shared_ptr<Approver> approver = fixed_approver(false);
    std::string version = "0.2.0";
    int approve_window_s = 300;
    // calls to local fairyfly at the same time; the rest wait in order. SAP GUI scripting is serial anyway, so a burst
    // from the cloud must not open one local connection (and thread) per call.
    int max_local_concurrency = 4;
    std::shared_ptr<Recorder> recorder;  // set: this desktop offers the audit trail (hello.caps)
    // What the local tools do with CallInfo (announced in hello.caps). Only fairyfly's in-process dispatcher (the
    // `fairyfly link` noun) does either; unmodified fairyfly over loopback HTTP never sees CallInfo.
    bool local_enforces_tcodes = false;  // applies CallInfo::tcodes with fairyfly's --tcode rules (kCapTcodes)
    bool local_honours_cancel = false;   // stops at CallInfo::cancelled() (kCapCancel)
};

class Agent {
public:
    Agent(State state, AgentConfig cfg);
    ~Agent();
    // Connects and reconnects (exponential backoff with full jitter, 1 s .. 60 s) until stop becomes true.
    void run(const std::atomic<bool>& stop);
    // Handles one req frame; returns the res frame and, if the call was recorded, its step and shot frames (sent
    // in this order after it). Public for tests.
    struct Outcome {
        proto::Frame res;
        std::vector<proto::Frame> trail;
    };
    // cancel: set when the gateway cancels this request (the connection loop owns it); null: never cancelled
    Outcome serve(const proto::Frame& req, std::shared_ptr<const std::atomic<bool>> cancel = nullptr);
    proto::Frame process(const proto::Frame& req, std::shared_ptr<const std::atomic<bool>> cancel = nullptr) { return serve(req, std::move(cancel)).res; }
    // ready.trail and trail frames: the policy this desktop runs under (tells the recorder to show the notice)
    void set_trail_policy(const proto::TrailPolicy& p);
    std::vector<std::string> caps() const;  // what hello announces
    bool refresh_local();  // reads the local tool catalog; false if fairyfly is unreachable

private:
    struct LocalTool { bool destructive = false; };
    bool session(const std::atomic<bool>& stop);
    bool approved(const assertion::Claims& c, const std::string& tool, const nlohmann::json& args);
    nlohmann::json call_local(const std::string& method, const nlohmann::json& params, int timeout_ms, const CallInfo& info = {});

    State st_;
    AgentConfig cfg_;
    assertion::Verifier ver_;
    std::mutex mu_;
    std::mutex local_mu_;
    std::condition_variable local_cv_;
    int local_busy_ = 0;
    std::map<std::string, LocalTool> local_;
    nlohmann::json local_tools_ = nlohmann::json::array();
    std::shared_ptr<LocalTools> tools_;
    int64_t approved_until_ = 0;
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> inflight_;
    std::atomic<int> active_{0};
    proto::TrailPolicy trail_;  // guarded by mu_
    std::vector<proto::Frame> trail_frames(const StepRequest& s, StepRecord rec);
};

}  // namespace ffy::link
