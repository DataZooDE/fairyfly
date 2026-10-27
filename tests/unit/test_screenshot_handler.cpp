#include <catch2/catch_test_macros.hpp>
#include "include/screenshot_handler.h"
#include "include/cli_handler.h"
#include "include/com/wrapper.h"
#include <filesystem>
#include <cstring>
#include <fstream>

#include "support/fake_dispatch.h"

using namespace fairyfly;
using namespace fairyfly::sap;

TEST_CASE("ScreenshotHandler - Validation", "[screenshot][handler]") {
    // Test with null session (should handle gracefully)
    ScreenshotHandler handler(nullptr);

    SECTION("Validates subsection parameters") {
        cli::ScreenshotOptions opts;

        SECTION("All parameters provided") {
            opts.crop_x = 10;
            opts.crop_y = 20;
            opts.crop_width = 100;
            opts.crop_height = 200;
            // Should not throw
            REQUIRE_NOTHROW(ScreenshotHandler::validate_subsection_complete(opts));
        }

        SECTION("Partial parameters throw") {
            opts.crop_x = 10;
            opts.crop_y = 20;
            // Missing width/height
            REQUIRE_THROWS_AS(
                ScreenshotHandler::validate_subsection_complete(opts),
                std::invalid_argument
            );
        }

        SECTION("No parameters valid") {
            // Empty options should be valid
            REQUIRE_NOTHROW(ScreenshotHandler::validate_subsection_complete(opts));
        }

        SECTION("Negative coordinates throw") {
            opts.crop_x = -1;
            opts.crop_y = 20;
            opts.crop_width = 100;
            opts.crop_height = 200;
            REQUIRE_THROWS_AS(
                ScreenshotHandler::validate_subsection_complete(opts),
                std::invalid_argument
            );
        }

        SECTION("Zero width throws") {
            opts.crop_x = 10;
            opts.crop_y = 20;
            opts.crop_width = 0;
            opts.crop_height = 200;
            REQUIRE_THROWS_AS(
                ScreenshotHandler::validate_subsection_complete(opts),
                std::invalid_argument
            );
        }
    }
}

TEST_CASE("ScreenshotHandler - Scale parsing", "[screenshot][handler]") {
    SECTION("Parses float scale factor") {
        int new_width, new_height;
        ScreenshotHandler::parse_scale_parameter("0.5", 1000, 500, new_width, new_height);
        REQUIRE(new_width == 500);
        REQUIRE(new_height == 250);
    }

    SECTION("Parses integer width target") {
        int new_width, new_height;
        ScreenshotHandler::parse_scale_parameter("800", 1600, 900, new_width, new_height);
        REQUIRE(new_width == 800);
        REQUIRE(new_height == 450);  // Maintains aspect ratio
    }

    SECTION("Throws on invalid scale factor") {
        int new_width, new_height;
        REQUIRE_THROWS_AS(
            ScreenshotHandler::parse_scale_parameter("-0.5", 1000, 500, new_width, new_height),
            std::invalid_argument
        );
    }

    SECTION("Throws on zero width") {
        int new_width, new_height;
        REQUIRE_THROWS_AS(
            ScreenshotHandler::parse_scale_parameter("0", 1000, 500, new_width, new_height),
            std::invalid_argument
        );
    }

    SECTION("Throws on excessive scale factor") {
        int new_width, new_height;
        REQUIRE_THROWS_AS(
            ScreenshotHandler::parse_scale_parameter("11.0", 1000, 500, new_width, new_height),
            std::invalid_argument
        );
    }
}

TEST_CASE("ScreenshotHandler - Capture with no session", "[screenshot][handler]") {
    ScreenshotHandler handler(nullptr);
    cli::ScreenshotOptions opts;
    opts.output_file = "test.png";

    Result result = handler.capture(opts);

    REQUIRE(result.status == Result::Status::Error);
    REQUIRE(result.error["code"] == "NO_SESSION");
}

TEST_CASE("ScreenshotHandler - base64 output honours --file", "[screenshot][handler]") {
    const std::string b64 = "iVBORw0KGgo=";
    SECTION("no file keeps the data URI in the result") {
        Result result;
        ScreenshotHandler::emit_base64_output(result, b64, "");
        CHECK(result.data["screenshot"] == "data:image/png;base64," + b64);
        CHECK(result.data["format"] == "base64");
        CHECK_FALSE(result.data.contains("filepath"));
    }
    SECTION("'-' keeps the result on stdout") {
        Result result;
        ScreenshotHandler::emit_base64_output(result, b64, "-");
        CHECK(result.data["screenshot"] == "data:image/png;base64," + b64);
        CHECK_FALSE(result.data.contains("filepath"));
    }
    SECTION("a file path writes the data URI and reports filepath without repeating it") {
        const auto path = (std::filesystem::temp_directory_path() / "fairyfly_b64_capture_test.txt").string();
        std::filesystem::remove(path);
        Result result;
        ScreenshotHandler::emit_base64_output(result, b64, path);
        CHECK(result.data["filepath"] == path);
        CHECK(result.data["format"] == "base64");
        CHECK_FALSE(result.data.contains("screenshot"));
        std::ifstream in(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(content == "data:image/png;base64," + b64);
        in.close();
        std::filesystem::remove(path);
    }
    SECTION("an unwritable path throws") {
        Result result;
        CHECK_THROWS_AS(ScreenshotHandler::emit_base64_output(result, b64, "Z:\\no\\such\\dir\\x.txt"),
                        std::runtime_error);
    }
}


namespace {

/// A session whose active window answers HardCopy / HardCopyToMemory (a 4x2 RGB PNG).
struct ScreenshotScene {
    fairyfly::test::ScopedDispatchCacheReset cache_reset;
    fairyfly::test::FakeNode* session = new fairyfly::test::FakeNode();
    fairyfly::test::FakeNode* window = new fairyfly::test::FakeNode();
    IDispatchPtr session_holder{session, false}, window_holder{window, false};
    ScreenshotScene() {
        session->strings[L"Type"] = L"GuiSession";
        window->strings[L"Type"] = L"GuiMainWindow";
        window->strings[L"Text"] = L"SAP Easy Access";
        session->dispatches[L"ActiveWindow"] = window;
        window->methods = {L"HardCopy", L"HardCopyToMemory"};
        static const unsigned char png[] = {
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
            0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x08, 0x02, 0x00, 0x00, 0x00, 0xF0, 0xCA, 0xEA, 0x34, 0x00, 0x00, 0x00,
            0x10, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x38, 0x91, 0x62, 0x04, 0x47, 0x0C, 0xC8, 0x1C, 0x00, 0x92,
            0xFA, 0x0A, 0xF1, 0x4B, 0x4E, 0x88, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60,
            0x82};
        SAFEARRAY* bytes = SafeArrayCreateVector(VT_UI1, 0, sizeof(png));
        void* data = nullptr;
        SafeArrayAccessData(bytes, &data);
        std::memcpy(data, png, sizeof(png));
        SafeArrayUnaccessData(bytes);
        _variant_t answer;
        answer.vt = VT_ARRAY | VT_UI1;
        answer.parray = bytes;
        window->method_values[L"HardCopyToMemory"] = answer;
    }
    ScreenshotHandler handler() { return ScreenshotHandler(ComGuiSession::create(IDispatchPtr(session, true))); }
};

}  // namespace

TEST_CASE("ScreenshotHandler - a file without processing is written by SAP GUI itself (HardCopy)", "[screenshot][handler]") {
    // Codex on #90: the extracted capture paths through a fake window
    ScreenshotScene scene;
    const auto path = std::filesystem::absolute(std::filesystem::temp_directory_path() / "fairyfly_hardcopy_test.png");
    cli::ScreenshotOptions opts;
    opts.output_file = path.string();

    SECTION("the file SAP GUI wrote is the result") {
        { std::ofstream(path, std::ios::binary) << "png"; }   // what SAP GUI would have written
        const Result r = scene.handler().capture(opts);
        INFO(r.to_json().dump());
        REQUIRE(r.status == Result::Status::Success);
        CHECK(r.data["filepath"] == path.string());
        REQUIRE(scene.window->calls.size() == 1);
        CHECK(scene.window->calls[0].first == L"HardCopy");
        CHECK(scene.window->calls[0].second == path.wstring() + L",2");   // the absolute path, IMAGE_TYPE_PNG
    }
    SECTION("no file from SAP GUI is SCREENSHOT_NOT_CREATED") {
        std::filesystem::remove(path);
        const Result r = scene.handler().capture(opts);
        CHECK(r.error["code"] == "SCREENSHOT_NOT_CREATED");
    }
    SECTION("a failing HardCopy is reported with its HRESULT") {
        scene.window->failing_calls = {L"HardCopy(" + path.wstring() + L",2)"};
        const Result r = scene.handler().capture(opts);
        INFO(r.to_json().dump());
        CHECK(r.error["code"] == "EXCEPTION");
        CHECK(r.error["message"].get<std::string>().rfind("HardCopy failed: 0x80020009", 0) == 0);
    }
    std::filesystem::remove(path);
}

TEST_CASE("ScreenshotHandler - a crop always takes the processing path (HardCopyToMemory, native pixels)", "[screenshot][handler]") {
    ScreenshotScene scene;
    cli::ScreenshotOptions opts;
    opts.output_file = (std::filesystem::temp_directory_path() / "fairyfly_crop_test.png").string();
    opts.format = "base64";
    opts.crop_x = 1;
    opts.crop_y = 0;
    opts.crop_width = 2;
    opts.crop_height = 2;
    const Result r = scene.handler().capture(opts);
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    REQUIRE(scene.window->calls.size() == 1);
    CHECK(scene.window->calls[0] == std::pair<std::wstring, std::wstring>{L"HardCopyToMemory", L"2"});
    CHECK(r.data["native_size"] == json{{"width", 4}, {"height", 2}});
    CHECK(r.data["output_size"] == json{{"width", 2}, {"height", 2}});
    std::filesystem::remove(opts.output_file);
}

TEST_CASE("ScreenshotHandler - a crop completely outside the native screenshot is refused, not a blank image", "[screenshot][handler]") {
    ScreenshotScene scene;
    cli::ScreenshotOptions opts;
    opts.format = "base64";
    opts.crop_x = 4;   // the native width is 4
    opts.crop_y = 0;
    opts.crop_width = 5;
    opts.crop_height = 2;
    const Result r = scene.handler().capture(opts);
    CHECK(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "INVALID_ARGUMENT");
}
