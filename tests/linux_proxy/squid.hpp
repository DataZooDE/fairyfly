// Copied from the gateway's tests/squid.hpp (libffl's own proxy tests), for fairyfly's proxy sources end to end on
// Linux: the same squid set-ups, and a TLS target that answers like the gateway (GET /health, the link's WebSocket at
// /ffl with subprotocol ffl.v1). Keep in step with the gateway's copy when it changes.
//
// A real squid for the proxy tests (skipped when squid is not installed; CI installs it, run-unit-tests.sh fails on a
// skip): plain, with Basic auth (basic_ncsa_auth), with NTLM (ntlm_fake_auth: any well-formed NTLM handshake passes,
// so the connection-oriented CONNECT handshake is exercised against a real proxy on Linux), or refusing CONNECT.
// Target host names are resolved by squid from its own hosts_file, so "target.ffy.test" exists only behind the proxy.
// Also: an HTTPS + WebSocket target with a certificate trusted through SSL_CERT_FILE, and an environment guard.
#pragma once
#include <httplib.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <map>
#include <thread>

#include "proxy.hpp"
#include "util.hpp"

namespace ffy::test {

inline uint16_t free_port() {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    socklen_t l = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
    uint16_t p = ntohs(a.sin_port);
    ::close(s);
    return p;
}

template <class F>
bool wait_for(F f, int ms = 8000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return f();
}

inline constexpr const char* kProxiedHost = "target.ffy.test";  // known to squid only

// The first non-loopback IPv4 address of this machine (a target that is not "loopback", which is never proxied).
inline std::string lan_ip() {
    ifaddrs* ifs = nullptr;
    std::string ip;
    if (::getifaddrs(&ifs) != 0) return ip;
    for (auto* i = ifs; i && ip.empty(); i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
        char b[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, b, sizeof b);
        ip = b;
    }
    ::freeifaddrs(ifs);
    return ip;
}

// Sets environment variables for one test and restores them afterwards (the proxy variables are read per request).
class EnvGuard {
public:
    EnvGuard(std::initializer_list<std::pair<const char*, const char*>> vars) {
        for (auto* n : {"HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "NO_PROXY", "no_proxy", "SSL_CERT_FILE"}) save(n), ::unsetenv(n);
        for (auto& [n, v] : vars) {
            save(n);
            if (v) ::setenv(n, v, 1);
            else ::unsetenv(n);
        }
    }
    void set(const char* n, const std::string& v) {
        save(n);
        ::setenv(n, v.c_str(), 1);
    }
    ~EnvGuard() {
        for (auto& [n, v] : saved_)
            if (v) ::setenv(n.c_str(), v->c_str(), 1);
            else ::unsetenv(n.c_str());
    }

private:
    void save(const char* n) {
        if (saved_.count(n)) return;
        const char* v = std::getenv(n);
        saved_[n] = v ? std::optional<std::string>(v) : std::nullopt;
    }
    std::map<std::string, std::optional<std::string>> saved_;
};

// Everything libffl logs through spdlog, at the most detailed level, for one test.
class SpdlogCapture {
public:
    SpdlogCapture() : prev_(spdlog::default_logger()) {
        auto l = std::make_shared<spdlog::logger>("capture", std::make_shared<spdlog::sinks::ostream_sink_mt>(buf_));
        l->set_level(spdlog::level::trace);
        spdlog::set_default_logger(l);
    }
    ~SpdlogCapture() { spdlog::set_default_logger(prev_); }
    std::string str() {
        spdlog::default_logger()->flush();
        return buf_.str();
    }

private:
    std::shared_ptr<spdlog::logger> prev_;
    std::ostringstream buf_;
};

// Installs a proxy configuration for one test, and the default (environment only) afterwards.
struct ProxyConfigGuard {
    explicit ProxyConfigGuard(proxy::Config c) { proxy::configure(std::move(c)); }
    ~ProxyConfigGuard() { proxy::configure({}); }
};

inline proxy::Config via(uint16_t port, std::vector<proxy::AuthenticatorFactory> auth = {}) {
    proxy::Config c;
    c.resolve = [port](const std::string&) { return proxy::Routes{proxy::Route::via({"127.0.0.1", port, proxy::Origin::configured})}; };
    c.authenticators = std::move(auth);
    c.source = "test resolver";
    return c;
}

class Squid {
public:
    enum Auth { None, Basic, Ntlm, Both };
    // deny_connect: every CONNECT is refused (403)
    explicit Squid(Auth auth = None, bool deny_connect = false, uint16_t port = 0) : auth_(auth), deny_(deny_connect) {
        bin_ = find_squid();
        if (bin_.empty()) return;
        port_ = port ? port : free_port();
        char tmpl[] = "/tmp/ffy-squid-XXXXXX";
        dir_ = ::mkdtemp(tmpl);
        // Ubuntu's squid package confines /usr/sbin/squid with an enforcing AppArmor profile (the GitHub runners load it):
        // it may not read a configuration or write logs under /tmp. The profile attaches by path, so a copy of the
        // binary in the test directory runs unconfined, as squid would in a container.
        std::error_code ec;
        std::filesystem::copy_file(bin_, dir_ + "/squid", ec);
        if (!ec) bin_ = dir_ + "/squid";
        start();
    }
    ~Squid() {
        stop();
        std::error_code ec;
        if (!dir_.empty()) std::filesystem::remove_all(dir_, ec);
    }
    bool ok() const { return ready_; }
    // why it did not start: squid's own output (for the SKIP message, which CI turns into a failure)
    std::string why() const {
        std::string out;
        for (auto* f : {"/stderr.log", "/cache.log"}) {
            std::ifstream in(dir_ + f);
            std::stringstream b;
            b << in.rdbuf();
            out += b.str();
        }
        return bin_.empty() ? std::string("squid not installed (apt install squid)") : "squid did not start: " + out.substr(out.size() > 1500 ? out.size() - 1500 : 0);
    }
    uint16_t port() const { return port_; }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    // the proxy dies (connections through it break); start() brings it back on the same port
    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
        ready_ = false;
    }
    void start() {
        write_config();
        pid_ = ::fork();
        if (pid_ == 0) {
            ::setpgid(0, 0);
            if (!std::freopen((dir_ + "/stdout.log").c_str(), "a", stdout)) ::_exit(126);
            if (!std::freopen((dir_ + "/stderr.log").c_str(), "a", stderr)) ::_exit(126);
            std::string conf = dir_ + "/squid.conf";
            // -n: its own service name, so its shared-memory segments (/dev/shm/<name>-...) never collide with another
            // squid's (apt starts the system service on the GitHub runners, which owns the default names)
            std::string name = "ffytest" + std::to_string(::getpid()) + "p" + std::to_string(port_);
            ::execl(bin_.c_str(), bin_.c_str(), "-N", "-n", name.c_str(), "-f", conf.c_str(), (char*)nullptr);
            ::_exit(127);
        }
        ready_ = wait_for(
            [&] {
                int s = ::socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                a.sin_port = htons(port_);
                bool up = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
                ::close(s);
                return up;
            },
            20000);
    }
    // squid's access log: one line per request ("... TCP_TUNNEL/200 ... CONNECT target.ffy.test:4433 alice ...")
    std::string access_log() const {
        std::ifstream f(dir_ + "/access.log");
        std::stringstream s;
        s << f.rdbuf();
        return s.str();
    }
    bool tunnelled(const std::string& hostport, const std::string& user = "-") const {
        std::istringstream in(access_log());
        for (std::string l; std::getline(in, l);)
            if (l.find("TCP_TUNNEL/200") != std::string::npos && l.find("CONNECT " + hostport + " ") != std::string::npos &&
                (user == "-" || l.find(" " + user + " ") != std::string::npos))
                return true;
        return false;
    }
    static constexpr const char* kUser = "alice";
    static constexpr const char* kPassword = "s3cret-Pw";  // checked to appear in no error message

private:
    static std::string find_squid() {
        for (auto* p : {"/usr/sbin/squid", "/usr/local/sbin/squid", "/usr/bin/squid"})
            if (::access(p, X_OK) == 0) return p;
        return {};
    }
    void write_config() {
        {
            std::ofstream h(dir_ + "/hosts");
            h << "127.0.0.1 " << kProxiedHost << "\n";
        }
        {
            // basic_ncsa_auth accepts crypt/MD5 hashes: make one with openssl (installed with the build tools)
            std::string cmd = std::string("openssl passwd -apr1 '") + kPassword + "' > " + dir_ + "/hash";
            if (std::system(cmd.c_str()) == 0) {
                std::ifstream in(dir_ + "/hash");
                std::string hash;
                std::getline(in, hash);
                std::ofstream pw(dir_ + "/passwd");
                pw << kUser << ":" << hash << "\n";
            }
        }
        std::ofstream c(dir_ + "/squid.conf");
        c << "http_port 127.0.0.1:" << port_ << "\n"
          << "pid_filename none\n"
          << "cache_log " << dir_ << "/cache.log\n"
          << "access_log stdio:" << dir_ << "/access.log\n"
          << "logfile_rotate 0\n"
          << "cache deny all\n"
          << "hosts_file " << dir_ << "/hosts\n"
          << "dns_nameservers 127.0.0.1\n"
          << "shutdown_lifetime 0 seconds\n"
          << "visible_hostname ffy-test-proxy\n"
          << "buffered_logs off\n";
        if (auth_ == Ntlm || auth_ == Both)
            c << "auth_param ntlm program /usr/lib/squid/ntlm_fake_auth\nauth_param ntlm children 2\n";
        if (auth_ == Basic || auth_ == Both)
            c << "auth_param basic program /usr/lib/squid/basic_ncsa_auth " << dir_ << "/passwd\nauth_param basic children 2\n"
              << "auth_param basic realm ffy-test\n";
        if (deny_) c << "http_access deny CONNECT\n";
        if (auth_ != None) c << "acl authed proxy_auth REQUIRED\nhttp_access allow authed\nhttp_access deny all\n";
        else c << "http_access allow all\n";
    }

    std::string bin_, dir_;
    Auth auth_;
    bool deny_;
    pid_t pid_ = -1;
    uint16_t port_ = 0;
    bool ready_ = false;
};

// A test NTLM client: a well-formed Type 1, then, for the proxy's Type 2 challenge, a Type 3 for CORP\bob (squid's
// ntlm_fake_auth checks the form, not the response). Stands in for Windows SSPI, which fairyfly supplies on Windows.
class TestNtlm final : public proxy::Authenticator {
public:
    std::optional<std::string> respond(const std::vector<std::string>& ch) override {
        for (auto& c : ch) {
            if (c.rfind("NTLM", 0) != 0) continue;
            std::string tok = trim(c.substr(4));
            if (step_ == 0 && tok.empty()) {
                step_ = 1;
                return "NTLM " + b64std_encode(type1());
            }
            if (step_ == 1 && !tok.empty()) {
                std::string t2 = b64std_decode(tok);
                if (t2.size() < 32 || t2.compare(0, 8, std::string("NTLMSSP\0", 8)) != 0 || t2[8] != 2) return std::nullopt;
                if (challenges) ++*challenges;
                step_ = 2;
                return "NTLM " + b64std_encode(type3("CORP", "bob", "WS1"));
            }
        }
        return std::nullopt;
    }
    std::string scheme() const override { return step_ ? "NTLM" : ""; }
    // counts the Type 2 challenges answered; shared, since the tunnel owns (and ends) the authenticator
    std::shared_ptr<std::atomic<int>> challenges;

private:
    static std::string le16(uint16_t v) { return {char(v & 0xff), char(v >> 8)}; }
    static std::string le32(uint32_t v) { return {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char(v >> 24)}; }
    static std::string type1() {
        std::string m("NTLMSSP\0", 8);
        m += le32(1) + le32(0x00088206) + std::string(16, '\0');  // flags: unicode, OEM, request target, NTLM, always sign
        return m;
    }
    static std::string type3(const std::string& domain, const std::string& user, const std::string& ws) {
        auto u16 = [](const std::string& s) {
            std::string o;
            for (char c : s) o += c, o += '\0';
            return o;
        };
        std::string lm(24, '\0'), nt(24, '\x11'), d = u16(domain), us = u16(user), w = u16(ws);
        uint32_t off = 64;
        std::string hdr("NTLMSSP\0", 8);
        hdr += le32(3);
        std::string payload;
        auto buf = [&](const std::string& b) {
            hdr += le16(uint16_t(b.size())) + le16(uint16_t(b.size())) + le32(off + uint32_t(payload.size()));
            payload += b;
        };
        buf(lm);
        buf(nt);
        buf(d);
        buf(us);
        buf(w);
        buf(std::string());               // session key
        hdr += le32(0x00088205);          // flags: unicode, request target, NTLM
        return hdr + payload;
    }
    int step_ = 0;
};

// The cloud side in miniature: HTTPS (GET /hello) and a WebSocket echo (/ws, subprotocol echoed) on one TLS port,
// with a self-signed certificate for target.ffy.test, this machine's LAN address and localhost, trusted by pointing
// SSL_CERT_FILE at it (the same trust path as the platform store on Linux).
class TlsTarget {
public:
    TlsTarget() {
        ip_ = lan_ip();
        EVP_PKEY* key = EVP_EC_gen("P-256");
        X509* cert = X509_new();
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 7);
        X509_gmtime_adj(X509_getm_notBefore(cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        X509_NAME* n = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(kProxiedHost), -1, -1, 0);
        X509_set_issuer_name(cert, n);
        X509V3_CTX v3;
        X509V3_set_ctx_nodb(&v3);
        X509V3_set_ctx(&v3, cert, cert, nullptr, nullptr, 0);
        std::string san = std::string("DNS:") + kProxiedHost + ",DNS:localhost,IP:127.0.0.1" + (ip_.empty() ? "" : ",IP:" + ip_);
        X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, NID_subject_alt_name, san.c_str());
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
        X509_EXTENSION* bc = X509V3_EXT_conf_nid(nullptr, &v3, NID_basic_constraints, "critical,CA:TRUE");
        X509_add_ext(cert, bc, -1);
        X509_EXTENSION_free(bc);
        X509_sign(cert, key, EVP_sha256());
        cert_pem_ = pem([&](BIO* b) { PEM_write_bio_X509(b, cert); });
        key_pem_ = pem([&](BIO* b) { PEM_write_bio_PrivateKey(b, key, nullptr, nullptr, 0, nullptr, nullptr); });
        X509_free(cert);
        EVP_PKEY_free(key);
        char tmpl[] = "/tmp/ffy-ca-XXXXXX";
        int fd = ::mkstemp(tmpl);
        ca_file_ = tmpl;
        ::write(fd, cert_pem_.data(), cert_pem_.size());
        ::close(fd);

        httplib::SSLServer::PemMemory pm{cert_pem_.c_str(), cert_pem_.size(), key_pem_.c_str(), key_pem_.size(), nullptr};
        srv_ = std::make_unique<httplib::SSLServer>(pm);
        srv_->Get("/hello", [this](const httplib::Request& rq, httplib::Response& r) {
            ++hits;
            r.set_content("hello over tls, host " + rq.get_header_value("Host"), "text/plain");
        });
        srv_->Get("/health", [this](const httplib::Request&, httplib::Response& r) {
            ++hits;
            r.set_content("ok", "text/plain");
        });
        srv_->WebSocket("/ffl", [this](const httplib::Request&, httplib::ws::WebSocket& ws) {
            ++hits;
            std::string m;
            while (ws.read(m)) ws.send("echo:" + m);
        }, [](const std::vector<std::string>& protos) { return protos.empty() ? std::string() : protos.front(); });
        port_ = srv_->bind_to_any_port("0.0.0.0");
        th_ = std::thread([this] { srv_->listen_after_bind(); });
        srv_->wait_until_ready();
    }
    ~TlsTarget() {
        srv_->stop();
        if (th_.joinable()) th_.join();
        ::unlink(ca_file_.c_str());
    }
    int port() const { return port_; }
    const std::string& ca_file() const { return ca_file_; }
    const std::string& ip() const { return ip_; }
    std::string hostport(const std::string& host = kProxiedHost) const { return host + ":" + std::to_string(port_); }
    std::string https(const std::string& host = kProxiedHost) const { return "https://" + hostport(host) + "/hello"; }
    std::string wss(const std::string& host = kProxiedHost) const { return "wss://" + hostport(host) + "/ffl"; }
    std::atomic<int> hits{0};

private:
    template <class F>
    static std::string pem(F f) {
        BIO* b = BIO_new(BIO_s_mem());
        f(b);
        char* d;
        long n = BIO_get_mem_data(b, &d);
        std::string s(d, size_t(n));
        BIO_free(b);
        return s;
    }
    std::string ip_, cert_pem_, key_pem_, ca_file_;
    std::unique_ptr<httplib::SSLServer> srv_;
    std::thread th_;
    int port_ = 0;
};

}  // namespace ffy::test
