#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <string>

#include "include/mcp/json_rpc.h"
#include "include/mcp/transport.h"

using namespace fairyfly::mcp;

namespace {
Message parse(const std::string& line, json& err) { return parse_message(line, err); }
}

TEST_CASE("parse_message classifies requests, notifications and responses", "[mcp][jsonrpc]") {
    json err;
    auto req = parse(R"({"jsonrpc":"2.0","id":7,"method":"ping"})", err);
    CHECK(req.kind == Message::Kind::Request);
    CHECK(req.id == 7);
    CHECK(req.method == "ping");
    CHECK(err.is_null());

    auto note = parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", err);
    CHECK(note.kind == Message::Kind::Notification);
    CHECK(note.id.is_null());

    auto resp = parse(R"({"jsonrpc":"2.0","id":"a","result":{}})", err);
    CHECK(resp.kind == Message::Kind::Response);
    auto resp_err = parse(R"({"jsonrpc":"2.0","id":1,"error":{"code":-1,"message":"x"}})", err);
    CHECK(resp_err.kind == Message::Kind::Response);
}

TEST_CASE("parse_message id types", "[mcp][jsonrpc]") {
    json err;
    CHECK(parse(R"({"id":"abc","method":"m"})", err).kind == Message::Kind::Request);
    CHECK(parse(R"({"id":0,"method":"m"})", err).kind == Message::Kind::Request);
    CHECK(parse(R"({"id":-5,"method":"m"})", err).kind == Message::Kind::Request);

    for (const char* bad : {R"({"id":null,"method":"m"})", R"({"id":1.5,"method":"m"})",
                            R"({"id":true,"method":"m"})", R"({"id":{},"method":"m"})",
                            R"({"id":[1],"method":"m"})"}) {
        INFO(bad);
        auto m = parse(bad, err);
        CHECK(m.kind == Message::Kind::Invalid);
        REQUIRE(err.is_object());
        CHECK(err["error"]["code"] == kInvalidRequest);
        CHECK(err["id"].is_null());
    }
}

TEST_CASE("parse_message tolerates BOM, CRLF and whitespace", "[mcp][jsonrpc]") {
    json err;
    auto m = parse(std::string("\xEF\xBB\xBF") + R"({"id":1,"method":"ping"})" + "\r\n", err);
    CHECK(m.kind == Message::Kind::Request);
    m = parse("   \t" + std::string(R"({"id":1,"method":"ping"})") + "  \r", err);
    CHECK(m.kind == Message::Kind::Request);
}

TEST_CASE("parse_message skips blank lines silently", "[mcp][jsonrpc]") {
    json err = "sentinel";
    for (const char* blank : {"", "   ", "\r\n", "\t"}) {
        auto m = parse(blank, err);
        CHECK(m.kind == Message::Kind::Invalid);
        CHECK(err.is_null());
    }
    auto bom_only = parse("\xEF\xBB\xBF", err);
    CHECK(bom_only.kind == Message::Kind::Invalid);
    CHECK(err.is_null());
}

TEST_CASE("parse_message rejects invalid JSON, arrays and non-objects", "[mcp][jsonrpc]") {
    json err;
    auto m = parse("{not json", err);
    CHECK(m.kind == Message::Kind::Invalid);
    CHECK(err["error"]["code"] == kParseError);
    CHECK(err["id"].is_null());

    m = parse(R"([{"id":1,"method":"ping"}])", err);
    CHECK(m.kind == Message::Kind::Invalid);
    CHECK(err["error"]["code"] == kInvalidRequest);

    for (const char* scalar : {"42", "\"text\"", "true", "null"}) {
        m = parse(scalar, err);
        CHECK(m.kind == Message::Kind::Invalid);
        CHECK(err["error"]["code"] == kInvalidRequest);
    }

    m = parse(R"({"id":3,"foo":1})", err);
    CHECK(m.kind == Message::Kind::Invalid);
    CHECK(err["error"]["code"] == kInvalidRequest);
    CHECK(err["id"] == 3);

    m = parse(R"({"id":1,"method":5})", err);
    CHECK(m.kind == Message::Kind::Invalid);
    CHECK(err["error"]["code"] == kInvalidRequest);

    m = parse(R"({"id":1,"method":"m","params":5})", err);
    CHECK(m.kind == Message::Kind::Invalid);
    CHECK(err["id"] == 1);
}

TEST_CASE("make_result and make_error are compact and single-line", "[mcp][jsonrpc]") {
    json result = make_result(5, json{{"text", "line1\nline2\r\n\"q\""}});
    std::string dumped = result.dump();
    CHECK(dumped.find('\n') == std::string::npos);
    CHECK(dumped.find('\r') == std::string::npos);
    CHECK(dumped.find(": ") == std::string::npos);
    CHECK(result["jsonrpc"] == "2.0");
    CHECK(result["id"] == 5);

    json error = make_error(nullptr, kParseError, "bad");
    CHECK(error["id"].is_null());
    CHECK_FALSE(error["error"].contains("data"));
    json with_data = make_error("x", kServerBusy, "server busy", json{{"retry", true}});
    CHECK(with_data["error"]["data"]["retry"] == true);
}

TEST_CASE("StringTransport writes one line per message", "[mcp][jsonrpc]") {
    std::istringstream in("first\r\nsecond\n");
    std::ostringstream out;
    StringTransport transport(in, out);
    std::string line;
    REQUIRE(transport.read_line(line));
    CHECK(line == "first");
    REQUIRE(transport.read_line(line));
    CHECK(line == "second");
    CHECK_FALSE(transport.read_line(line));
    transport.write_line(make_result(1, json::object()).dump());
    CHECK(out.str() == "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{}}\n");
}
