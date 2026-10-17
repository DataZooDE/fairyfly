// WebSocket client on cpp-httplib. read() runs on the owning thread; send_text()/close()/abort() may be called from
// any other thread (supported by cpp-httplib, which serialises frame writes and TLS calls internally).
#include "ws_client.hpp"

#include <httplib.h>

#include <atomic>

#include "util.hpp"

namespace ffy {

struct WsClient::Impl {
    std::unique_ptr<httplib::ws::WebSocketClient> ws;
    std::atomic<bool> aborted{false};
    std::atomic<bool> closed{false};
};

WsClient::WsClient(std::unique_ptr<Impl> i) : impl_(std::move(i)) {}
WsClient::~WsClient() {
    try { close(); } catch (...) {}
}

std::unique_ptr<WsClient> WsClient::connect(const std::string& url, const std::string& sub, int timeout_ms) {
    Url u = parse_url(url);
    if (u.scheme != "ws" && u.scheme != "wss") throw Error("link url must be ws:// or wss://");
    auto im = std::make_unique<Impl>();
    httplib::Headers headers{{"Sec-WebSocket-Protocol", sub}, {"User-Agent", "ffy-link/0.2"}};
    im->ws = std::make_unique<httplib::ws::WebSocketClient>(url, headers);
    if (!im->ws->is_valid()) throw Error("invalid websocket url " + url);
    // the connect and the upgrade response are each bounded by timeout_ms
    im->ws->set_connection_timeout(std::chrono::milliseconds(timeout_ms));
    im->ws->set_read_timeout(std::chrono::milliseconds(timeout_ms));
    im->ws->set_write_timeout(std::chrono::seconds(20));
    auto res = im->ws->connect();
    if (!res) {
        std::string why = httplib::to_string(res.error());
        if (res.status() != -1) why += " (HTTP " + std::to_string(res.status()) + ")";
        throw Error("websocket connect " + url + ": " + why);
    }
    // from now on reads poll in short slices so abort()/close() from another thread take effect promptly
    im->ws->set_read_timeout(std::chrono::milliseconds(200));
    return std::unique_ptr<WsClient>(new WsClient(std::move(im)));
}

bool WsClient::send_text(const std::string& s) {
    if (impl_->aborted || !impl_->ws->is_open()) return false;
    return impl_->ws->send(s);
}

std::optional<std::string> WsClient::read(size_t max_bytes) {
    std::string msg;
    for (;;) {
        if (impl_->aborted) return std::nullopt;
        auto r = impl_->ws->read(msg);
        if (r == httplib::ws::Timeout) continue;
        if (r == httplib::ws::Fail) return std::nullopt;
        if (msg.size() > max_bytes) {
            abort();
            return std::nullopt;
        }
        return msg;
    }
}

void WsClient::abort() {
    impl_->aborted = true;
    if (!impl_->closed.exchange(true)) impl_->ws->close(httplib::ws::CloseStatus::GoingAway, "");
}

void WsClient::close() {
    impl_->aborted = true;
    if (!impl_->closed.exchange(true)) impl_->ws->close(httplib::ws::CloseStatus::Normal, "");
}

}  // namespace ffy
