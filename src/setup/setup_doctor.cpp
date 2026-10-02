#include "include/setup/setup_doctor.h"

#include "include/setup/setup_model.h"
#include "include/setup/setup_service.h"

namespace fairyfly::setup {

config::SetupFacts collect_setup_facts(Hosts& h, const config::SetupQuery& q) {
    config::SetupFacts f;
    f.available = true;
    f.now = h.sys.now();
    f.elevation = elevation_name(h.elevator.elevation_type());

    f.manifest_path = manifest_path_for(h.sys);
    const auto manifest = load_manifest(h.sys, f.manifest_path, &f.manifest_unreadable);
    bool tls = q.tls;
    std::string hostname = q.hostname;
    int port = q.port;
    if (manifest) {
        f.manifest_present = true;
        f.manifest_mode = manifest->mode;
        f.manifest_hostname = manifest->hostname;
        f.manifest_port = manifest->port;
        f.manifest_thumbprint = manifest->thumbprint;
        f.manifest_firewall_rule = manifest->firewall_rule;
        // The recorded setup is what was installed: judge the machine against it.
        tls = manifest->mode == "tls";
        hostname = manifest->hostname;
        port = manifest->port;
    }
    if (hostname.empty()) hostname = default_hostname(h.sys.computer_dns_name());
    f.prefix = url_prefix(tls, hostname, port);

    // urlacl
    try {
        const auto sddl = h.http.query_urlacl(f.prefix);
        f.urlacl_known = true;
        f.urlacl_reserved = sddl.has_value();
        f.urlacl_covers_user = sddl && sddl_covers_sid(*sddl, h.elevator.current_user_sid());
    } catch (const std::exception&) {
        f.urlacl_known = false;
    }

    // sslcert + certificate
    std::string thumb = f.manifest_thumbprint;
    if (tls) {
        try {
            const auto v4 = h.http.query_sslcert("0.0.0.0:" + std::to_string(port));
            const auto v6 = h.http.query_sslcert("[::]:" + std::to_string(port));
            f.ssl_known = true;
            f.ssl_v4 = v4.has_value() && is_our_app_id(v4->app_id);
            f.ssl_v6 = v6.has_value() && is_our_app_id(v6->app_id);
            f.ssl_foreign = (v4 && !is_our_app_id(v4->app_id)) || (v6 && !is_our_app_id(v6->app_id));
            if (v4 && is_our_app_id(v4->app_id)) f.ssl_thumbprint = v4->thumbprint;
        } catch (const std::exception&) {
            f.ssl_known = false;
        }
        if (!f.ssl_thumbprint.empty()) thumb = f.ssl_thumbprint;
        try {
            CertInfo cert;
            if (!thumb.empty()) cert = h.certs.find_by_thumbprint(thumb);
            else cert = h.certs.find_self_signed(hostname);
            f.cert_known = true;
            f.cert_found = cert.found;
            f.cert_thumbprint = cert.thumbprint;
            f.cert_has_key = cert.has_private_key;
            f.cert_not_after = cert.not_after;
            f.cert_san_ok = san_matches(cert.dns_names, hostname);
        } catch (const std::exception&) {
            f.cert_known = false;
        }
    }

    // firewall: only the rule setup recorded
    if (!f.manifest_firewall_rule.empty()) {
        try {
            // by the unique internal Name; an older manifest only has the display name
            f.firewall_exists = manifest && !manifest->firewall_rule_name.empty() && firewall_name_ok(manifest->firewall_rule_name)
                                    ? h.firewall.exists(manifest->firewall_rule_name)
                                    : h.firewall.display_name_exists(f.manifest_firewall_rule);
            f.firewall_known = true;
        } catch (const std::exception&) {
            f.firewall_known = false;
        }
    }

    // handshake: only when something listens
    f.port_listening = h.sys.tcp_listening("127.0.0.1", port);
    if (tls && f.port_listening) {
        const TlsProbe probe = h.sys.tls_probe(hostname, port, f.cert_thumbprint);
        f.tls_status = probe.status;
        f.tls_protocol = probe.protocol;
        f.tls_thumbprint_match = probe.thumbprint_match;
    }
    return f;
}

} // namespace fairyfly::setup
