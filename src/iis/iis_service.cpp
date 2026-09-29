#include "include/iis/iis_service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "include/iis/validators.h"

namespace fairyfly::iis {

using nlohmann::json;

const char* change_name(Change change) {
    switch (change) {
        case Change::Created: return "created";
        case Change::Updated: return "updated";
        case Change::Unchanged: return "unchanged";
    }
    return "unchanged";
}

namespace {

constexpr const char* kProxyUser = "proxy";
constexpr int kSelfSignedDays = 730;

std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string join_path(const std::string& dir, const std::string& leaf) { return dir + "\\" + leaf; }

std::string parent_dir(const std::string& path) {
    const auto pos = path.find_last_of('\\');
    return pos == std::string::npos ? path : path.substr(0, pos);
}

std::string cer_path_for(const std::string& site_path, const std::string& site_name, const std::string& hostname) {
    return join_path(parent_dir(site_path), site_name + "-" + hostname + ".cer");
}

std::string iso_utc(int64_t secs) {
    // Howard Hinnant's civil_from_days.
    int64_t z = secs / 86400 + 719468;
    if (secs < 0 && secs % 86400 != 0) --z;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = yoe + era * 400 + (m <= 2 ? 1 : 0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT00:00:00Z", static_cast<long long>(y),
                  static_cast<long long>(m), static_cast<long long>(d));
    return buf;
}

int days_left(int64_t not_after, int64_t now) {
    const int64_t diff = not_after - now;
    return static_cast<int>(diff >= 0 ? diff / 86400 : -((-diff + 86399) / 86400));
}

bool host_matches(const std::string& pattern_in, const std::string& host_in) {
    const std::string pattern = to_lower(pattern_in), host = to_lower(host_in);
    if (pattern == host) return true;
    if (pattern.size() > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const auto dot = host.find('.');
        return dot != std::string::npos && host.substr(dot) == pattern.substr(1);
    }
    return false;
}

bool certificate_matches(const CertInfo& cert, const std::string& host) {
    return std::any_of(cert.dns_names.begin(), cert.dns_names.end(),
                       [&](const std::string& n) { return host_matches(n, host); });
}

Report failure(const std::string& code, const std::string& message, json details = json::object()) {
    Report r;
    r.ok = false;
    r.error_code = code;
    r.error_message = message;
    r.data = std::move(details);
    return r;
}

json make_step(const std::string& name, const std::string& status, const std::string& detail) {
    return json{{"step", name}, {"status", status}, {"detail", detail}};
}

std::string status_for(Change change, bool dry_run) {
    if (!dry_run) return change_name(change);
    switch (change) {
        case Change::Created: return "would_create";
        case Change::Updated: return "would_update";
        case Change::Unchanged: return "unchanged";
    }
    return "unchanged";
}

bool site_matches(const SiteInfo& site, const SiteSpec& spec) {
    if (!site.exists || site.physical_path != spec.physical_path || site.app_pool != spec.app_pool) return false;
    return std::any_of(site.bindings.begin(), site.bindings.end(), [&](const BindingInfo& b) {
        return b.protocol == "https" && to_lower(b.host) == to_lower(spec.hostname) && b.port == spec.port &&
               (spec.thumbprint.empty() || b.thumbprint == spec.thumbprint);
    });
}

WebConfigParams web_params(const SetupOptions& o, const std::string& secret) {
    WebConfigParams p;
    p.upstream = o.upstream;
    p.proxy_secret = secret;
    p.allow_ips = o.allow_ips;
    p.allow_any_ip = o.allow_any_ip;
    return p;
}

json manifest_json(const SetupOptions& o, const std::string& thumbprint, const std::string& cer_path) {
    WebConfigParams masked = web_params(o, kSecretPlaceholder);
    return json{{"schema", 1},
                {"created_by", "fairyfly mcp iis setup"},
                {"site_name", o.site_name},
                {"site_path", o.site_path},
                {"hostname", o.hostname},
                {"port", o.port},
                {"upstream", o.upstream},
                {"allow_ips", o.allow_ips},
                {"allow_any_ip", o.allow_any_ip},
                {"cert_mode", o.self_signed ? "self-signed" : "existing"},
                {"cert_thumbprint", thumbprint},
                {"cer_path", o.self_signed ? cer_path : ""},
                {"firewall_rule", o.open_firewall ? firewall_rule_name(o.port) : ""},
                {"web_config_hash", content_hash(generate_web_config(masked))}};
}

std::optional<json> read_manifest(IisHost& host, const std::string& site_path) {
    const auto text = host.read_file(manifest_path(site_path));
    if (!text) return std::nullopt;
    const json parsed = json::parse(*text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    return std::optional<json>(std::in_place, parsed);
}

std::string proxy_secret_or_empty(cred::CredentialStore& store) {
    try {
        auto cred = store.read(kProxySecretCredential);
        if (cred) return cred->password.utf8();
    } catch (const cred::CredentialError&) {}
    return {};
}

} // namespace

std::string firewall_rule_name(int port) { return "fairyfly MCP HTTPS " + std::to_string(port); }
std::string manifest_path(const std::string& site_path) { return join_path(site_path, kManifestFile); }
std::string web_config_path(const std::string& site_path) { return join_path(site_path, "web.config"); }

json build_checklist(const HostFacts& f, bool needs_ip_security) {
    json list = json::array();
    auto add = [&](const std::string& id, const std::string& label, bool ok, const std::string& detail, const std::string& fix) {
        list.push_back(json{{"id", id}, {"label", label}, {"ok", ok}, {"detail", detail}, {"fix", ok ? "" : fix}});
    };
    add("admin_rights", "Administrator rights", f.elevated,
        f.elevated ? "running elevated" : "not running elevated",
        "Start PowerShell with 'Run as administrator' and repeat the command.");
    add("iis_role", "IIS web server role", f.iis_installed,
        f.iis_installed ? "IIS is installed" : "IIS (W3SVC) was not found",
        "Windows client: Enable-WindowsOptionalFeature -Online -FeatureName IIS-WebServerRole,IIS-ManagementScriptingTools -All"
        " | Windows Server: Install-WindowsFeature Web-Server,Web-Scripting-Tools"
        " | DISM: dism /online /enable-feature /featurename:IIS-WebServerRole /all");
    add("iis_powershell_module", "IIS PowerShell module (WebAdministration or IISAdministration)", !f.admin_module.empty(),
        f.admin_module.empty() ? "neither module found" : f.admin_module + " is available",
        "Install the IIS management scripts: Enable-WindowsOptionalFeature -Online -FeatureName IIS-ManagementScriptingTools -All"
        " (Server: Install-WindowsFeature Web-Scripting-Tools)");
    add("url_rewrite", "IIS URL Rewrite module", f.url_rewrite,
        f.url_rewrite ? "URL Rewrite is installed" : "URL Rewrite module not found",
        "winget search \"URL Rewrite\" (Microsoft IIS URL Rewrite) | choco install urlrewrite -y"
        " | https://www.iis.net/downloads/microsoft/url-rewrite");
    add("arr_installed", "Application Request Routing (ARR) 3.0", f.arr_installed,
        f.arr_installed ? "ARR is installed" : "ARR not found",
        "choco install iis-arr -y | https://www.iis.net/downloads/microsoft/application-request-routing"
        " (requires URL Rewrite first)");
    add("arr_proxy_enabled", "ARR proxy enabled (server level)", f.arr_proxy_enabled,
        f.arr_proxy_enabled ? "proxy is enabled" : "ARR proxy is disabled",
        "IIS Manager > server node > Application Request Routing Cache > Server Proxy Settings > Enable proxy, or:"
        " Set-WebConfigurationProperty -PSPath 'MACHINE/WEBROOT/APPHOST' -Filter 'system.webServer/proxy' -Name enabled -Value $true");
    if (needs_ip_security) {
        add("ip_security", "IIS IP and Domain Restrictions feature", f.ip_security,
            f.ip_security ? "IP Security is installed" : "IP Security feature not found",
            "Enable-WindowsOptionalFeature -Online -FeatureName IIS-IPSecurity -All"
            " | Server: Install-WindowsFeature Web-IP-Security");
    }
    return list;
}

std::vector<std::string> missing_ids(const json& checklist) {
    std::vector<std::string> out;
    for (const auto& item : checklist) {
        if (!item.value("ok", false)) out.push_back(item.value("id", ""));
    }
    return out;
}

std::optional<OptionProblem> validate_site(const std::string& site_name, const std::string& site_path) {
    if (const auto e = site_name_error(site_name)) return OptionProblem{"site-name", *e};
    if (const auto e = site_path_error(site_path)) return OptionProblem{"site-path", *e};
    return std::nullopt;
}

std::optional<OptionProblem> validate(const SetupOptions& o) {
    if (o.hostname.empty()) return OptionProblem{"hostname", "--hostname is required"};
    if (const auto e = hostname_error(o.hostname)) return OptionProblem{"hostname", *e};
    if (const auto e = port_error(o.port)) return OptionProblem{"port", *e};
    if (const auto e = upstream_error(o.upstream)) return OptionProblem{"upstream", *e};
    if (const auto up = parse_upstream(o.upstream); up && up->port == o.port)
        return OptionProblem{"port", "the https port must differ from the fairyfly upstream port"};
    if (const auto p = validate_site(o.site_name, o.site_path)) return p;
    if (o.self_signed && !o.cert_thumbprint.empty())
        return OptionProblem{"cert-thumbprint", "--cert-thumbprint and --self-signed are mutually exclusive"};
    if (!o.self_signed && o.cert_thumbprint.empty())
        return OptionProblem{"cert-thumbprint", "give --cert-thumbprint (existing LocalMachine\\My certificate) or --self-signed"};
    if (!o.cert_thumbprint.empty()) {
        if (const auto e = thumbprint_error(o.cert_thumbprint)) return OptionProblem{"cert-thumbprint", *e};
    }
    if (o.allow_any_ip && !o.allow_ips.empty())
        return OptionProblem{"allow-ip", "--allow-any-ip and --allow-ip are mutually exclusive"};
    if (!o.allow_any_ip && o.allow_ips.empty())
        return OptionProblem{"allow-ip", "--allow-ip CIDR[,CIDR...] is required (or --allow-any-ip to disable the IIS IP restriction, not recommended)"};
    for (const auto& ip : o.allow_ips) {
        if (const auto e = cidr_error(ip)) return OptionProblem{"allow-ip", "'" + ip + "': " + *e};
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// setup

Report run_setup(IisContext& ctx, const SetupOptions& input) {
    SetupOptions o = input;
    if (const auto problem = validate(o)) {
        return failure("INVALID_ARGUMENT", problem->message, json{{"field", problem->field}});
    }
    if (!o.cert_thumbprint.empty()) o.cert_thumbprint = normalize_thumbprint(o.cert_thumbprint);

    const bool dry = o.dry_run;
    IisHost& host = ctx.host;
    const HostFacts facts = host.detect();
    const json checklist = build_checklist(facts, !o.allow_any_ip);
    const auto missing = missing_ids(checklist);

    if (!dry && !missing.empty()) {
        return failure("IIS_PREREQUISITE_MISSING",
                       "IIS prerequisites are missing: " + [&] {
                           std::string s;
                           for (const auto& m : missing) s += (s.empty() ? "" : ", ") + m;
                           return s;
                       }() + ". Nothing was changed. See the fix hints; fairyfly never installs software itself.",
                       json{{"missing", missing}, {"prerequisites", checklist}});
    }
    if (!dry && !o.yes) {
        return failure("CONFIRMATION_REQUIRED",
                       "setup changes IIS, the certificate store and file permissions on this machine; "
                       "review with --dry-run, then re-run with --yes",
                       json{{"prerequisites", checklist}});
    }

    // Without IIS/WebAdministration the read-only probes cannot run; a dry run then plans against an empty machine.
    const bool can_probe = facts.iis_installed && !facts.admin_module.empty();
    json steps = json::array();
    json warnings = json::array();
    std::string shown_secret;  // only set when a secret was generated in this run
    const std::string app_pool = o.site_name;
    const std::string wc_path = web_config_path(o.site_path);
    const std::string cer_path = cer_path_for(o.site_path, o.site_name, o.hostname);
    std::string thumbprint = o.cert_thumbprint;
    std::string secret = kSecretPlaceholder;
    const int64_t now = ctx.now();

    auto rollback_note = [&](const std::string& what) {
        json created = json::array();
        for (const auto& s : steps) {
            if (s.value("status", "") == "created" || s.value("status", "") == "updated") created.push_back(s.value("step", ""));
        }
        return json{{"failed_step", what},
                    {"applied_steps", created},
                    {"rollback", "Applied steps stay in place and setup is idempotent: fix the cause and run setup again. "
                                 "To undo everything run: fairyfly mcp iis remove --site-name " + o.site_name + " --yes"}};
    };
    std::string current_step = "preflight";

    try {
        // 1. certificate (validated before anything is changed)
        current_step = "certificate";
        if (!o.self_signed) {
            const auto cert = host.find_certificate(o.cert_thumbprint);
            if (!cert) {
                return failure("CERT_NOT_FOUND", "certificate " + o.cert_thumbprint + " was not found in LocalMachine\\My",
                               json{{"thumbprint", o.cert_thumbprint}});
            }
            std::string reason;
            if (!cert->has_private_key) reason = "the certificate has no private key";
            else if (cert->not_after <= now) reason = "the certificate expired on " + iso_utc(cert->not_after);
            else if (!certificate_matches(*cert, o.hostname)) reason = "the certificate does not cover host name " + o.hostname;
            if (!reason.empty()) return failure("CERT_INVALID", reason, json{{"thumbprint", o.cert_thumbprint}});
            const int left = days_left(cert->not_after, now);
            if (left < kCertWarnDays) warnings.push_back("certificate expires in " + std::to_string(left) + " days");
            steps.push_back(make_step("certificate", "unchanged",
                                      "existing certificate " + thumbprint + ", valid until " + iso_utc(cert->not_after)));
        } else {
            auto existing = host.find_self_signed(o.hostname);
            const bool reusable = existing && existing->has_private_key && days_left(existing->not_after, now) >= kCertWarnDays;
            if (reusable) {
                thumbprint = existing->thumbprint;
                steps.push_back(make_step("certificate", "unchanged", "reusing self-signed certificate " + thumbprint));
            } else if (dry) {
                thumbprint.clear();
                steps.push_back(make_step("certificate", "would_create", "new self-signed certificate for " + o.hostname));
            } else {
                thumbprint = host.create_self_signed(o.hostname, kSelfSignedDays).thumbprint;
                steps.push_back(make_step("certificate", "created", "self-signed certificate " + thumbprint + " for " + o.hostname));
            }
            warnings.push_back("self-signed certificate: every MCP client must trust " + cer_path +
                               " (import it into the client's trust store) or TLS verification will fail");
        }

        // 2. proxy secret
        current_step = "proxy_secret";
        {
            const auto existing = ctx.secrets.read(kProxySecretCredential);
            if (existing && !o.rotate_secret) {
                secret = existing->password.utf8();
                steps.push_back(make_step("proxy_secret", "unchanged", "kept the stored secret (Credential Manager 'fairyfly:" +
                                                                           std::string(kProxySecretCredential) + "')"));
            } else if (dry) {
                steps.push_back(make_step("proxy_secret", existing ? "would_update" : "would_create",
                                          existing ? "would rotate the proxy secret" : "would generate the proxy secret"));
            } else {
                secret = ctx.generate_secret();
                cred::CredentialSummary meta;
                meta.connection = kProxySecretCredential;
                meta.username = kProxyUser;
                ctx.secrets.write(kProxySecretCredential, meta, cred::SecretBuffer(std::string(secret)));
                shown_secret = secret;
                steps.push_back(make_step("proxy_secret", existing ? "updated" : "created",
                                          existing ? "rotated the proxy secret" : "generated the proxy secret (shown once below)"));
            }
        }

        // 3. site directory
        current_step = "site_directory";
        {
            const Change c = host.dir_exists(o.site_path) ? Change::Unchanged : Change::Created;
            steps.push_back(make_step("site_directory", dry ? status_for(c, true) : change_name(c == Change::Unchanged ? c : host.create_dir(o.site_path)), o.site_path));
        }

        // 4. application pool
        current_step = "app_pool";
        {
            const AppPoolInfo pool = can_probe ? host.get_app_pool(app_pool) : AppPoolInfo{};
            const bool ok = pool.exists && pool.runtime_version.empty() && pool.identity == "ApplicationPoolIdentity";
            const Change probe = !pool.exists ? Change::Created : (ok ? Change::Unchanged : Change::Updated);
            const Change c = dry ? probe : host.ensure_app_pool(app_pool);
            steps.push_back(make_step("app_pool", status_for(c, dry), app_pool + " (No Managed Code, ApplicationPoolIdentity)"));
        }

        // 5. site + https binding
        current_step = "site";
        {
            SiteSpec spec{o.site_name, o.site_path, app_pool, o.hostname, o.port, thumbprint};
            const SiteInfo site = can_probe ? host.get_site(o.site_name) : SiteInfo{};
            const Change probe = !site.exists ? Change::Created : (site_matches(site, spec) ? Change::Unchanged : Change::Updated);
            const Change c = dry ? probe : host.ensure_site(spec);
            steps.push_back(make_step("site", status_for(c, dry),
                                      o.site_name + " https://" + o.hostname + ":" + std::to_string(o.port) + " -> " + o.site_path));
        }

        // 6. allowedServerVariables + ipSecurity unlock (needed for the rewrite rules and IP restriction in web.config)
        current_step = "site_config_access";
        if (dry) {
            steps.push_back(make_step("site_config_access", "would_apply",
                                      "allow the rewrite server variables for the site and unlock ipSecurity"));
        } else {
            const Change c = host.ensure_site_config_access(o.site_name, required_server_variables());
            steps.push_back(make_step("site_config_access", change_name(c), "allowedServerVariables and ipSecurity override"));
        }

        // 7. web.config
        current_step = "web_config";
        {
            const std::string content = generate_web_config(web_params(o, secret));
            const auto current = host.read_file(wc_path);
            const Change probe = !current ? Change::Created : (*current == content ? Change::Unchanged : Change::Updated);
            const bool placeholder_only = dry && secret == kSecretPlaceholder;
            Change c = probe;
            if (placeholder_only && current) c = Change::Updated;  // the secret will differ; do not claim "unchanged"
            if (!dry) c = host.write_file(wc_path, content);
            steps.push_back(make_step("web_config", status_for(c, dry), wc_path));
        }

        // 8. ACL on web.config
        current_step = "web_config_acl";
        if (dry) {
            steps.push_back(make_step("web_config_acl", "would_apply", "web.config readable by IIS AppPool\\" + app_pool + ", Administrators and SYSTEM only"));
        } else {
            const Change c = host.restrict_acl(wc_path, app_pool);
            steps.push_back(make_step("web_config_acl", change_name(c), "IIS AppPool\\" + app_pool + ", Administrators, SYSTEM"));
        }

        // 9. manifest (non-secret record used by status/remove)
        current_step = "manifest";
        {
            const std::string text = manifest_json(o, thumbprint, cer_path).dump(2) + "\n";
            const auto current = host.read_file(manifest_path(o.site_path));
            const Change probe = !current ? Change::Created : (*current == text ? Change::Unchanged : Change::Updated);
            const Change c = dry ? probe : host.write_file(manifest_path(o.site_path), text);
            steps.push_back(make_step("manifest", status_for(c, dry), manifest_path(o.site_path)));
        }

        // 10. exported .cer for self-signed certificates
        if (o.self_signed) {
            current_step = "certificate_export";
            if (dry) {
                steps.push_back(make_step("certificate_export", host.read_file(cer_path) ? "unchanged" : "would_create", cer_path));
            } else {
                const Change c = host.export_certificate(thumbprint, cer_path);
                steps.push_back(make_step("certificate_export", change_name(c), cer_path));
            }
        }

        // 11. firewall (only on request)
        if (o.open_firewall) {
            current_step = "firewall";
            const std::string rule = firewall_rule_name(o.port);
            if (dry) {
                steps.push_back(make_step("firewall", host.firewall_rule_exists(rule) ? "unchanged" : "would_create", rule));
            } else {
                steps.push_back(make_step("firewall", change_name(host.ensure_firewall_rule(rule, o.port)), rule));
            }
        } else {
            steps.push_back(make_step("firewall", "skipped", "no rule added (use --open-firewall); reachable only if the port is already open"));
        }
    } catch (const HostError& e) {
        json details = rollback_note(current_step);
        details["steps"] = steps;
        details["host_error"] = e.code;
        return failure("IIS_SETUP_FAILED", "step '" + current_step + "' failed: " + e.message, details);
    } catch (const cred::CredentialError& e) {
        json details = rollback_note(current_step);
        details["steps"] = steps;
        return failure("IIS_SETUP_FAILED", std::string("credential store: ") + e.what(), details);
    } catch (const std::invalid_argument& e) {
        json details = rollback_note(current_step);
        details["steps"] = steps;
        return failure("IIS_SETUP_FAILED", e.what(), details);
    }

    if (o.allow_any_ip) {
        warnings.push_back("WARNING: --allow-any-ip is set. The IIS IP restriction (the second factor in front of the bearer tokens) is DISABLED.");
    }
    if (const auto up = parse_upstream(o.upstream); up && !host.tcp_reachable(up->host == "localhost" ? "127.0.0.1" : up->host, up->port, 1000)) {
        warnings.push_back("nothing listens on " + o.upstream + " yet: start it with `fairyfly mcp --http`");
    }

    Report report;
    report.data = json{{"operation", "setup"},
                       {"dry_run", dry},
                       {"site_name", o.site_name},
                       {"hostname", o.hostname},
                       {"url", "https://" + o.hostname + ":" + std::to_string(o.port) + kMcpPath},
                       {"prerequisites", checklist},
                       {"prerequisites_ok", missing.empty()},
                       {"steps", steps},
                       {"warnings", warnings},
                       {"next_steps",
                        json::array({"Start the server: fairyfly mcp --http (listening on " + o.upstream + ")",
                                     "Create a bearer token: fairyfly mcp token create <name>",
                                     "Verify: curl.exe -i https://" + o.hostname + ":" + std::to_string(o.port) + kMcpPath +
                                         " (expect 401 without a token)",
                                     "IIS Windows/Basic authentication is intentionally NOT used: fairyfly bearer tokens authenticate; "
                                     "the IIS IP restriction is the second factor."})}};
    if (!dry && !missing.empty()) report.data["prerequisites_ok"] = false;
    if (dry && !missing.empty()) {
        report.data["warnings"].push_back("dry run: prerequisites are missing (" + std::to_string(missing.size()) +
                                          "); a real run would stop with IIS_PREREQUISITE_MISSING");
    }
    if (!shown_secret.empty()) {
        report.data["proxy_secret"] = shown_secret;
        report.data["proxy_secret_notice"] =
            "Shown once. It is stored in Credential Manager as 'fairyfly:" + std::string(kProxySecretCredential) +
            "' and injected into web.config; it is never printed again.";
    }
    return report;
}

// ---------------------------------------------------------------------------------------------
// status

Report run_status(IisContext& ctx, const StatusOptions& o) {
    if (const auto p = validate_site(o.site_name, o.site_path)) {
        return failure("INVALID_ARGUMENT", p->message, json{{"field", p->field}});
    }
    IisHost& host = ctx.host;
    const int64_t now = ctx.now();
    json checks = json::array();
    auto add = [&](const std::string& id, const std::string& status, const std::string& detail, const std::string& fix = "") {
        json c{{"check", id}, {"status", status}, {"detail", detail}};
        if (!fix.empty()) c["fix"] = fix;
        checks.push_back(c);
    };

    const HostFacts facts = host.detect();
    const auto manifest = read_manifest(host, o.site_path);
    const bool needs_ip = !manifest || !manifest->value("allow_any_ip", false);
    const json checklist = build_checklist(facts, needs_ip);
    {
        std::vector<std::string> missing;
        for (const auto& item : checklist) {
            if (!item.value("ok", false) && item.value("id", "") != "admin_rights") missing.push_back(item.value("id", ""));
        }
        std::string list;
        for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
        add("prerequisites", missing.empty() ? "ok" : "fail", missing.empty() ? "IIS, URL Rewrite, ARR and proxy are available" : "missing: " + list,
            missing.empty() ? "" : "run `fairyfly mcp iis setup --dry-run` for per-item fix hints");
        add("elevation", facts.elevated ? "ok" : "info", facts.elevated ? "elevated" : "not elevated (setup/remove need an elevated shell)");
    }

    const bool can_probe = facts.iis_installed && !facts.admin_module.empty();
    const SiteInfo site = can_probe ? host.get_site(o.site_name) : SiteInfo{};
    std::string thumbprint = manifest ? manifest->value("cert_thumbprint", "") : "";
    std::string hostname = manifest ? manifest->value("hostname", "") : "";
    int port = manifest ? manifest->value("port", 0) : 0;
    if (!site.exists) {
        add("site", "fail", "site '" + o.site_name + "' does not exist", "fairyfly mcp iis setup ...");
    } else if (site.state != "Started") {
        add("site", "fail", "site is " + site.state, "start it in IIS Manager or: Start-Website -Name " + o.site_name);
    } else {
        add("site", "ok", "site is Started");
    }
    if (site.exists) {
        const AppPoolInfo pool = can_probe ? host.get_app_pool(o.site_name) : AppPoolInfo{};
        if (!pool.exists) add("app_pool", "fail", "application pool missing");
        else if (!pool.runtime_version.empty()) add("app_pool", "warn", "pool runs managed code (" + pool.runtime_version + "); expected No Managed Code");
        else add("app_pool", "ok", "No Managed Code, " + pool.identity);

        const auto binding = std::find_if(site.bindings.begin(), site.bindings.end(), [&](const BindingInfo& b) {
            return b.protocol == "https" && (hostname.empty() || to_lower(b.host) == to_lower(hostname)) && (port == 0 || b.port == port);
        });
        if (binding == site.bindings.end()) {
            add("binding", "fail", "no matching https binding", "fairyfly mcp iis setup ...");
        } else {
            add("binding", "ok", "https " + binding->host + ":" + std::to_string(binding->port));
            if (!binding->thumbprint.empty()) thumbprint = binding->thumbprint;
            if (hostname.empty()) hostname = binding->host;
            if (port == 0) port = binding->port;
        }
    }

    if (thumbprint.empty()) {
        add("certificate", site.exists ? "fail" : "info", "no certificate bound");
    } else if (const auto cert = host.find_certificate(thumbprint)) {
        const int left = days_left(cert->not_after, now);
        if (left < 0) add("certificate", "fail", "certificate expired on " + iso_utc(cert->not_after), "renew the certificate and re-run setup --cert-thumbprint");
        else if (left < kCertWarnDays) add("certificate", "warn", "certificate expires in " + std::to_string(left) + " days (" + iso_utc(cert->not_after) + ")", "renew it and re-run setup");
        else add("certificate", "ok", "valid for " + std::to_string(left) + " more days (until " + iso_utc(cert->not_after) + ")");
    } else {
        add("certificate", "fail", "certificate " + thumbprint + " is not in LocalMachine\\My");
    }

    const std::string secret = proxy_secret_or_empty(ctx.secrets);
    add("proxy_secret", secret.empty() ? "fail" : "ok",
        secret.empty() ? "no proxy secret stored" : "stored in Credential Manager (value not shown)",
        secret.empty() ? "fairyfly mcp iis setup ... (generates one)" : "");

    json drift = json::object();
    const auto actual = host.read_file(web_config_path(o.site_path));
    if (!actual) {
        add("web_config", "fail", "web.config missing in " + o.site_path, "fairyfly mcp iis setup ...");
    } else if (!manifest || secret.empty()) {
        add("web_config", "warn", "present, but drift cannot be checked (" + std::string(manifest ? "secret missing" : "manifest missing") + ")");
    } else {
        try {
            WebConfigParams p;
            p.upstream = manifest->value("upstream", "");
            p.proxy_secret = secret;
            p.allow_any_ip = manifest->value("allow_any_ip", false);
            for (const auto& ip : manifest->value("allow_ips", json::array())) p.allow_ips.push_back(ip.get<std::string>());
            const std::string expected = content_hash(mask_secret(generate_web_config(p), secret));
            const std::string have = content_hash(mask_secret(*actual, secret));
            drift = json{{"expected_hash", expected}, {"actual_hash", have}, {"drift", expected != have}};
            if (expected == have) add("web_config", "ok", "matches the generated content (hash " + have + ")");
            else add("web_config", "warn", "drift: web.config differs from the generated content (expected " + expected + ", found " + have + ")",
                     "fairyfly mcp iis setup ... (rewrites it), or revert the manual edit");
        } catch (const std::exception& e) {
            add("web_config", "warn", std::string("manifest is unusable for a drift check: ") + e.what());
        }
    }

    if (manifest && !manifest->value("firewall_rule", "").empty()) {
        const std::string rule = manifest->value("firewall_rule", "");
        add("firewall", host.firewall_rule_exists(rule) ? "ok" : "warn", "rule '" + rule + "'");
    }

    const std::string upstream = manifest ? manifest->value("upstream", "") : "";
    if (const auto up = parse_upstream(upstream)) {
        const bool up_ok = host.tcp_reachable(up->host == "localhost" ? "127.0.0.1" : up->host, up->port, 1000);
        add("upstream", up_ok ? "ok" : "warn", up_ok ? upstream + " accepts connections" : "nothing listens on " + upstream,
            up_ok ? "" : "start the server: fairyfly mcp --http");
    } else {
        add("upstream", "info", "unknown (no manifest); run setup");
    }

    int fails = 0, warns = 0;
    for (const auto& c : checks) {
        if (c["status"] == "fail") ++fails;
        if (c["status"] == "warn") ++warns;
    }
    Report r;
    r.data = json{{"operation", "status"},
                  {"site_name", o.site_name},
                  {"summary", fails ? "unhealthy" : (warns ? "degraded" : "healthy")},
                  {"failures", fails},
                  {"warnings", warns},
                  {"checks", checks},
                  {"drift", drift}};
    if (!hostname.empty() && port) r.data["url"] = "https://" + hostname + ":" + std::to_string(port) + kMcpPath;
    return r;
}

// ---------------------------------------------------------------------------------------------
// remove

Report run_remove(IisContext& ctx, const RemoveOptions& o) {
    if (const auto p = validate_site(o.site_name, o.site_path)) {
        return failure("INVALID_ARGUMENT", p->message, json{{"field", p->field}});
    }
    IisHost& host = ctx.host;
    const bool dry = o.dry_run;
    if (!dry) {
        const HostFacts facts = host.detect();
        if (!facts.elevated) {
            return failure("IIS_PREREQUISITE_MISSING", "administrator rights are required to remove the site. Nothing was changed.",
                           json{{"missing", json::array({"admin_rights"})}, {"prerequisites", build_checklist(facts, false)}});
        }
        if (!o.yes) {
            return failure("CONFIRMATION_REQUIRED", "remove deletes the IIS site and app pool; review with --dry-run, then re-run with --yes");
        }
    }

    const HostFacts remove_facts = host.detect();
    const bool can_probe = remove_facts.iis_installed && !remove_facts.admin_module.empty();
    json steps = json::array();
    json warnings = json::array();
    std::string current_step = "preflight";
    const auto manifest = read_manifest(host, o.site_path);
    try {
        auto removal = [&](const std::string& name, bool present, const std::function<bool()>& apply, const std::string& detail) {
            if (!present) steps.push_back(make_step(name, "unchanged", "not present: " + detail));
            else if (dry) steps.push_back(make_step(name, "would_remove", detail));
            else steps.push_back(make_step(name, apply() ? "removed" : "unchanged", detail));
        };

        current_step = "firewall";
        if (o.remove_firewall) {
            if (manifest && !manifest->value("firewall_rule", "").empty()) {
                const std::string rule = manifest->value("firewall_rule", "");
                removal("firewall", host.firewall_rule_exists(rule), [&] { return host.remove_firewall_rule(rule); }, rule);
            } else {
                steps.push_back(make_step("firewall", "skipped", "the manifest records no firewall rule set up by fairyfly"));
            }
        }

        current_step = "site";
        removal("site", can_probe && host.get_site(o.site_name).exists, [&] { return host.remove_site(o.site_name); }, o.site_name);
        current_step = "app_pool";
        removal("app_pool", can_probe && host.get_app_pool(o.site_name).exists, [&] { return host.remove_app_pool(o.site_name); }, o.site_name);

        current_step = "files";
        const std::string wc = web_config_path(o.site_path);
        removal("web_config", host.read_file(wc).has_value(), [&] { return host.remove_file(wc); }, wc);
        const std::string mf = manifest_path(o.site_path);
        removal("manifest", manifest.has_value(), [&] { return host.remove_file(mf); }, mf);
        if (manifest && !manifest->value("cer_path", "").empty()) {
            const std::string cer = manifest->value("cer_path", "");
            removal("certificate_export", host.read_file(cer).has_value(), [&] { return host.remove_file(cer); }, cer);
        }
        removal("site_directory", host.dir_exists(o.site_path), [&] { return host.remove_dir_if_empty(o.site_path); }, o.site_path + " (only if empty)");

        current_step = "certificate";
        if (o.remove_cert) {
            const std::string tp = manifest ? manifest->value("cert_thumbprint", "") : "";
            if (manifest && manifest->value("cert_mode", "") == "self-signed" && !tp.empty()) {
                removal("certificate", host.find_certificate(tp).has_value(), [&] { return host.remove_certificate(tp); }, "self-signed " + tp);
            } else {
                steps.push_back(make_step("certificate", "skipped", "not removed: it was not created by `setup --self-signed` (or no manifest)"));
                warnings.push_back("--remove-cert only removes self-signed certificates created by fairyfly");
            }
        }

        current_step = "proxy_secret";
        if (o.delete_secret) {
            const bool present = proxy_secret_or_empty(ctx.secrets).size() > 0;
            removal("proxy_secret", present, [&] { return ctx.secrets.remove(kProxySecretCredential); }, std::string("fairyfly:") + kProxySecretCredential);
        } else {
            steps.push_back(make_step("proxy_secret", "kept", "still stored; use --delete-secret to remove it"));
        }
    } catch (const HostError& e) {
        json details{{"failed_step", current_step}, {"steps", steps},
                     {"rollback", "Removal is idempotent: fix the cause and run remove again."}};
        return failure("IIS_REMOVE_FAILED", "step '" + current_step + "' failed: " + e.message, details);
    } catch (const cred::CredentialError& e) {
        return failure("IIS_REMOVE_FAILED", std::string("credential store: ") + e.what(), json{{"failed_step", current_step}, {"steps", steps}});
    }

    Report r;
    r.data = json{{"operation", "remove"}, {"dry_run", dry}, {"site_name", o.site_name}, {"steps", steps}, {"warnings", warnings}};
    return r;
}

} // namespace fairyfly::iis
