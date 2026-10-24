#include "link_pair.hpp"

#include <algorithm>
#include <istream>
#include <ostream>

#include <thread>

#include "http_client.hpp"

namespace ffy::link {

using json = nlohmann::json;

namespace {
HttpResponse post(const std::string& url, const json& body) {
    HttpRequest r;
    r.method = "POST";
    r.url = url;
    r.headers = {{"Content-Type", "application/json"}};
    r.body = body.dump();
    r.timeout_ms = 15000;
    return http_do(r);
}
}  // namespace

PairingClient::PairingClient(std::string base, std::string host, std::string os, std::string version)
    : base_(std::move(base)), host_(std::move(host)), os_(std::move(os)), version_(std::move(version)) {
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

PairingDisplay PairingClient::start() {
    key_ = ec_generate();
    auto res = post(base_ + "/pair/start", {{"key", jwk_from_ec(key_).to_json()}, {"host", host_}, {"os", os_}, {"version", version_}});
    auto j = json::parse(res.body, nullptr, false);
    if (res.status != 200 || !j.is_object()) throw Error("pairing could not start (HTTP " + std::to_string(res.status) + ")");
    pid_ = j.value("pid", "");
    status_ = "pending";
    return {j.value("link", ""), j.value("code", ""), j.value("match", ""), j.value("expires_in", 0)};
}

std::optional<State> PairingClient::collect(const std::string& pid, const PKey& key) {
    std::string sig = b64url_encode(es256_sign(key, "ffy-pair-collect|" + pid));
    auto res = post(base_ + "/pair/collect", {{"pid", pid}, {"sig", sig}});
    auto j = json::parse(res.body, nullptr, false);
    if (res.status == 202) return std::nullopt;
    if (res.status != 200 || !j.is_object()) {
        if (j.is_object() && j.contains("status")) status_ = j.value("status", "expired");
        else if (res.status == 404 || res.status == 410) status_ = "expired";
        return std::nullopt;
    }
    State st;
    st.key = key;
    st.device_id = j.value("device_id", ""); st.tenant = j.value("tenant", ""); st.link_url = j.value("link_url", "");
    st.issuer = j.value("issuer", ""); st.email = j.value("email", ""); st.tenant_name = j.value("tenant_name", "");
    st.provider = j.value("provider", ""); st.email_verified = j.value("email_verified", false); st.idp_directory = j.value("idp_directory", "");
    for (auto& k : j.value("gateway_keys", json::array())) st.gateway_keys.push_back(Jwk::from_json(k));
    if (st.device_id.empty() || st.gateway_keys.empty()) throw Error("pairing: malformed result");
    status_ = "approved";
    return st;
}

std::optional<State> PairingClient::poll() {
    if (pid_.empty()) throw Error("pairing not started");
    return collect(pid_, key_);
}

bool PairingClient::collect_as(const std::string& pid, const PKey& key) { return collect(pid, key).has_value(); }

std::optional<State> PairingClient::wait(int timeout_s, int interval_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < end) {
        if (auto st = poll()) return st;
        if (status_ != "pending") return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
    return std::nullopt;
}

bool confirm_pairing(const State& st, std::istream& in, std::ostream& out) {
    // The organisation's name is chosen by whoever created it; the e-mail address is only trustworthy when the
    // identity provider verified it. Both facts are shown, so a stranger's organisation is recognisable.
    std::string who = st.provider == "google" ? "Google" : st.provider == "microsoft" ? "Microsoft Entra" : st.provider;
    out << "\nThis desktop will act for " << st.email;
    if (st.email_verified) out << " (address verified by " << who << ")";
    else out << " (signed in with " << (who.empty() ? "an unknown provider" : who) << "; this address was NOT verified by it"
             << (st.idp_directory.empty() ? "" : ", directory " + st.idp_directory) << ")";
    out << "\nin the organisation \"" << (st.tenant_name.empty() ? st.tenant : st.tenant_name) << "\" (id " << st.tenant << ").\n"
        << "AI assistants of that organisation can then read this computer's SAP screens.\n";
    if (!st.email_verified) out << "WARNING: only continue if YOU just approved this on your own phone.\n";
    out << "If this is not your own account, answer no.\nType yes to confirm: " << std::flush;
    std::string answer;
    if (!std::getline(in, answer)) return false;
    return lower(trim(answer)) == "yes";
}

std::string format_pairing_number(const std::string& match) {
    if (match.size() != 6 || !std::all_of(match.begin(), match.end(), [](char c) { return c >= '0' && c <= '9'; })) return match;
    return match.substr(0, 3) + " " + match.substr(3);
}

}  // namespace ffy::link
