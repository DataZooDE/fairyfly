#include "proto.hpp"

#include "crypto.hpp"
#include "util.hpp"

namespace ffy::proto {

std::string Frame::dump() const {
    nlohmann::json j;
    j["t"] = t;
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) j[k] = v;
    };
    put("id", id); put("device_id", device_id); put("tenant", tenant); put("proto", proto); put("agent_version", agent_version);
    put("nonce_c", nonce_c); put("nonce_g", nonce_g); put("sig", sig); put("session", session);
    put("assertion", assertion); put("kind", kind);
    if (!rpc.is_null()) j["rpc"] = rpc;
    if (!tools.is_null()) j["tools"] = tools;
    if (deadline_ms) j["deadline_ms"] = deadline_ms;
    if (retry_after_ms) j["retry_after_ms"] = retry_after_ms;
    if (error) j["error"] = {{"code", error->code}, {"message", error->message}};
    return j.dump();
}

std::optional<Frame> Frame::parse(const std::string& text) {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("t") || !j["t"].is_string()) return std::nullopt;
    Frame f;
    auto s = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    f.t = s("t"); f.id = s("id"); f.device_id = s("device_id"); f.tenant = s("tenant"); f.proto = s("proto"); f.agent_version = s("agent_version");
    f.nonce_c = s("nonce_c"); f.nonce_g = s("nonce_g"); f.sig = s("sig"); f.session = s("session");
    f.assertion = s("assertion"); f.kind = s("kind");
    if (j.contains("rpc")) f.rpc = j["rpc"];
    if (j.contains("tools")) f.tools = j["tools"];
    if (j.contains("deadline_ms") && j["deadline_ms"].is_number_integer()) f.deadline_ms = j["deadline_ms"];
    if (j.contains("retry_after_ms") && j["retry_after_ms"].is_number_integer()) f.retry_after_ms = j["retry_after_ms"];
    if (j.contains("error") && j["error"].is_object()) {
        Error e;
        if (j["error"].contains("code") && j["error"]["code"].is_string()) e.code = j["error"]["code"];
        if (j["error"].contains("message") && j["error"]["message"].is_string()) e.message = j["error"]["message"];
        f.error = e;
    }
    return f;
}

std::string proof_input(const std::string& g, const std::string& c, const std::string& t, const std::string& d) { return sha256("ffl1|" + g + "|" + c + "|" + t + "|" + d); }
std::string new_request_id(const std::string& instance) { return instance + "." + rand_hex(16); }
std::string instance_of(const std::string& id) {
    auto i = id.find('.');
    return i == std::string::npos || i == 0 ? std::string() : id.substr(0, i);
}

}  // namespace ffy::proto
