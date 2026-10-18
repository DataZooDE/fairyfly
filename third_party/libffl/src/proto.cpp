#include "proto.hpp"

#include <cctype>
#include <vector>

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
    if (!caps.empty()) j["caps"] = caps;
    if (!trail.is_null()) j["trail"] = trail;
    if (!step.is_null()) j["step"] = step;
    put("data", data);
    if (t == TShot) {
        j["seq"] = seq;
        j["last"] = last;
    }
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
    if (j.contains("caps") && j["caps"].is_array())
        for (auto& c : j["caps"])
            if (c.is_string() && f.caps.size() < 16) f.caps.push_back(c);
    if (j.contains("trail")) f.trail = j["trail"];
    if (j.contains("step")) f.step = j["step"];
    f.data = s("data");
    if (j.contains("seq") && j["seq"].is_number_integer()) f.seq = j["seq"];
    if (j.contains("last") && j["last"].is_boolean()) f.last = j["last"];
    return f;
}

nlohmann::json TrailPolicy::to_json() const {
    return {{"v", 1}, {"on", on}, {"since", since}, {"mode", mode}, {"retention_days", retention_days}, {"blur", blur}};
}

TrailPolicy TrailPolicy::from_json(const nlohmann::json& j) {
    TrailPolicy p;
    if (!j.is_object() || !j.contains("on") || !j["on"].is_boolean()) return p;
    p.on = j["on"];
    if (j.contains("since") && j["since"].is_number_integer()) p.since = j["since"];
    if (j.contains("mode") && j["mode"].is_string() && (j["mode"] == "every" || j["mode"] == "changes")) p.mode = j["mode"];
    if (j.contains("retention_days") && j["retention_days"].is_number_integer()) p.retention_days = j["retention_days"];
    if (j.contains("blur") && j["blur"].is_boolean()) p.blur = j["blur"];
    return p;
}

std::string proof_input(const std::string& g, const std::string& c, const std::string& t, const std::string& d) { return sha256("ffl1|" + g + "|" + c + "|" + t + "|" + d); }
std::string new_request_id(const std::string& instance) { return instance + "." + rand_hex(16); }
std::string instance_of(const std::string& id) {
    auto i = id.find('.');
    return i == std::string::npos || i == 0 ? std::string() : id.substr(0, i);
}

}  // namespace ffy::proto

namespace ffy::proto {
namespace {
// The one list for the desktop (libffl) and the gateway. Needles match anywhere in the upper-cased field id and label;
// needles starting with '=' match only a whole word (letters and digits), so "PIN" does not match "SHIPPING".
bool any_of(const std::string& field, const std::string& label, std::initializer_list<const char*> needles) {
    std::string hay;
    for (char c : field + " " + label) hay += char(std::toupper(static_cast<unsigned char>(c)));
    std::vector<std::string> words;
    std::string w;
    for (char c : hay + " ") {
        if (std::isalnum(static_cast<unsigned char>(c))) w += c;
        else if (!w.empty()) { words.push_back(w); w.clear(); }
    }
    for (auto n : needles) {
        if (n[0] == '=') {
            for (auto& x : words)
                if (x == n + 1) return true;
        } else if (hay.find(n) != std::string::npos) {
            return true;
        }
    }
    return false;
}
}  // namespace

bool password_field(const std::string& field, const std::string& label) {
    // SAP GUI scripting ids name the control type: "pwd" is a password field (e.g. wnd[0]/usr/pwdRSYST-BCODE)
    auto slash = field.rfind('/');
    if (field.compare(slash == std::string::npos ? 0 : slash + 1, 3, "pwd") == 0) return true;
    return any_of(field, label, {"PASSW", "PWD", "KENNW", "BCODE", "NCODE", "NEWCODE", "PASSCODE", "PASSPHRASE", "GEHEIMZAHL", "ZUGANGSCODE",
                                 "SECRET", "TOKEN", "=PIN"});
}

bool bank_field(const std::string& field, const std::string& label) {
    return any_of(field, label, {"BANKN", "BANKL", "BANKS", "BANKK", "BKONT", "BKREF", "KOINH", "ACCNAME", "IBAN", "SWIFT", "BIC", "CCNUM",
                                 "CCARD", "CARD NUMBER", "KARTENNUMMER", "ACCOUNT NUMBER", "ACCOUNT HOLDER", "KONTONUMMER", "KONTONR",
                                 "KONTOINHABER", "BANKLEITZAHL", "BANK KEY", "BANKSCHL", "SORT CODE", "ROUTING NUMBER"});
}
}  // namespace ffy::proto
