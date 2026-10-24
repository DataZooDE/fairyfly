#include "link_agent.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <thread>

#include "crypto.hpp"
#include "http_client.hpp"
#include "policy.hpp"
#include "ws_client.hpp"

namespace ffy::link {

using json = nlohmann::json;

Agent::Agent(State s, AgentConfig c) : st_(std::move(s)), cfg_(std::move(c)), ver_(st_.issuer, st_.tenant, st_.gateway_keys) {
    tools_ = cfg_.local ? cfg_.local : http_local_tools(cfg_.local_url, cfg_.local_token);
}
Agent::~Agent() {
    for (int i = 0; i < 100 && active_ > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// ---- local fairyfly ---------------------------------------------------------------------------------------------------

json Agent::call_local(const std::string& method, const json& params, int timeout_ms, const CallInfo& info) {
    {
        std::unique_lock<std::mutex> l(local_mu_);
        if (!local_cv_.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return local_busy_ < std::max(1, cfg_.max_local_concurrency); }))
            throw Error("local fairyfly busy: waited " + std::to_string(timeout_ms / 1000) + " s for a free connection");
        local_busy_++;
    }
    struct Slot {
        Agent& a;
        ~Slot() {
            { std::lock_guard<std::mutex> l(a.local_mu_); a.local_busy_--; }
            a.local_cv_.notify_one();
        }
    } slot{*this};
    return tools_->rpc(method, params, timeout_ms, info);
}

namespace {
class HttpLocalTools : public LocalTools {
public:
    HttpLocalTools(std::string url, std::string token) : url_(std::move(url)), token_(std::move(token)) {}
    json rpc(const std::string& method, const json& params, int timeout_ms, const CallInfo&) override {
        HttpRequest r;
        r.method = "POST";
        r.url = url_;
        r.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
        if (!token_.empty()) r.headers.push_back({"Authorization", "Bearer " + token_});
        r.body = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}}.dump();
        r.timeout_ms = timeout_ms;
        r.max_body = 7u << 20;
        auto res = http_do(r);
        if (res.status != 200) throw Error("local fairyfly: HTTP " + std::to_string(res.status) + " " + trim(res.body.substr(0, 200)));
        if (res.header("content-type").find("text/event-stream") != std::string::npos) {
            json last;
            std::istringstream is(res.body);
            std::string line;
            while (std::getline(is, line)) {
                if (line.rfind("data:", 0) != 0) continue;
                json j = json::parse(trim(line.substr(5)), nullptr, false);
                if (j.is_object() && (j.contains("result") || j.contains("error"))) last = j;
            }
            if (last.is_null()) throw Error("local fairyfly returned no result");
            return last;
        }
        json j = json::parse(res.body, nullptr, false);
        if (!j.is_object()) throw Error("local fairyfly returned malformed JSON");
        return j;
    }

private:
    std::string url_, token_;
};
}  // namespace

std::shared_ptr<LocalTools> http_local_tools(std::string url, std::string bearer_token) {
    return std::make_shared<HttpLocalTools>(std::move(url), std::move(bearer_token));
}

bool Agent::refresh_local() {
    try {
        json r = call_local("tools/list", json::object(), 15000);
        if (!r.contains("result") || !r["result"].contains("tools") || !r["result"]["tools"].is_array()) return false;
        std::map<std::string, LocalTool> m;
        for (auto& t : r["result"]["tools"]) {
            if (!t.is_object() || !t.contains("name")) continue;
            LocalTool lt;
            lt.destructive = t.contains("annotations") && t["annotations"].value("destructiveHint", false);
            m[t["name"].get<std::string>()] = lt;
        }
        std::lock_guard<std::mutex> l(mu_);
        local_ = std::move(m);
        local_tools_ = r["result"]["tools"];
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("local fairyfly not reachable: {}", e.what());
        return false;
    }
}

// ---- enforcement -------------------------------------------------------------------------------------------------------

bool Agent::approved(const assertion::Claims& c, const std::string& tool) {
    {
        std::lock_guard<std::mutex> l(mu_);
        if (now_unix() < approved_until_) return true;
    }
    std::string client = c.client.empty() ? "unknown client" : c.client.substr(0, 24);
    std::string text = "An AI assistant (" + client + ") acting for " + c.sub + " wants to change data in your SAP session using '" + tool +
                       "'.\n\nAllow SAP changes for the next " + std::to_string(cfg_.approve_window_s / 60) + " minutes?";
    if (!cfg_.approver->approve(text)) return false;
    std::lock_guard<std::mutex> l(mu_);
    approved_until_ = now_unix() + cfg_.approve_window_s;
    return true;
}

static proto::Frame fail(const std::string& id, const std::string& code, const std::string& msg) {
    proto::Frame f;
    f.t = proto::TRes; f.id = id; f.error = proto::Error{code, msg};
    return f;
}

Agent::Outcome Agent::serve(const proto::Frame& req, std::shared_ptr<const std::atomic<bool>> cancel) {
    auto fail = [](const std::string& id, const std::string& code, const std::string& msg) { return Outcome{link::fail(id, code, msg), {}}; };
    assertion::Claims c;
    try {
        c = ver_.verify(req.assertion, st_.device_id, req.id);
    } catch (const Error& e) {
        spdlog::warn("assertion rejected ({}): {}", req.id, e.what());
        return fail(req.id, "ASSERTION_REJECTED", "the desktop rejected the gateway's authorization");
    }
    const json& rpc = req.rpc;
    std::string tool;
    json args = json::object();
    if (!rpc.is_object() || rpc.value("method", std::string()) != "tools/call" || !rpc.contains("params") || !rpc["params"].is_object() ||
        !rpc["params"].contains("name") || !rpc["params"]["name"].is_string())
        return fail(req.id, "BAD_REQUEST", "only tools/call is served over the link");
    tool = rpc["params"]["name"];
    if (rpc["params"].contains("arguments") && rpc["params"]["arguments"].is_object()) args = rpc["params"]["arguments"];
    auto deny = [&](const std::string& code, const std::string& msg) {
        spdlog::info("denied on desktop: tool={} user={} code={}", tool, c.sub, code);
        return fail(req.id, code, msg);
    };
    const auto& g = c.grants;
    if (!policy::tool_allowed(g.tools, tool)) return deny("POLICY_DENIED", "tool not granted");

    LocalTool lt;
    bool known = false, have_fill = false;
    auto lookup = [&] {
        std::lock_guard<std::mutex> l(mu_);
        auto i = local_.find(tool);
        known = i != local_.end();
        if (known) lt = i->second;
        have_fill = local_.count("gui_element_fill") > 0;
    };
    lookup();
    if (!known) {
        if (!refresh_local()) return fail(req.id, "LOCAL_UNAVAILABLE", "fairyfly is not reachable on this desktop");
        lookup();
        if (!known) return deny("UNKNOWN_TOOL", "tool not offered by local fairyfly");
    }
    // the steps of a batch, transactions, the command field, screenshots: the same check as on the gateway
    const auto checked = policy::check_call(g, tool, args);
    if (!checked.ok()) return deny(checked.code, checked.message);
    // The read-only promise: a read-only grant must not run against a write-mode fairyfly.
    if (have_fill && !g.allow_write && !cfg_.allow_local_write_mode && !tools_->per_call_read_only())
        return deny("LOCAL_WRITE_MODE", "local fairyfly runs in write mode but the policy grants read-only; restart fairyfly without --allow-write");
    if (checked.writes && !g.allow_write) return deny("WRITE_DENIED", "write tools are not granted");
    // a transaction list binds the screen only where fairyfly applies it per call (the gateway checks the same caps)
    if (!cfg_.local_enforces_tcodes && policy::needs_desktop_tcodes(g, tool, checked))
        return deny("TCODE_UNSUPPORTED", "your transactions are restricted, and the fairyfly on this desktop cannot enforce that; update fairyfly");
    if (g.allow_write && (checked.writes || lt.destructive) && g.approval != "none") {
        // phone: the user approved this call with a passkey; the gateway put the approval id into the signed
        // assertion. phone_or_desktop: without it the desktop dialog decides. device/desktop: the dialog decides.
        bool phone_ok = !c.approval.empty() && (g.approval == "phone" || g.approval == "phone_or_desktop");
        if (g.approval == "phone" && !phone_ok) return deny("APPROVAL_REQUIRED", "this change needs your approval on the phone");
        if (!phone_ok && !approved(c, tool)) return deny("APPROVAL_DENIED", "the person at the desktop did not approve the change");
        if (phone_ok) spdlog::info("write approved on the phone: tool={} approval={}", tool, c.approval);
    }

    int64_t budget = req.deadline_ms > 0 ? std::min<int64_t>(req.deadline_ms + 1000, 130000) : 125000;
    try {
        CallInfo info{st_.tenant, c.sub, c.client, req.id, g.allow_write, g.tcodes, cancel};
        // audit trail: only while the notice is up (policy on) and when this call asks for it
        bool record = false;
        StepRequest sr;
        std::string withheld;
        if (cfg_.recorder && req.trail.is_object()) {
            bool blur_policy = true;
            {
                std::lock_guard<std::mutex> l(mu_);
                record = trail_.on;
                blur_policy = trail_.blur;
            }
            if (record) {
                sr.request_id = req.id; sr.tool = tool; sr.args = args; sr.info = info;
                sr.shots = req.trail.contains("shots") && req.trail["shots"].is_boolean() && req.trail["shots"].get<bool>();
                // either says blur: blur (a policy change racing a call errs on the private side)
                sr.blur = blur_policy || !req.trail.contains("blur") || !req.trail["blur"].is_boolean() || req.trail["blur"].get<bool>();
                if (sr.shots && sr.blur && !cfg_.recorder->can_blur()) {
                    sr.shots = false;
                    withheld = "blur_unsupported";
                }
                sr.local = [this, info](const std::string& t, const json& a) { return call_local("tools/call", {{"name", t}, {"arguments", a}}, 15000, info); };
                try { cfg_.recorder->before(sr); } catch (const std::exception& e) { spdlog::warn("trail: before {} failed: {}", tool, e.what()); }
            }
        }
        json out = call_local("tools/call", {{"name", tool}, {"arguments", args}}, int(budget), info);
        spdlog::info("served: tool={} user={} client={}", tool, c.sub, c.client);
        Outcome o;
        if (record) {
            StepRecord rec;
            try {
                rec = cfg_.recorder->after(sr, out.contains("result") ? out["result"] : json::object());
            } catch (const std::exception& e) {
                spdlog::warn("trail: after {} failed: {}", tool, e.what());
            }
            if (rec.withheld.empty()) rec.withheld = withheld;
            o.trail = trail_frames(sr, std::move(rec));
        }
        // field changes reported by local fairyfly are for the audit trail only, never for the assistant
        if (out.contains("result") && out["result"].is_object() && out["result"].contains("_meta") && out["result"]["_meta"].is_object()) {
            out["result"]["_meta"].erase("fairyfly/changes");
            if (out["result"]["_meta"].empty()) out["result"].erase("_meta");
        }
        o.res.t = proto::TRes; o.res.id = req.id; o.res.rpc = std::move(out);
        return o;
    } catch (const std::exception& e) {
        spdlog::warn("local call failed: tool={} err={}", tool, e.what());
        return fail(req.id, "LOCAL_ERROR", e.what());
    }
}

// ---- audit trail ----------------------------------------------------------------------------------------------------

namespace {
std::string clip(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80) k--;  // not inside a UTF-8 sequence
    return s.substr(0, k);
}
bool image_ok(const std::string& mime, const std::string& b) {
    if (mime == "image/png") return b.size() > 8 && b.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0;
    if (mime == "image/webp") return b.size() > 12 && b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0;
    return false;
}
}  // namespace

std::vector<proto::Frame> Agent::trail_frames(const StepRequest& s, StepRecord rec) {
    json changes = json::array();
    for (auto& c : rec.changes) {
        if (changes.size() >= proto::kStepMaxChanges) break;
        // never a password; no bank data while blurring is on (the same rule as for the screenshots)
        bool mask = proto::password_field(c.field, c.label) || (s.blur && proto::bank_field(c.field, c.label));
        changes.push_back({{"field", clip(c.field, proto::kStepMaxText)}, {"label", clip(c.label, proto::kStepMaxText)},
                           {"old", mask ? std::string(proto::kMasked) : clip(c.old_value, proto::kStepMaxText)},
                           {"new", mask ? std::string(proto::kMasked) : clip(c.new_value, proto::kStepMaxText)}});
    }
    std::vector<Screenshot> shots;
    std::string withheld = rec.withheld;
    if (s.shots)
        for (auto& sh : rec.shots) {
            bool dup = false;
            for (auto& x : shots) dup = dup || x.kind == sh.kind;
            if ((sh.kind != "before" && sh.kind != "after") || dup || !image_ok(sh.mime, sh.bytes)) {
                if (withheld.empty()) withheld = "bad_type";
                continue;
            }
            if (sh.bytes.size() > proto::kShotMaxBytes) {
                withheld = "too_large";
                continue;
            }
            shots.push_back(std::move(sh));
        }
    json meta = json::array();
    for (auto& sh : shots) meta.push_back({{"kind", sh.kind}, {"mime", sh.mime}, {"bytes", sh.bytes.size()}, {"sha256", hex_encode(sha256(sh.bytes))}});
    proto::Frame st;
    st.t = proto::TStep; st.id = s.request_id;
    st.step = {{"v", 1}, {"description", clip(rec.description, 300)}, {"changes", changes}, {"blurred", std::max(0, rec.blurred)}, {"shots", meta}, {"withheld", withheld}};
    while (st.dump().size() > proto::kStepMaxBytes && !st.step["changes"].empty()) {  // drop changes from the end
        st.step["changes"].erase(st.step["changes"].size() - 1);
        st.step["truncated"] = true;
    }
    std::vector<proto::Frame> out{st};
    for (auto& sh : shots)
        for (size_t off = 0, seq = 0; off < sh.bytes.size(); off += proto::kShotChunkBytes, seq++) {
            proto::Frame f;
            f.t = proto::TShot; f.id = s.request_id; f.kind = sh.kind; f.seq = int(seq);
            f.data = b64std_encode(std::string_view(sh.bytes).substr(off, proto::kShotChunkBytes));
            f.last = off + proto::kShotChunkBytes >= sh.bytes.size();
            out.push_back(std::move(f));
        }
    return out;
}

void Agent::set_trail_policy(const proto::TrailPolicy& p) {
    {
        std::lock_guard<std::mutex> l(mu_);
        if (p.to_json() == trail_.to_json()) return;
        trail_ = p;
    }
    if (p.on) spdlog::warn("audit trail ON: your administrator records the steps assistants take on this desktop{}", p.mode == "every" ? ", with screenshots" : "");
    else spdlog::info("audit trail off");
    if (cfg_.recorder) cfg_.recorder->policy(p);
}

std::vector<std::string> Agent::caps() const {
    std::vector<std::string> c;
    if (cfg_.recorder) {
        c.push_back(proto::kCapTrail);
        if (cfg_.recorder->can_blur()) c.push_back(proto::kCapTrailBlur);
    }
    if (cfg_.local_enforces_tcodes) c.push_back(proto::kCapTcodes);
    if (cfg_.local_honours_cancel) c.push_back(proto::kCapCancel);
    return c;
}

namespace {
class McpRecorder : public Recorder {
public:
    void policy(const proto::TrailPolicy& p) override {
        if (p.on)
            std::cerr << "\n*** fairyfly: your administrator turned on the audit trail for this desktop. Every step an AI assistant takes here is "
                         "recorded" << (p.mode == "every" ? ", with screenshots of the SAP screen" : ", with screenshots of steps that change SAP")
                      << " (kept " << p.retention_days << " days). ***\n" << std::flush;
        else
            std::cerr << "\n*** fairyfly: the audit trail for this desktop is off. ***\n" << std::flush;
    }
    bool can_blur() const override { return false; }
    void before(const StepRequest& s) override {
        if (!s.shots) return;
        auto shot = capture(s, "before");
        std::lock_guard<std::mutex> l(mu_);
        if (pending_.size() > 64) pending_.clear();  // a step whose after() never came
        if (shot) pending_[s.request_id] = std::move(*shot);
    }
    StepRecord after(const StepRequest& s, const json& result) override {
        StepRecord r;
        r.description = s.tool;
        for (auto k : {"code", "id"})
            if (s.args.contains(k) && s.args[k].is_string()) r.description += " " + s.args[k].get<std::string>();
        if (result.is_object() && result.contains("_meta") && result["_meta"].is_object() && result["_meta"].contains("fairyfly/changes") &&
            result["_meta"]["fairyfly/changes"].is_array())
            for (auto& c : result["_meta"]["fairyfly/changes"])
                if (c.is_object())
                    r.changes.push_back({c.value("field", ""), c.value("label", ""), c.value("old", ""), c.value("new", "")});
        if (!s.shots) return r;
        {
            std::lock_guard<std::mutex> l(mu_);
            if (auto i = pending_.find(s.request_id); i != pending_.end()) {
                r.shots.push_back(std::move(i->second));
                pending_.erase(i);
            }
        }
        if (auto a = capture(s, "after")) r.shots.push_back(std::move(*a));
        return r;
    }

private:
    static std::optional<Screenshot> capture(const StepRequest& s, const char* kind) {
        try {
            json r = s.local("gui_screen_capture", json::object());
            if (!r.contains("result") || !r["result"].contains("content") || !r["result"]["content"].is_array()) return std::nullopt;
            for (auto& c : r["result"]["content"])
                if (c.is_object() && c.value("type", "") == "image" && c.contains("data") && c["data"].is_string())
                    return Screenshot{kind, c.value("mimeType", "image/png"), b64std_decode(c["data"].get<std::string>())};
        } catch (const std::exception& e) {
            spdlog::warn("trail: screenshot failed: {}", e.what());
        }
        return std::nullopt;
    }
    std::mutex mu_;
    std::map<std::string, Screenshot> pending_;
};
}  // namespace

std::shared_ptr<Recorder> mcp_recorder() { return std::make_shared<McpRecorder>(); }

// ---- connection loop ----------------------------------------------------------------------------------------------------

void Agent::run(const std::atomic<bool>& stop) {
    int backoff_ms = 1000;
    std::mt19937_64 rng{std::random_device{}()};
    while (!stop) {
        int64_t started = now_ms();
        bool had_ready = false;
        std::string err;
        try {
            had_ready = session(stop);
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (stop) return;
        if (now_ms() - started > 60000) backoff_ms = 1000;
        int wait = int(std::uniform_int_distribution<int>(0, backoff_ms)(rng));
        spdlog::warn("link down{}, reconnecting in {} ms", err.empty() ? "" : " (" + err + ")", wait);
        for (int waited = 0; waited < wait && !stop; waited += 100) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (backoff_ms < 60000) backoff_ms *= 2;
        (void)had_ready;
    }
}

bool Agent::session(const std::atomic<bool>& stop) {
    auto ws = WsClient::connect(st_.link_url, proto::kSubprotocol, 15000);
    std::atomic<int64_t> last_rx{now_ms()};
    std::atomic<bool> ready{false}, done{false};
    const int64_t t0 = now_ms();

    // watcher: handshake deadline, 20 s app-level pings, 60 s silence detection, stop request
    std::thread watcher([&] {
        int64_t next_ping = now_ms() + 20000;
        while (!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (stop || (!ready && now_ms() - t0 > 15000) || now_ms() - last_rx > 60000) { ws->abort(); return; }
            if (ready && now_ms() >= next_ping) {
                proto::Frame p;
                p.t = proto::TPing;
                ws->send_text(p.dump());
                next_ping = now_ms() + 20000;
            }
        }
    });
    struct Join { std::atomic<bool>& d; std::thread& t; ~Join() { d = true; if (t.joinable()) t.join(); } } join{done, watcher};

    proto::Frame hello;
    hello.t = proto::THello; hello.device_id = st_.device_id; hello.tenant = st_.tenant; hello.proto = proto::kVersion; hello.agent_version = cfg_.version; hello.nonce_c = rand_hex(16);
    hello.caps = caps();
    if (!ws->send_text(hello.dump())) return false;
    auto m = ws->read(64 * 1024);
    auto ch = m ? proto::Frame::parse(*m) : std::nullopt;
    if (!ch || ch->t != proto::TChallenge) throw Error("handshake: no challenge");
    last_rx = now_ms();
    proto::Frame pf;
    pf.t = proto::TProof;
    pf.sig = b64url_encode(es256_sign(st_.key, proto::proof_input(ch->nonce_g, hello.nonce_c, st_.tenant, st_.device_id)));
    ws->send_text(pf.dump());
    m = ws->read(64 * 1024);
    auto rd = m ? proto::Frame::parse(*m) : std::nullopt;
    if (!rd || rd->t != proto::TReady) throw Error("handshake: not ready (device revoked or unknown?)");
    last_rx = now_ms();
    ready = true;
    spdlog::info("link up: device={} gateway={}", st_.device_id, st_.link_url);
    set_trail_policy(proto::TrailPolicy::from_json(rd->trail));  // a gateway without the audit trail: off

    if (refresh_local() && cfg_.publish_catalog) {
        proto::Frame cf;
        cf.t = proto::TCatalog;
        { std::lock_guard<std::mutex> l(mu_); cf.tools = local_tools_; }
        ws->send_text(cf.dump());
    }

    while (auto msg = ws->read(8u << 20)) {
        last_rx = now_ms();
        auto f = proto::Frame::parse(*msg);
        if (!f) continue;
        if (f->t == proto::TReq) {
            if (active_ >= 32) { ws->send_text(fail(f->id, "SERVER_BUSY", "desktop agent is busy").dump()); continue; }
            auto cancelled = std::make_shared<std::atomic<bool>>(false);
            { std::lock_guard<std::mutex> l(mu_); inflight_[f->id] = cancelled; }
            active_++;
            std::thread([this, f = *f, cancelled, w = ws.get()] {
                Outcome o = serve(f, cancelled);
                // also after a cancel: a gateway that waits for it learns which steps ran; others ignore the late reply
                w->send_text(o.res.dump());
                for (auto& t : o.trail) w->send_text(t.dump());  // the record follows the answer
                { std::lock_guard<std::mutex> l(mu_); inflight_.erase(f.id); }
                active_--;
            }).detach();
        } else if (f->t == proto::TCancel) {
            std::lock_guard<std::mutex> l(mu_);
            if (auto i = inflight_.find(f->id); i != inflight_.end()) *i->second = true;
        } else if (f->t == proto::TTrail) {
            set_trail_policy(proto::TrailPolicy::from_json(f->trail));
        } else if (f->t == proto::TGoaway) {
            spdlog::info("gateway asked to reconnect ({})", f->kind);
            ws->close();
            break;
        }
    }
    // let in-flight requests finish writing before the socket object dies
    for (int i = 0; i < 200 && active_ > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return true;
}

}  // namespace ffy::link
