#pragma once
// Tiny blocking raw HTTP/1.1 client over a plain socket to 127.0.0.1 (tests only). It never adds headers of its
// own: what the test sends is exactly what reaches the server. Every read has a timeout.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

namespace fairyfly::test {

struct RawResponse {
    int status = 0;
    std::string status_line;
    std::map<std::string, std::string> headers;  ///< lower-case names
    std::string body;                            ///< de-chunked
    bool complete = false;                       ///< the whole message was received
    std::string header(const std::string& lower_name) const {
        auto it = headers.find(lower_name);
        return it == headers.end() ? std::string() : it->second;
    }
};

class RawHttpClient {
public:
    RawHttpClient() {
        static const bool wsa = [] {
            WSADATA d;
            return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
        }();
        (void)wsa;
    }
    ~RawHttpClient() { close(); }
    RawHttpClient(const RawHttpClient&) = delete;
    RawHttpClient& operator=(const RawHttpClient&) = delete;

    bool connect(int port, int timeout_ms = 3000) {
        close();
        sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        u_long nonblocking = 1;
        ::ioctlsocket(sock_, FIONBIO, &nonblocking);
        const int rc = ::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (rc != 0 && ::WSAGetLastError() != WSAEWOULDBLOCK) {
            close();
            return false;
        }
        fd_set w;
        FD_ZERO(&w);
        FD_SET(sock_, &w);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        if (::select(0, nullptr, &w, nullptr, &tv) <= 0) {
            close();
            return false;
        }
        int err = 0, len = sizeof(err);
        ::getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        if (err != 0) {
            close();
            return false;
        }
        u_long blocking = 0;
        ::ioctlsocket(sock_, FIONBIO, &blocking);
        return true;
    }

    bool send(std::string_view data) {
        std::size_t off = 0;
        while (off < data.size()) {
            const int n = ::send(sock_, data.data() + off, static_cast<int>(data.size() - off), 0);
            if (n <= 0) return false;
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Reads one response (headers, then body by chunked / Content-Length / until close).
    bool read_response(RawResponse& out, int timeout_ms = 5000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        if (!read_head(out, deadline)) return false;
        const std::string te = out.header("transfer-encoding");
        if (te.find("chunked") != std::string::npos) {
            std::string chunk;
            bool last = false;
            while (read_chunk_until(chunk, last, deadline)) {
                if (last) {
                    out.complete = true;
                    return true;
                }
                out.body += chunk;
            }
            return false;
        }
        const std::string cl = out.header("content-length");
        if (!cl.empty()) {
            const std::size_t n = static_cast<std::size_t>(std::strtoull(cl.c_str(), nullptr, 10));
            while (buf_.size() < n)
                if (!fill(deadline)) return false;
            out.body = buf_.substr(0, n);
            buf_.erase(0, n);
            out.complete = true;
            return true;
        }
        while (fill(deadline)) {}
        out.body = std::move(buf_);
        buf_.clear();
        out.complete = closed_;
        return out.complete;
    }

    /// Reads only status line + headers (for streaming responses; follow with read_chunk()).
    bool read_headers(RawResponse& out, int timeout_ms = 5000) {
        return read_head(out, Clock::now() + std::chrono::milliseconds(timeout_ms));
    }

    /// One chunk of a chunked body. `last` is set for the terminating zero-length chunk.
    bool read_chunk(std::string& data, bool& last, int timeout_ms = 5000) {
        return read_chunk_until(data, last, Clock::now() + std::chrono::milliseconds(timeout_ms));
    }

    /// True once the peer closed the connection (or reset it) within the timeout; drains what arrives meanwhile.
    bool wait_closed(int timeout_ms) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!closed_) {
            if (!fill(deadline)) break;
            buf_.clear();
        }
        return closed_;
    }

    /// Everything that arrives within `timeout_ms` (already buffered bytes first), unparsed.
    std::string drain(int timeout_ms) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (fill(deadline)) {}
        std::string out = std::move(buf_);
        buf_.clear();
        return out;
    }

    bool closed() const { return closed_; }

    void close() {
        if (sock_ != INVALID_SOCKET) ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
        buf_.clear();
        closed_ = false;
    }

    /// One-shot: connect, send `raw`, read one response.
    static bool exchange(int port, std::string_view raw, RawResponse& out, int timeout_ms = 5000) {
        RawHttpClient c;
        return c.connect(port, timeout_ms) && c.send(raw) && c.read_response(out, timeout_ms);
    }

    /// Well-formed POST request text.
    static std::string post(const std::string& path, const std::string& body, const std::string& extra_headers = {},
                            const std::string& host = "127.0.0.1", bool content_type_json = true) {
        std::string r = "POST " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
        if (content_type_json) r += "Content-Type: application/json\r\n";
        r += extra_headers;
        r += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        return r;
    }

private:
    using Clock = std::chrono::steady_clock;

    /// Receives more bytes into buf_. false on timeout, close or error (closed_ is set on close/error).
    bool fill(Clock::time_point deadline) {
        if (sock_ == INVALID_SOCKET || closed_) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return false;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(sock_, &r);
        timeval tv{static_cast<long>(left / 1000), static_cast<long>((left % 1000) * 1000)};
        if (::select(0, &r, nullptr, nullptr, &tv) <= 0) return false;
        char tmp[8192];
        const int n = ::recv(sock_, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            closed_ = true;
            return false;
        }
        buf_.append(tmp, static_cast<std::size_t>(n));
        return true;
    }

    bool read_head(RawResponse& out, Clock::time_point deadline) {
        std::size_t end;
        while ((end = buf_.find("\r\n\r\n")) == std::string::npos)
            if (!fill(deadline)) return false;
        const std::string head = buf_.substr(0, end);
        buf_.erase(0, end + 4);
        std::size_t pos = head.find("\r\n");
        out.status_line = head.substr(0, pos);
        const std::size_t sp = out.status_line.find(' ');
        out.status = sp == std::string::npos ? 0 : std::atoi(out.status_line.c_str() + sp + 1);
        while (pos != std::string::npos) {
            const std::size_t next = head.find("\r\n", pos + 2);
            const std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
            pos = next;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::size_t v = colon + 1;
            while (v < line.size() && line[v] == ' ') ++v;
            out.headers[name] = line.substr(v);
        }
        return true;
    }

    bool read_chunk_until(std::string& data, bool& last, Clock::time_point deadline) {
        std::size_t eol;
        while ((eol = buf_.find("\r\n")) == std::string::npos)
            if (!fill(deadline)) return false;
        const std::size_t size = static_cast<std::size_t>(std::strtoull(buf_.c_str(), nullptr, 16));
        const std::size_t need = eol + 2 + size + 2;
        while (buf_.size() < need)
            if (!fill(deadline)) return false;
        data = buf_.substr(eol + 2, size);
        buf_.erase(0, need);
        last = size == 0;
        return true;
    }

    SOCKET sock_ = INVALID_SOCKET;
    std::string buf_;
    bool closed_ = false;
};

} // namespace fairyfly::test
