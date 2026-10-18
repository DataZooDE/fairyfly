// Desktop link agent. Dials the gateway (outbound only), verifies each gateway-signed assertion offline, enforces
// grants and the write gate, and relays accepted calls to the local, unmodified fairyfly MCP endpoint on loopback.
#pragma once
#include <condition_variable>
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
};

class Agent {
public:
    Agent(State state, AgentConfig cfg);
    ~Agent();
    // Connects and reconnects (exponential backoff with full jitter, 1 s .. 60 s) until stop becomes true.
    void run(const std::atomic<bool>& stop);
    // Handles one req frame; returns the res frame. Public for tests.
    proto::Frame process(const proto::Frame& req);
    bool refresh_local();  // reads the local tool catalog; false if fairyfly is unreachable

private:
    struct LocalTool { bool destructive = false; };
    bool session(const std::atomic<bool>& stop);
    bool approved(const assertion::Claims& c, const std::string& tool);
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
};

}  // namespace ffy::link
