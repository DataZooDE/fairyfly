#include <catch2/catch_test_macros.hpp>
#include "include/screenshot_handler.h"
#include "include/cli_handler.h"
#include "include/com/wrapper.h"

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

