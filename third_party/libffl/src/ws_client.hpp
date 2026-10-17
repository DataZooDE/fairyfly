// Blocking WebSocket client on cpp-httplib (ws:// for tests, wss:// with certificate + host name verification).
// read() blocks on the owning thread; send_text()/close()/abort() may be called from any thread.
#pragma once
#include <memory>
#include <optional>
#include <string>

namespace ffy {

class WsClient {
public:
    ~WsClient();
    // Throws ffy::Error. connect_timeout_ms bounds the TCP/TLS connect and, separately, the upgrade response.
    static std::unique_ptr<WsClient> connect(const std::string& url, const std::string& subprotocol, int connect_timeout_ms);
    bool send_text(const std::string& s);
    std::optional<std::string> read(size_t max_bytes);  // nullopt on close/error
    void abort();
    void close();

private:
    struct Impl;
    explicit WsClient(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

}  // namespace ffy
