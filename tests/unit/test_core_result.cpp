// fairyfly::Result: the envelope of every command. Errors are built with Result::fail, successes with Result::ok;
// a Result nobody set reports an internal error instead of a misleading "not implemented".
#include <catch2/catch_test_macros.hpp>

#include "include/core.h"

using fairyfly::Result;
using fairyfly::json;

TEST_CASE("Result: a result nobody set is an internal error, not 'not implemented'", "[core][result]") {
    // review 2026-10-25: the default status was NotImplemented ("Phase 1 scaffolding ...")
    const Result r;
    CHECK(r.status == Result::Status::Error);
    const json j = r.to_json();
    CHECK(j["status"] == "error");
    CHECK(j["error"]["code"] == "INTERNAL_NO_STATUS");
    CHECK(j.dump().find("Phase 1") == std::string::npos);
}

TEST_CASE("Result::fail builds the error envelope", "[core][result]") {
    const Result r = Result::fail("ELEMENT_NOT_FOUND", "no such element");
    CHECK(r.status == Result::Status::Error);
    CHECK(r.error == json{{"code", "ELEMENT_NOT_FOUND"}, {"message", "no such element"}});
    const Result s = Result::fail("X", "y", json::array({"try this"}));
    CHECK(s.error["suggestions"] == json::array({"try this"}));
    CHECK(s.to_json()["error"]["code"] == "X");
}

TEST_CASE("Result::set_error turns a result into an error and keeps its other fields", "[core][result]") {
    Result r = Result::ok(json{{"partial", true}});
    r.diagnostics = json{{"step", 3}};
    r.duration = std::chrono::milliseconds(42);
    r.set_error("TAB_LOAD_FAILED", "the tab did not load", json::array({"retry"}));
    CHECK(r.status == Result::Status::Error);
    CHECK(r.error == json{{"code", "TAB_LOAD_FAILED"}, {"message", "the tab did not load"}, {"suggestions", json::array({"retry"})}});
    CHECK(r.data == json{{"partial", true}});
    CHECK(r.diagnostics == json{{"step", 3}});
    CHECK(r.duration.count() == 42);
    Result s;
    s.error["details"] = "kept";
    s.set_error("X", "y");
    CHECK(s.error == json{{"details", "kept"}, {"code", "X"}, {"message", "y"}});  // like the field-by-field writes it replaces
}

TEST_CASE("Result::ok carries the data", "[core][result]") {
    const Result r = Result::ok(json{{"a", 1}});
    CHECK(r.status == Result::Status::Success);
    CHECK(r.data == json{{"a", 1}});
    CHECK(r.to_json()["status"] == "success");
    CHECK(Result::ok().data.is_null());
}

TEST_CASE("Result: NotImplemented names no scaffolding phase", "[core][result]") {
    Result r;
    r.status = Result::Status::NotImplemented;
    const json j = r.to_json();
    CHECK(j["error"]["code"] == "NOT_IMPLEMENTED");
    CHECK(j.dump().find("Phase 1") == std::string::npos);
}
