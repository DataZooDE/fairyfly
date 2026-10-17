#include "link_enrol.hpp"

#include <httplib.h>

#include <atomic>
#include <future>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "http_client.hpp"

namespace ffy::link {

State enrol(const std::string& gateway_base, const std::string& id_token, const std::string& host, const std::string& version) {
    State st;
    st.key = ec_generate();
    nlohmann::json body = {{"key", jwk_from_ec(st.key).to_json()}, {"host", host}, {"version", version}};
    HttpRequest r;
    r.method = "POST";
    std::string base = gateway_base;
    while (!base.empty() && base.back() == '/') base.pop_back();
    r.url = base + "/enrol";
    r.headers = {{"Authorization", "Bearer " + id_token}, {"Content-Type", "application/json"}};
    r.body = body.dump();
    r.timeout_ms = 20000;
    auto res = http_do(r);
    if (res.status != 200) throw Error("enrolment refused (HTTP " + std::to_string(res.status) + "): " + trim(res.body.substr(0, 300)));
    auto j = nlohmann::json::parse(res.body, nullptr, false);
    if (!j.is_object() || !j.contains("device_id") || !j.contains("gateway_keys") || !j["gateway_keys"].is_array() || j["gateway_keys"].empty())
        throw Error("enrolment: malformed response");
    auto s = [&](const char* k) { return j.value(k, std::string()); };
    st.device_id = s("device_id"); st.link_url = s("link_url"); st.issuer = s("issuer"); st.tenant = s("tenant"); st.email = s("email");
    for (auto& k : j["gateway_keys"]) st.gateway_keys.push_back(Jwk::from_json(k));
    if (st.link_url.empty()) throw Error("enrolment: gateway has no public link URL configured (FFY_PUBLIC_LINK_URL)");
    return st;
}

std::string google_login(const std::string& client_id, const std::string& client_secret, const std::function<void(const std::string&)>& open_url,
                         const std::string& auth_endpoint, const std::string& token_endpoint) {
    // loopback redirect (RFC 8252): a one-shot listener on 127.0.0.1 with an OS-chosen port
    httplib::Server cb;
    int port = cb.bind_to_any_port("127.0.0.1");
    if (port <= 0) throw Error("cannot open a loopback port for the sign-in redirect");
    std::string redirect = "http://127.0.0.1:" + std::to_string(port) + "/callback";
    std::string verifier = b64url_encode(rand_bytes(32)), state = b64url_encode(rand_bytes(16));
    std::string challenge = b64url_encode(sha256(verifier));
    std::string url = auth_endpoint + "?client_id=" + url_encode(client_id) + "&redirect_uri=" + url_encode(redirect) +
                      "&response_type=code&scope=" + url_encode("openid email") + "&code_challenge=" + challenge +
                      "&code_challenge_method=S256&state=" + state + "&prompt=select_account";

    std::promise<std::string> code_p;
    auto code_f = code_p.get_future();
    std::atomic<bool> answered{false};
    cb.Get("/callback", [&](const httplib::Request& req, httplib::Response& res) {
        auto query = req.target.find('?') == std::string::npos ? std::string() : req.target.substr(req.target.find('?') + 1);
        bool ok = query_get(query, "state") == state && !query_get(query, "code").empty();
        res.set_content(ok ? "<html><body>fairyfly: signed in - you can close this tab.</body></html>"
                           : "<html><body>fairyfly: sign-in failed.</body></html>",
                        "text/html");
        if (answered.exchange(true)) return;
        if (ok) code_p.set_value(query_get(query, "code"));
        else code_p.set_exception(std::make_exception_ptr(Error("login failed: " + query_get(query, "error"))));
    });
    std::thread srv([&] { cb.listen_after_bind(); });
    struct Stop { httplib::Server& s; std::thread& t; ~Stop() { s.stop(); if (t.joinable()) t.join(); } } stop{cb, srv};

    open_url(url);
    if (code_f.wait_for(std::chrono::minutes(5)) != std::future_status::ready) throw Error("login timed out");
    std::string code = code_f.get();

    HttpRequest r;
    r.method = "POST";
    r.url = token_endpoint;
    r.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    r.body = "code=" + url_encode(code) + "&client_id=" + url_encode(client_id) + "&client_secret=" + url_encode(client_secret) +
             "&redirect_uri=" + url_encode(redirect) + "&grant_type=authorization_code&code_verifier=" + verifier;
    auto res = http_do(r);
    auto j = nlohmann::json::parse(res.body, nullptr, false);
    if (!j.is_object() || !j.contains("id_token")) throw Error("token exchange failed: " + (j.is_object() ? j.value("error_description", std::string("?")) : std::string("?")));
    return j["id_token"];
}

void open_browser(const std::string& url) {
    std::cerr << "Sign in with your company account in the browser. If it does not open, visit:\n" << url << "\n";
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    if (std::system(("open \"" + url + "\" >/dev/null 2>&1 &").c_str()) != 0) {}
#else
    if (std::system(("xdg-open \"" + url + "\" >/dev/null 2>&1 &").c_str()) != 0) {}
#endif
}

}  // namespace ffy::link
