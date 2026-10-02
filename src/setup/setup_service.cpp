#include "include/setup/setup_service.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "include/auth/crypto.h"
#include "include/setup/setup_validators.h"

namespace fairyfly::setup {

using nlohmann::json;

namespace {

const char* const kSetupKeys[] = {"server.tls", "server.host", "server.port", "server.allowed_hosts", "server.allow_ip"};

std::string data_dir(SystemProbe& sys) { return join_path(sys.local_app_data(), "fairyfly"); }

bool sid_ok(const std::string& sid) {
    static const std::regex re(R"(^S-1-\d+(-\d+)+$)");
    return std::regex_match(sid, re);
}

std::string yaml_scalar(const YAML::Node& n) {
    if (n.IsScalar()) return n.as<std::string>();
    if (n.IsSequence()) {
        std::string out = "[";
        bool first = true;
        for (const auto& item : n) {
            out += (first ? "" : ", ") + (item.IsScalar() ? item.as<std::string>() : std::string("?"));
            first = false;
        }
        return out + "]";
    }
    return "";
}

void read_config(SystemProbe& sys, const std::string& path, Diagnosis& d) {
    const auto text = sys.read_file(path);
    if (!text) {
        d.config_exists = sys.file_exists(path);
        d.config_unreadable = d.config_exists;
        return;
    }
    d.config_exists = true;
    try {
        const YAML::Node root = YAML::Load(*text);
        if (root.IsMap() && root["server"] && root["server"].IsMap()) {
            const YAML::Node server = root["server"];
            for (const char* key : kSetupKeys) {
                const std::string name = std::string(key).substr(std::string("server.").size());
                if (server[name]) d.config_current[key] = yaml_scalar(server[name]);
            }
        }
    } catch (const std::exception&) {
        d.config_unreadable = true;
    }
}

ErrorInfo host_error_info(const HostError& e) { return {e.code(), e.what(), 1}; }

} // namespace

// ---- manifest io -------------------------------------------------------------------------------------------
std::string manifest_path_for(SystemProbe& sys) { return join_path(data_dir(sys), "mcp-setup.json"); }

std::optional<Manifest> load_manifest(SystemProbe& sys, const std::string& path, bool* unreadable) {
    if (unreadable) *unreadable = false;
    const auto text = sys.read_file(path);
    if (!text) {
        if (unreadable && sys.file_exists(path)) *unreadable = true;
        return std::nullopt;
    }
    try {
        auto m = Manifest::from_json(json::parse(*text));
        if (!m && unreadable) *unreadable = true;
        return m;
    } catch (...) {
        if (unreadable) *unreadable = true;
        return std::nullopt;
    }
}

// ---- diagnose -----------------------------------------------------------------------------------------------
void fill_defaults(Hosts& hosts, Options& o) {
    if (o.hostname.empty() && !o.no_tls) o.hostname = default_hostname(hosts.sys.computer_dns_name());
}

Diagnosis diagnose(Hosts& h, const Options& o) {
    Diagnosis d;
    d.now = h.sys.now();
    d.elevation = h.elevator.elevation_type();
    d.tls = o.cert_mode != CertMode::NoTls;
    d.hostname = o.hostname;
    d.port = o.port;
    d.prefix = url_prefix(d.tls, d.hostname, d.port);
    if (d.tls) d.ipports = ssl_ipports(d.port);
    if (o.user.empty()) {
        d.sid = h.elevator.current_user_sid();
        d.user = h.elevator.current_user_name();
    } else if (auto sid = h.elevator.resolve_user_sid(o.user)) {
        d.sid = *sid;
        d.user = o.user;
    } else {
        d.identity_error = "Cannot resolve the account '" + o.user + "' to a SID";
    }
    const std::string dir = data_dir(h.sys);
    d.cer_path = join_path(dir, cer_file_name(d.hostname));
    d.manifest_path = manifest_path_for(h.sys);
    d.config_path = o.config_path.empty() ? join_path(dir, "mcp.yaml") : o.config_path;
    d.manifest = load_manifest(h.sys, d.manifest_path, &d.manifest_unreadable);

    // http.sys: these queries may need elevation on some systems; then the recorded manifest is the evidence.
    try {
        d.urlacl_sddl = h.http.query_urlacl(d.prefix);
    } catch (const HostError&) {
        if (d.manifest && d.manifest->sid == d.sid &&
            std::find(d.manifest->prefixes.begin(), d.manifest->prefixes.end(), d.prefix) != d.manifest->prefixes.end())
            d.urlacl_sddl = sddl_for_sid(d.sid);
    }
    for (const auto& ipport : d.ipports) {
        try {
            d.ssl[ipport] = h.http.query_sslcert(ipport);
        } catch (const HostError&) {
            if (d.manifest && !d.manifest->thumbprint.empty() &&
                std::find(d.manifest->ipports.begin(), d.manifest->ipports.end(), ipport) != d.manifest->ipports.end())
                d.ssl[ipport] = SslBinding{ipport, d.manifest->thumbprint, normalize_app_id(kAppId), kCertStore};
            else d.ssl[ipport] = std::nullopt;
        }
    }
    try {
        if (o.cert_mode == CertMode::SelfSigned) d.cert = h.certs.find_self_signed(d.hostname);
        else if (o.cert_mode == CertMode::Thumbprint) d.cert = h.certs.find_by_thumbprint(o.thumbprint);
    } catch (const std::exception&) {
        d.cert = CertInfo{};
    }
    if (d.tls) {
        if (d.cert.found) {
            try {
                const auto s = h.certs.cer_file_state(d.cert.thumbprint, d.cer_path, CerFormat::Der);
                d.cer = s == CerFileState::Matches ? CerState::Matches : s == CerFileState::Differs ? CerState::Differs : CerState::Missing;
            } catch (...) {
                d.cer = CerState::Missing;
            }
        } else {
            d.cer = h.sys.file_exists(d.cer_path) ? CerState::Differs : CerState::Missing;
        }
    }
    if (d.tls && o.open_firewall) {
        try {
            d.firewall_exists = h.firewall.exists(firewall_rule_name(d.port));
        } catch (const std::exception&) {
            d.firewall_exists = false;
        }
    }
    d.port_listening = h.sys.tcp_listening("127.0.0.1", d.port);
    if (d.tls && d.port_listening && d.cert.found) d.tls_probe = h.sys.tls_probe(d.hostname, d.port, d.cert.thumbprint);
    read_config(h.sys, d.config_path, d);
    return d;
}

TeardownDiagnosis diagnose_teardown(Hosts& h, const TeardownOptions& o) {
    TeardownDiagnosis d;
    d.now = h.sys.now();
    d.elevation = h.elevator.elevation_type();
    d.manifest_path = manifest_path_for(h.sys);
    d.manifest = load_manifest(h.sys, d.manifest_path, &d.manifest_unreadable);
    d.explicit_target = !o.hostname.empty() || o.port != 0;
    std::vector<int> ports;
    if (d.manifest && !d.explicit_target) {
        d.hostname = d.manifest->hostname;
        d.port = d.manifest->port;
        ports = {d.port};
    } else if (d.explicit_target) {
        d.hostname = !o.hostname.empty() ? o.hostname : (d.manifest ? d.manifest->hostname : default_hostname(h.sys.computer_dns_name()));
        d.port = o.port != 0 ? o.port : (d.manifest ? d.manifest->port : kDefaultTlsPort);
        ports = {d.port};
    } else {
        d.hostname = default_hostname(h.sys.computer_dns_name());
        d.port = kDefaultTlsPort;
        ports = {kDefaultTlsPort, kDefaultNoTlsPort};
    }
    const std::string dir = data_dir(h.sys);
    d.cer_path = d.manifest && !d.manifest->cer_path.empty() ? d.manifest->cer_path : join_path(dir, cer_file_name(d.hostname));
    d.cer_exists = h.sys.file_exists(d.cer_path);
    d.config_path = o.config_path.empty() ? join_path(dir, "mcp.yaml") : o.config_path;
    for (int port : ports) {
        for (const bool tls : {true, false}) {
            const std::string prefix = url_prefix(tls, "", port);
            try {
                if (h.http.query_urlacl(prefix) && std::find(d.urlacls.begin(), d.urlacls.end(), prefix) == d.urlacls.end()) d.urlacls.push_back(prefix);
            } catch (const HostError&) {
                if (d.manifest && std::find(d.manifest->prefixes.begin(), d.manifest->prefixes.end(), prefix) != d.manifest->prefixes.end()) d.urlacls.push_back(prefix);
            }
        }
        for (const auto& ipport : ssl_ipports(port)) {
            try {
                if (const auto b = h.http.query_sslcert(ipport)) (is_our_app_id(b->app_id) ? d.ssl_ours : d.ssl_foreign).push_back(ipport);
            } catch (const HostError&) {
                if (d.manifest && std::find(d.manifest->ipports.begin(), d.manifest->ipports.end(), ipport) != d.manifest->ipports.end()) d.ssl_ours.push_back(ipport);
            }
        }
        if (d.firewall_rule.empty() || port == d.port) d.firewall_rule = firewall_rule_name(port);
        try {
            if (h.firewall.exists(firewall_rule_name(port))) {
                d.firewall_exists = true;
                d.firewall_rule = firewall_rule_name(port);
            }
        } catch (const std::exception&) {
        }
    }
    if (d.manifest && !d.manifest->thumbprint.empty()) {
        try {
            d.cert = h.certs.find_by_thumbprint(d.manifest->thumbprint);
        } catch (const std::exception&) {
        }
    }
    Diagnosis scratch;
    read_config(h.sys, d.config_path, scratch);
    d.config_exists = scratch.config_exists;
    if (const auto text = h.sys.read_file(d.config_path)) d.config_sha256 = auth::sha256_hex(*text);
    const auto tls = scratch.config_current.find("server.tls");
    d.config_says_tls = tls != scratch.config_current.end() && tls->second == "true";
    return d;
}

// ---- elevated work -----------------------------------------------------------------------------------------------
json make_elevated_work(const Plan& p) {
    json w;
    w["schema"] = 1;
    w["operation"] = p.operation;
    json steps = json::array();
    for (const auto& s : p.steps)
        if (s.elevated && (s.status.rfind("would", 0) == 0)) steps.push_back(s.id);
    w["steps"] = steps;
    if (p.operation == "setup") {
        w["hostname"] = p.hostname;
        w["port"] = p.port;
        w["tls"] = p.tls;
        w["sid"] = p.sid;
        w["thumbprint"] = p.thumbprint;
        w["create_cert"] = p.create_cert;
        w["replace_cert"] = p.step("certificate") && p.step("certificate")->status == "would_update";
        w["valid_days"] = kSelfSignedDays;
        w["replace_urlacl"] = p.replace_urlacl;
        w["force_binding"] = p.force_binding;
        w["firewall_rule"] = p.open_firewall ? p.firewall_rule : "";
    } else {
        w["ipports"] = p.teardown.ipports;
        w["prefixes"] = p.teardown.prefixes;
        w["firewall_rule"] = p.teardown.firewall_rule;
        w["cert_thumbprint"] = p.teardown.cert_thumbprint;
    }
    return w;
}

namespace {

json step_json(const std::string& id, const std::string& status, const std::string& detail) {
    return json{{"id", id}, {"status", status}, {"detail", detail}};
}

std::optional<std::string> validate_work(const json& w) {
    if (!w.is_object() || w.value("schema", 0) != 1) return "unsupported plan file";
    const std::string op = w.value("operation", "");
    if (op != "setup" && op != "teardown") return "unknown operation";
    static const std::set<std::string> known = {"certificate", "urlacl", "sslcert", "firewall"};
    if (!w.contains("steps") || !w["steps"].is_array()) return "steps missing";
    for (const auto& s : w["steps"])
        if (!s.is_string() || !known.count(s.get<std::string>())) return "unknown step";
    auto rule_ok = [](const std::string& rule) {
        static const std::regex re(R"(^fairyfly MCP HTTPS \d{1,5}$)");
        return rule.empty() || std::regex_match(rule, re);
    };
    try {
        if (op == "setup") {
            if (auto e = hostname_error(w.at("hostname").get<std::string>())) return "hostname: " + *e;
            if (auto e = port_error(w.at("port").get<int>())) return "port: " + *e;
            const std::string t = w.value("thumbprint", "");
            if (!t.empty())
                if (auto e = thumbprint_error(t)) return "thumbprint: " + *e;
            if (!sid_ok(w.at("sid").get<std::string>())) return "sid is not a SID";
            if (!rule_ok(w.value("firewall_rule", ""))) return "firewall rule name";
        } else {
            for (const auto& ip : w.at("ipports")) {
                int port = 0;
                if (!parse_ipport(ip.get<std::string>(), nullptr, &port)) return "ipport";
            }
            for (const auto& prefix : w.at("prefixes")) {
                static const std::regex re(R"(^(https://\+|http://127\.0\.0\.1):\d{1,5}/mcp/$)");
                if (!std::regex_match(prefix.get<std::string>(), re)) return "prefix";
            }
            const std::string t = w.value("cert_thumbprint", "");
            if (!t.empty())
                if (auto e = thumbprint_error(t)) return "thumbprint: " + *e;
            if (!rule_ok(w.value("firewall_rule", ""))) return "firewall rule name";
        }
    } catch (const std::exception&) {
        return "malformed plan";
    }
    return std::nullopt;
}

bool wants(const json& w, const char* id) {
    for (const auto& s : w["steps"])
        if (s == id) return true;
    return false;
}

json execute_setup_work(const json& w, Hosts& h) {
    json steps = json::array();
    std::string thumb = w.value("thumbprint", "");
    const std::string host = w["hostname"];
    const int port = w["port"];
    const bool tls = w.value("tls", true);
    const std::string prefix = url_prefix(tls, host, port);
    std::string failed;
    std::vector<std::string> bound;   // address families that ended up bound
    auto fail = [&](const std::string& id, const std::string& why) {
        steps.push_back(step_json(id, "failed", why));
        failed = id;
    };
    // certificate
    if (wants(w, "certificate") && failed.empty()) {
        try {
            if (w.value("create_cert", false)) {
                const CertInfo c = h.certs.create_self_signed(host, w.value("valid_days", kSelfSignedDays));
                thumb = c.thumbprint;
                steps.push_back(step_json("certificate", w.value("replace_cert", false) ? "updated" : "created", "thumbprint " + c.thumbprint));
            } else {
                steps.push_back(step_json("certificate", "unchanged", ""));
            }
        } catch (const HostError& e) {
            fail("certificate", e.what());
        } catch (const std::exception& e) {
            fail("certificate", e.what());
        }
    }
    // urlacl
    if (wants(w, "urlacl") && failed.empty()) {
        try {
            const auto existing = h.http.query_urlacl(prefix);
            const std::string sid = w["sid"];
            if (existing && sddl_covers_sid(*existing, sid)) {
                steps.push_back(step_json("urlacl", "unchanged", "already reserved"));
            } else {
                const std::string sddl = merge_sddl(existing.value_or(""), sid);
                if (existing) h.http.remove_urlacl(prefix);
                h.http.add_urlacl(prefix, sddl);
                steps.push_back(step_json("urlacl", existing ? "updated" : "created", prefix));
            }
        } catch (const std::exception& e) {
            fail("urlacl", e.what());
        }
    }
    // sslcert
    if (wants(w, "sslcert") && failed.empty()) {
        try {
            if (thumb.empty()) throw HostError("NO_CERTIFICATE", "no certificate to bind");
            std::string status = "unchanged", detail;
            for (const auto& ipport : ssl_ipports(port)) {
                const auto b = h.http.query_sslcert(ipport);
                if (b && !is_our_app_id(b->app_id) && !w.value("force_binding", false))
                    throw HostError("BINDING_FOREIGN", ipport + " is bound by another application " + b->app_id + " (use --force-binding)");
                if (b && is_our_app_id(b->app_id) && b->thumbprint == thumb) {
                    bound.push_back(ipport);
                    continue;
                }
                try {
                    h.http.set_sslcert(ipport, thumb);
                } catch (const HostError& e) {
                    if (ipport.rfind("[::]", 0) == 0) {   // IPv6 may be disabled on this machine
                        detail += (detail.empty() ? "" : "; ") + ipport + " skipped: " + e.what();
                        continue;
                    }
                    throw;
                }
                bound.push_back(ipport);
                status = b ? "updated" : (status == "updated" ? "updated" : "created");
                if (b) status = "updated";
                detail += (detail.empty() ? "" : "; ") + ipport + " bound";
            }
            steps.push_back(step_json("sslcert", status, detail));
        } catch (const std::exception& e) {
            fail("sslcert", e.what());
        }
    }
    // firewall
    if (wants(w, "firewall") && failed.empty()) {
        try {
            const std::string change = h.firewall.ensure(w.value("firewall_rule", ""), port);
            steps.push_back(step_json("firewall", change, w.value("firewall_rule", "")));
        } catch (const std::exception& e) {
            fail("firewall", e.what());
        }
    }
    json out{{"ok", failed.empty()}, {"steps", steps}, {"thumbprint", thumb}};
    if (!bound.empty()) out["bound_ipports"] = bound;
    if (!failed.empty()) out["error"] = {{"code", "STEP_FAILED"}, {"message", "step " + failed + " failed"}};
    return out;
}

json execute_teardown_work(const json& w, Hosts& h) {
    json steps = json::array();
    std::string failed;
    auto fail = [&](const std::string& id, const std::string& why) {
        steps.push_back(step_json(id, "failed", why));
        failed = id;
    };
    if (wants(w, "sslcert")) {
        try {
            std::string detail;
            for (const auto& j : w["ipports"]) {
                const std::string ip = j;
                const auto b = h.http.query_sslcert(ip);
                if (b && !is_our_app_id(b->app_id)) continue;   // never touch somebody else's binding
                if (b && h.http.remove_sslcert(ip)) detail += (detail.empty() ? "" : ", ") + ip;
            }
            steps.push_back(step_json("sslcert", "removed", detail));
        } catch (const std::exception& e) {
            fail("sslcert", e.what());
        }
    }
    if (wants(w, "urlacl") && failed.empty()) {
        try {
            std::string detail;
            for (const auto& j : w["prefixes"])
                if (h.http.remove_urlacl(j.get<std::string>())) detail += (detail.empty() ? "" : ", ") + j.get<std::string>();
            steps.push_back(step_json("urlacl", "removed", detail));
        } catch (const std::exception& e) {
            fail("urlacl", e.what());
        }
    }
    if (wants(w, "firewall") && failed.empty()) {
        try {
            h.firewall.remove(w.value("firewall_rule", ""));
            steps.push_back(step_json("firewall", "removed", w.value("firewall_rule", "")));
        } catch (const std::exception& e) {
            fail("firewall", e.what());
        }
    }
    if (wants(w, "certificate") && failed.empty()) {
        try {
            const std::string t = w.value("cert_thumbprint", "");
            const CertInfo c = h.certs.find_by_thumbprint(t);
            if (c.found && c.friendly_name.rfind("fairyfly-mcp ", 0) != 0) {
                steps.push_back(step_json("certificate", "skipped", "certificate does not carry the fairyfly friendly name"));
            } else {
                h.certs.remove(t);
                steps.push_back(step_json("certificate", "removed", t));
            }
        } catch (const std::exception& e) {
            fail("certificate", e.what());
        }
    }
    json out{{"ok", failed.empty()}, {"steps", steps}};
    if (!failed.empty()) out["error"] = {{"code", "STEP_FAILED"}, {"message", "step " + failed + " failed"}};
    return out;
}

} // namespace

json execute_elevated_work(const json& work, Hosts& hosts) {
    if (auto bad = validate_work(work)) return json{{"ok", false}, {"steps", json::array()}, {"error", {{"code", "INVALID_PLAN"}, {"message", *bad}}}};
    try {
        return work["operation"] == "setup" ? execute_setup_work(work, hosts) : execute_teardown_work(work, hosts);
    } catch (const std::exception& e) {
        return json{{"ok", false}, {"steps", json::array()}, {"error", {{"code", "STEP_FAILED"}, {"message", e.what()}}}};
    }
}

namespace {

json invalid_plan(const std::string& why) {
    return json{{"ok", false}, {"steps", json::array()}, {"error", {{"code", "INVALID_PLAN"}, {"message", why}}}};
}

bool lower_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

json apply_checked(Hosts& hosts, const std::string& bytes, const ApplyArgs& args) {
    if (bytes.size() > kMaxPlanBytes) return invalid_plan("plan file too large");
    if (!lower_hex64(args.sha256) || args.nonce.empty()) return invalid_plan("the plan was not approved by the requesting process");
    // 1. the bytes are exactly what the unelevated parent wrote and put on the elevated command line
    if (!auth::constant_time_equal(auth::sha256_hex(bytes), args.sha256)) return invalid_plan("plan file does not match the approved SHA-256 (changed after approval)");
    // 2. parse the SAME bytes (no second read of the file)
    json plan;
    try {
        plan = json::parse(bytes);
    } catch (const std::exception&) {
        return invalid_plan("plan is not valid JSON");
    }
    if (!plan.is_object()) return invalid_plan("malformed plan");
    // 3. the plan belongs to this request and is fresh
    if (plan.value("nonce", std::string()) != args.nonce) return invalid_plan("plan nonce does not match the request");
    if (plan.value("parent_pid", 0LL) != args.parent_pid) return invalid_plan("plan parent pid does not match the request");
    const long long age = hosts.sys.now() - plan.value("created_at", 0LL);
    if (age > kPlanMaxAgeSeconds || age < -60) return invalid_plan("plan is stale");
    // 4. claims on the command line: the SID the parent resolved and the consent for --force-binding
    if (plan.value("sid", std::string()) != args.sid) return invalid_plan("plan SID does not match the approved SID");
    if (plan.value("force_binding", false) && !args.force_binding) return invalid_plan("force_binding was not approved by the requesting process");
    return execute_elevated_work(plan, hosts);
}

} // namespace

json apply_plan_bytes(Hosts& hosts, const std::string& plan_bytes, const ApplyArgs& args) {
    json result;
    try {
        result = apply_checked(hosts, plan_bytes, args);
    } catch (const std::exception& e) {
        result = invalid_plan(e.what());
    }
    result["nonce"] = args.nonce;
    return result;
}

int run_apply_plan(Hosts& hosts, const std::string& plan_file, const std::string& result_file, const ApplyArgs& args) {
    json result;
    const auto text = hosts.sys.read_file(plan_file);   // the one and only read
    result = text ? apply_plan_bytes(hosts, *text, args) : json{{"ok", false}, {"steps", json::array()}, {"nonce", args.nonce}, {"error", {{"code", "INVALID_PLAN"}, {"message", "plan file unreadable"}}}};
    hosts.sys.write_file(result_file, result.dump());
    return result.value("ok", false) ? 0 : 1;
}

// ---- output plumbing ---------------------------------------------------------------------------------------------
namespace {

int finish(const RunEnv& env, bool as_json, Report& r, int exit_code) {
    if (as_json) {
        *env.out << report_to_json(r).dump(2) << std::endl;
    } else if (r.error) {
        *env.err << "error " << r.error->code << ": " << r.error->message << std::endl;
    }
    return exit_code;
}

int finish_error(const RunEnv& env, bool as_json, const ErrorInfo& e, Report* with = nullptr) {
    Report r;
    if (with) r = *with;
    else r.has_data = false;
    r.error = e;
    return finish(env, as_json, r, e.exit_code);
}

std::string apply_status(const std::string& would) {
    if (would == "would_create") return "created";
    if (would == "would_update") return "updated";
    if (would == "would_remove") return "removed";
    return would;
}

/// Runs the elevated part in process or through the elevated child. Sets `error` and returns false on failure.
bool run_elevated_part(Hosts& h, const Plan& plan, json* result, ErrorInfo* error) {
    const json work = make_elevated_work(plan);
    if (work["steps"].empty()) {
        *result = json{{"ok", true}, {"steps", json::array()}, {"thumbprint", plan.thumbprint}};
        return true;
    }
    if (h.elevator.is_elevated()) {
        *result = execute_elevated_work(work, h);
        return true;
    }
    // Bind the plan to what this process approved: exact bytes + SHA-256 (on the elevated command line), nonce, pid, time.
    const std::string nonce = auth::random_hex(auth::random_bytes, 16);
    json bound = work;
    bound["nonce"] = nonce;
    bound["parent_pid"] = h.elevator.process_id();
    bound["created_at"] = h.sys.now();
    ElevatedRequest request;
    request.plan_text = bound.dump();
    request.plan_sha256 = auth::sha256_hex(request.plan_text);
    request.nonce = nonce;
    request.sid = work.value("sid", std::string());
    request.parent_pid = h.elevator.process_id();
    request.force_binding = work.value("force_binding", false);
    const ElevatedRun run = h.elevator.run_elevated(request);
    if (run.declined) {
        *error = {"ELEVATION_DECLINED", "The UAC prompt was declined; nothing was changed.", 1};
        return false;
    }
    if (!run.launched || run.result.is_null() || !run.result.is_object()) {
        *error = {"ELEVATION_FAILED", "The elevated process did not report a result" + (run.error.empty() ? "" : ": " + run.error) +
                                          " (exit code " + std::to_string(run.exit_code) + ")", 1};
        return false;
    }
    if (run.result.value("nonce", std::string()) != nonce) {
        *error = {"ELEVATION_FAILED", "The elevated process reported a result that does not belong to this request (exit code " + std::to_string(run.exit_code) + ")", 1};
        return false;
    }
    *result = run.result;
    return true;
}

void merge_results(std::vector<StepItem>& steps, const json& result, bool* any_failed, std::string* first_failed_detail) {
    for (const auto& s : result.value("steps", json::array())) {
        for (auto& step : steps) {
            if (step.id != s.value("id", "")) continue;
            const std::string status = s.value("status", "");
            step.status = status;
            const std::string detail = s.value("detail", "");
            if (!detail.empty()) step.detail = detail;
            if (status == "failed") {
                *any_failed = true;
                if (first_failed_detail->empty()) *first_failed_detail = step.id + ": " + detail;
            }
        }
    }
}

/// would_* -> created/updated/removed; after a failure the steps that never ran become `skipped`.
void finalize_statuses(std::vector<StepItem>& steps, bool failed) {
    for (auto& s : steps) {
        if (s.status.rfind("would", 0) != 0) continue;
        if (failed) {
            s.status = "skipped";
            s.detail = "not run after a failure";
        } else {
            s.status = apply_status(s.status);
        }
    }
}

bool old_protocol(const std::string& p) { return p == "TLS 1.0" || p == "TLS 1.1" || p.rfind("SSL", 0) == 0; }

} // namespace

// ---- run_setup ---------------------------------------------------------------------------------------------------
int run_setup(Hosts& h, Options o, const RunEnv& env) {
    fill_defaults(h, o);
    if (const auto e = validate_options(o)) return finish_error(env, o.json, *e);

    if (o.print_runbook) {
        RunbookContext ctx;
        ctx.user = !o.user.empty() ? o.user : h.elevator.current_user_name();
        ctx.thumbprint = o.thumbprint;
        ctx.cer_path = join_path(data_dir(h.sys), cer_file_name(o.hostname));
        Report r;
        r.has_data = true;
        r.runbook = make_runbook(o, ctx);
        r.hostname = o.hostname;
        if (o.json) *env.out << report_to_json(r).dump(2) << std::endl;
        else *env.out << r.runbook;
        return 0;
    }

    const Diagnosis d = diagnose(h, o);
    Plan plan = MakePlan(d, o);
    if (plan.error) return finish_error(env, o.json, *plan.error);
    Report rep = report_from_plan(plan, o.dry_run);

    if (o.dry_run) {
        if (o.json) *env.out << report_to_json(rep).dump(2) << std::endl;
        else *env.out << render_plan_text(plan, true);
        return 0;
    }
    if (plan.nothing) {
        rep.nothing = true;
        if (o.json) *env.out << report_to_json(rep).dump(2) << std::endl;
        else *env.out << nothing_message("setup") << "\n";
        return 0;
    }
    if (!o.json) *env.out << render_plan_text(plan, false);
    if (env.read_only)
        return finish_error(env, o.json, {"READ_ONLY", "FAIRYFLY_READ_ONLY is set: refusing to change the machine (dry runs and --print-runbook still work)", 2}, &rep);
    if (plan.blocked) {
        std::string why;
        for (const auto& s : plan.steps)
            if (s.status == "blocked") why += (why.empty() ? "" : "; ") + s.id + ": " + s.detail;
        return finish_error(env, o.json, {"PLAN_BLOCKED", "Setup cannot proceed: " + why, 1}, &rep);
    }

    // consent gate
    if (!o.yes) {
        if (o.non_interactive || !env.stdin_is_tty)
            return finish_error(env, o.json, {"CONFIRMATION_REQUIRED", "Refusing to change " + o.hostname + " without confirmation (pass --yes)", 2}, &rep);
        const bool ok = env.confirm && env.confirm("Change this machine (" + o.hostname + ")? [y/N] ");
        if (!ok) return finish_error(env, o.json, {"CONFIRMATION_REQUIRED", "Aborted; nothing was changed", 2}, &rep);
    }
    // elevation gate
    if (plan.needs_elevation && !h.elevator.is_elevated() && o.non_interactive) {
        const std::string cmd = o.command_line.empty() ? "fairyfly mcp setup ... --yes" : o.command_line;
        return finish_error(env, o.json, {"ELEVATION_REQUIRED", "This setup changes machine configuration and needs an elevated process. Run this from an elevated prompt: " + cmd, 2}, &rep);
    }

    // ---- apply ----
    std::vector<StepItem> steps = plan.steps;
    bool any_failed = false;
    std::string failure;
    ErrorInfo err;
    json result;
    if (!run_elevated_part(h, plan, &result, &err)) {
        rep.steps = steps;
        return finish_error(env, o.json, err, &rep);
    }
    merge_results(steps, result, &any_failed, &failure);
    std::string thumb = result.value("thumbprint", plan.thumbprint);
    if (thumb.empty()) thumb = plan.thumbprint;
    if (!result.value("ok", true) && !any_failed) {
        any_failed = true;
        failure = result.value("error", json::object()).value("message", "elevated step failed");
    }

    auto set_status = [&](const std::string& id, const std::string& status, const std::string& detail) {
        for (auto& s : steps)
            if (s.id == id) {
                s.status = status;
                if (!detail.empty()) s.detail = detail;
            }
    };
    auto pending = [&](const std::string& id) {
        for (const auto& s : steps)
            if (s.id == id) return s.status.rfind("would", 0) == 0;
        return false;
    };

    if (result.contains("bound_ipports") && result["bound_ipports"].is_array() && !result["bound_ipports"].empty())
        plan.manifest.ipports = result["bound_ipports"].get<std::vector<std::string>>();
    if (!any_failed && plan.tls) {
        if (pending("certificate_export")) {
            try {
                if (thumb.empty()) throw HostError("NO_CERTIFICATE", "no certificate to export");
                const std::string change = h.certs.export_cer(thumb, plan.cer_path, CerFormat::Der);
                set_status("certificate_export", change, plan.cer_path);
            } catch (const std::exception& e) {
                set_status("certificate_export", "failed", e.what());
                any_failed = true;
                failure = std::string("certificate_export: ") + e.what();
            }
        }
    }
    if (!any_failed && pending("config")) {
        if (h.sys.write_file(plan.config_path, plan.config_yaml)) {
            set_status("config", "created", plan.config_path);
            // record the hash of the bytes actually on disk (teardown compares against it)
            if (const auto written = h.sys.read_file(plan.config_path)) plan.manifest.config_sha256 = auth::sha256_hex(*written);
        } else {
            set_status("config", "failed", "cannot write " + plan.config_path);
            any_failed = true;
            failure = "config: cannot write " + plan.config_path;
        }
    }
    if (!any_failed && pending("manifest")) {
        Manifest m = plan.manifest;
        m.thumbprint = plan.tls && plan.cert_mode != CertMode::NoTls ? thumb : "";
        const std::string now = iso_utc(h.sys.now());
        if (m.created_at.empty()) m.created_at = now;
        m.updated_at = now;
        if (h.sys.write_file(plan.manifest_path, m.to_json().dump(2))) set_status("manifest", "created", plan.manifest_path);
        else {
            set_status("manifest", "failed", "cannot write " + plan.manifest_path);
            any_failed = true;
            failure = "manifest: cannot write " + plan.manifest_path;
        }
        for (auto& s : steps)
            if (s.id == "manifest" && s.status == "created" && plan.step("manifest")->status == "would_update") s.status = "updated";
    }
    finalize_statuses(steps, any_failed);
    if (any_failed) {
        rep.steps = steps;
        rep.error = ErrorInfo{"STEP_FAILED", failure, 1};
        if (!o.json) *env.out << render_result_text(rep);
        return finish(env, o.json, rep, 1);
    }
    rep.steps = steps;

    // ---- verify ----
    VerifyOut vo;
    {
        Options again = o;
        const Diagnosis after = diagnose(h, again);
        const Plan check = MakePlan(after, again);
        std::string missing;
        for (const auto& s : check.steps)
            if ((s.id == "urlacl" || s.id == "sslcert" || s.id == "certificate" || s.id == "certificate_export") &&
                (s.status.rfind("would", 0) == 0 || s.status == "blocked"))
                missing += (missing.empty() ? "" : ", ") + s.id;
        if (!missing.empty()) {
            vo = {"failed", "", 0, false, "after apply these are not in place: " + missing};
        } else {
            VerifyRequest req{plan.tls, plan.hostname, plan.port, plan.tls ? thumb : "", plan.sid};
            const VerifyResult vr = h.verify.round_trip(req);
            vo = {vr.status, vr.protocol, vr.http_status, vr.thumbprint_match, vr.detail};
            if (vr.status == "ok" && (vr.http_status != 401 || (plan.tls && !vr.thumbprint_match))) {
                vo.status = "failed";
                vo.detail = vr.http_status != 401 ? "expected HTTP 401 for an unauthenticated request, got " + std::to_string(vr.http_status)
                                                  : "the presented certificate is not the bound one";
            }
        }
    }
    rep.verify = vo;
    rep.human = plan.human;
    if (vo.status == "ok" && old_protocol(vo.protocol))
        rep.human.push_back({"schannel", "The server negotiated " + vo.protocol + ": raise the machine's Schannel policy to TLS 1.2 or newer (registry SCHANNEL\\Protocols)."});
    rep.next_steps = plan.next_steps;
    if (vo.status == "failed") {
        rep.error = ErrorInfo{"VERIFY_FAILED", "Setup applied, but verification failed: " + vo.detail, 1};
        if (!o.json) *env.out << render_result_text(rep);
        return finish(env, o.json, rep, 1);
    }
    if (!o.json) *env.out << render_result_text(rep) << "\nsetup complete: " << plan.url << "\n";
    else *env.out << report_to_json(rep).dump(2) << std::endl;
    return 0;
}

// ---- run_teardown --------------------------------------------------------------------------------------------------
int run_teardown(Hosts& h, TeardownOptions o, const RunEnv& env) {
    if (const auto e = validate_teardown_options(o)) return finish_error(env, o.json, *e);
    const TeardownDiagnosis d = diagnose_teardown(h, o);
    Plan plan = MakeTeardownPlan(d, o);
    if (plan.error) return finish_error(env, o.json, *plan.error);
    Report rep = report_from_plan(plan, o.dry_run);
    if (o.dry_run) {
        if (o.json) *env.out << report_to_json(rep).dump(2) << std::endl;
        else *env.out << render_plan_text(plan, true);
        return 0;
    }
    if (plan.nothing) {
        rep.nothing = true;
        if (o.json) *env.out << report_to_json(rep).dump(2) << std::endl;
        else *env.out << nothing_message("teardown") << "\n";
        return 0;
    }
    if (!o.json) *env.out << render_plan_text(plan, false);
    if (env.read_only)
        return finish_error(env, o.json, {"READ_ONLY", "FAIRYFLY_READ_ONLY is set: refusing to change the machine (use --dry-run to see the plan)", 2}, &rep);
    const std::string label = d.hostname.empty() ? "this machine" : d.hostname;
    if (!o.yes) {
        if (o.non_interactive || !env.stdin_is_tty)
            return finish_error(env, o.json, {"CONFIRMATION_REQUIRED", "Refusing to change " + label + " without confirmation (pass --yes)", 2}, &rep);
        const bool ok = env.confirm && env.confirm("Change this machine (" + label + ")? [y/N] ");
        if (!ok) return finish_error(env, o.json, {"CONFIRMATION_REQUIRED", "Aborted; nothing was changed", 2}, &rep);
    }
    if (plan.needs_elevation && !h.elevator.is_elevated() && o.non_interactive) {
        const std::string cmd = o.command_line.empty() ? "fairyfly mcp teardown --yes" : o.command_line;
        return finish_error(env, o.json, {"ELEVATION_REQUIRED", "The teardown needs an elevated process. Run this from an elevated prompt: " + cmd, 2}, &rep);
    }

    std::vector<StepItem> steps = plan.steps;
    ErrorInfo err;
    json result;
    if (!run_elevated_part(h, plan, &result, &err)) {
        rep.steps = steps;
        return finish_error(env, o.json, err, &rep);
    }
    bool any_failed = false;
    std::string failure;
    merge_results(steps, result, &any_failed, &failure);
    if (!result.value("ok", true) && !any_failed) {
        any_failed = true;
        failure = result.value("error", json::object()).value("message", "elevated step failed");
    }
    auto set_status = [&](const std::string& id, const std::string& status, const std::string& detail) {
        for (auto& s : steps)
            if (s.id == id) {
                s.status = status;
                if (!detail.empty()) s.detail = detail;
            }
    };
    auto pending = [&](const std::string& id) {
        for (const auto& s : steps)
            if (s.id == id) return s.status == "would_remove";
        return false;
    };
    if (!any_failed && pending("certificate_export")) {
        if (h.sys.remove_file(plan.teardown.cer_path) || !h.sys.file_exists(plan.teardown.cer_path)) set_status("certificate_export", "removed", plan.teardown.cer_path);
        else {
            set_status("certificate_export", "failed", "cannot delete " + plan.teardown.cer_path);
            any_failed = true;
            failure = "certificate_export: cannot delete the file";
        }
    }
    if (!any_failed && pending("config")) {
        if (h.sys.remove_file(plan.teardown.config_path) || !h.sys.file_exists(plan.teardown.config_path)) set_status("config", "removed", plan.teardown.config_path);
        else {
            set_status("config", "failed", "cannot delete " + plan.teardown.config_path);
            any_failed = true;
            failure = "config: cannot delete the file";
        }
    }
    if (!any_failed && pending("manifest")) {
        if (h.sys.remove_file(plan.manifest_path) || !h.sys.file_exists(plan.manifest_path)) set_status("manifest", "removed", plan.manifest_path);
        else {
            set_status("manifest", "failed", "cannot delete " + plan.manifest_path);
            any_failed = true;
            failure = "manifest: cannot delete the file";
        }
    }
    finalize_statuses(steps, any_failed);
    rep.steps = steps;
    rep.human = plan.human;
    if (any_failed) {
        rep.error = ErrorInfo{"STEP_FAILED", failure, 1};
        if (!o.json) *env.out << render_result_text(rep);
        return finish(env, o.json, rep, 1);
    }
    // verify: nothing of ours is left
    const TeardownDiagnosis after = diagnose_teardown(h, o);
    const Plan check = MakeTeardownPlan(after, o);
    if (!check.error && !check.nothing) {
        std::string left;
        for (const auto& s : check.steps)
            if (s.status == "would_remove") left += (left.empty() ? "" : ", ") + s.id;
        rep.error = ErrorInfo{"TEARDOWN_INCOMPLETE", "These are still in place: " + left, 1};
        if (!o.json) *env.out << render_result_text(rep);
        return finish(env, o.json, rep, 1);
    }
    if (!o.json) *env.out << render_result_text(rep) << "\nteardown complete.\n";
    else *env.out << report_to_json(rep).dump(2) << std::endl;
    return 0;
}

// ---- cert export ----------------------------------------------------------------------------------------------------
int run_cert_export(Hosts& h, CertExportOptions o, const RunEnv& env) {
    std::string thumb;
    std::string host = o.hostname;
    int port = o.port;
    bool unreadable = false;
    const auto manifest = load_manifest(h.sys, manifest_path_for(h.sys), &unreadable);
    if (manifest && manifest->mode == "tls") {
        thumb = manifest->thumbprint;
        if (host.empty()) host = manifest->hostname;
        if (port == 0) port = manifest->port;
    }
    if (host.empty()) host = default_hostname(h.sys.computer_dns_name());
    if (thumb.empty()) {
        try {
            if (port == 0) port = kDefaultTlsPort;
            if (const auto b = h.http.query_sslcert("0.0.0.0:" + std::to_string(port))) thumb = b->thumbprint;
        } catch (const HostError&) {
        }
    }
    if (thumb.empty())
        return finish_error(env, o.json, {"CERT_NOT_FOUND", "No certificate is set up (no manifest, no TLS binding on port " + std::to_string(port ? port : kDefaultTlsPort) + "). Run 'fairyfly mcp setup' first.", 1});
    if (!h.certs.find_by_thumbprint(thumb).found)
        return finish_error(env, o.json, {"CERT_NOT_FOUND", "Certificate " + thumb + " is not in LocalMachine\\My", 1});
    const std::string ext = o.format == CerFormat::Pem ? ".pem" : ".cer";
    std::string path = o.out_path;
    if (path.empty()) {
        path = join_path(data_dir(h.sys), cer_file_name(host));
        if (o.format == CerFormat::Pem) path = path.substr(0, path.size() - 4) + ext;
    }
    std::string change;
    try {
        change = h.certs.export_cer(thumb, path, o.format);
    } catch (const HostError& e) {
        return finish_error(env, o.json, host_error_info(e));
    } catch (const std::exception& e) {
        return finish_error(env, o.json, {"EXPORT_FAILED", e.what(), 1});
    }
    const std::string hints = trust_hints(path, host);
    if (o.json) {
        json data{{"path", path}, {"format", o.format == CerFormat::Pem ? "pem" : "der"}, {"thumbprint", thumb}, {"change", change}, {"trust_hints", json::array()}};
        std::istringstream in(hints);
        for (std::string line; std::getline(in, line);) data["trust_hints"].push_back(line);
        *env.out << json{{"status", "success"}, {"data", data}}.dump(2) << std::endl;
    } else {
        *env.out << "certificate " << change << ": " << path << "\nthumbprint: " << thumb << "\n\nTrust it on every client:\n" << hints << "\n";
    }
    return 0;
}

} // namespace fairyfly::setup
