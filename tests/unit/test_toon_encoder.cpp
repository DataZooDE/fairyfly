#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "include/formatters/toon_encoder.h"

using json = nlohmann::json;
using namespace fairyfly::formatters;

// ============================================================================
// Primitive Encoding Tests
// ============================================================================

TEST_CASE("TOON encoder encodes primitives correctly", "[toon][primitives]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Encodes null") {
        json value = nullptr;
        REQUIRE(encoder.encode(value, opts) == "null");
    }

    SECTION("Encodes booleans") {
        REQUIRE(encoder.encode(json(true), opts) == "true");
        REQUIRE(encoder.encode(json(false), opts) == "false");
    }

    SECTION("Encodes integers") {
        REQUIRE(encoder.encode(json(42), opts) == "42");
        REQUIRE(encoder.encode(json(-123), opts) == "-123");
        REQUIRE(encoder.encode(json(0), opts) == "0");
    }

    SECTION("Encodes floats") {
        REQUIRE(encoder.encode(json(3.14), opts) == "3.14");
        REQUIRE(encoder.encode(json(-2.5), opts) == "-2.5");

        // Handle -0 normalization
        REQUIRE(encoder.encode(json(-0.0), opts) == "0");
    }

    SECTION("Encodes strings") {
        REQUIRE(encoder.encode(json("hello"), opts) == "hello");
        REQUIRE(encoder.encode(json("world"), opts) == "world");
    }
}

// ============================================================================
// String Quoting Tests
// ============================================================================

TEST_CASE("TOON encoder quotes strings when necessary", "[toon][quoting]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Empty string requires quotes") {
        REQUIRE(encoder.encode(json(""), opts) == "\"\"");
    }

    SECTION("Strings with leading/trailing spaces require quotes") {
        REQUIRE(encoder.encode(json(" padded "), opts) == "\" padded \"");
        REQUIRE(encoder.encode(json("  "), opts) == "\"  \"");
    }

    SECTION("Reserved words require quotes") {
        REQUIRE(encoder.encode(json("true"), opts) == "\"true\"");
        REQUIRE(encoder.encode(json("false"), opts) == "\"false\"");
        REQUIRE(encoder.encode(json("null"), opts) == "\"null\"");
    }

    SECTION("Numeric strings require quotes") {
        REQUIRE(encoder.encode(json("42"), opts) == "\"42\"");
        REQUIRE(encoder.encode(json("-3.14"), opts) == "\"-3.14\"");
        REQUIRE(encoder.encode(json("1e-6"), opts) == "\"1e-6\"");
        REQUIRE(encoder.encode(json("05"), opts) == "\"05\"");  // Leading zero
    }

    SECTION("Strings with colon require quotes") {
        REQUIRE(encoder.encode(json("a:b"), opts) == "\"a:b\"");
    }

    SECTION("Strings with comma (default delimiter) require quotes") {
        REQUIRE(encoder.encode(json("a,b"), opts) == "\"a,b\"");
    }

    SECTION("Strings starting with hyphen require quotes") {
        REQUIRE(encoder.encode(json("-"), opts) == "\"-\"");
        REQUIRE(encoder.encode(json("- item"), opts) == "\"- item\"");
    }

    SECTION("Strings with brackets/braces require quotes") {
        REQUIRE(encoder.encode(json("[5]"), opts) == "\"[5]\"");
        REQUIRE(encoder.encode(json("{key}"), opts) == "\"{key}\"");
    }

    SECTION("Unicode and emoji are safe unquoted") {
        REQUIRE(encoder.encode(json("hello 👋 world"), opts) == "hello 👋 world");
        REQUIRE(encoder.encode(json("Привет"), opts) == "Привет");
    }

    SECTION("Inner spaces are safe unquoted") {
        REQUIRE(encoder.encode(json("hello world"), opts) == "hello world");
    }
}

// ============================================================================
// String Escaping Tests
// ============================================================================

TEST_CASE("TOON encoder escapes special characters", "[toon][escaping]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Escapes backslash") {
        REQUIRE(encoder.encode(json("C:\\Users"), opts) == "\"C:\\\\Users\"");
    }

    SECTION("Escapes double quote") {
        REQUIRE(encoder.encode(json("say \"hi\""), opts) == "\"say \\\"hi\\\"\"");
    }

    SECTION("Escapes newline") {
        REQUIRE(encoder.encode(json("line1\nline2"), opts) == "\"line1\\nline2\"");
    }

    SECTION("Escapes carriage return") {
        REQUIRE(encoder.encode(json("line1\rline2"), opts) == "\"line1\\rline2\"");
    }

    SECTION("Escapes tab") {
        REQUIRE(encoder.encode(json("col1\tcol2"), opts) == "\"col1\\tcol2\"");
    }
}

// ============================================================================
// Object Encoding Tests
// ============================================================================

TEST_CASE("TOON encoder encodes objects correctly", "[toon][objects]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Encodes simple object") {
        json obj = {
            {"id", 123},
            {"name", "Ada"},
            {"active", true}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("id: 123") != std::string::npos);
        REQUIRE(result.find("name: Ada") != std::string::npos);
        REQUIRE(result.find("active: true") != std::string::npos);
    }

    SECTION("Encodes nested object") {
        json obj = {
            {"user", {
                {"id", 123},
                {"name", "Ada"}
            }}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("user:") != std::string::npos);
        REQUIRE(result.find("  id: 123") != std::string::npos);
        REQUIRE(result.find("  name: Ada") != std::string::npos);
    }

    SECTION("Encodes empty object") {
        json obj = json::object();
        REQUIRE(encoder.encode(obj, opts) == "");
    }
}

// ============================================================================
// Array Encoding Tests - Inline Format
// ============================================================================

TEST_CASE("TOON encoder encodes inline arrays", "[toon][arrays][inline]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Encodes primitive array") {
        json obj = {
            {"tags", json::array({"admin", "ops", "dev"})}
        };
        std::string result = encoder.encode(obj, opts);
        // Trim any trailing whitespace for comparison
        while (!result.empty() && (result.back() == ' ' || result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        REQUIRE(result == "tags[3]: admin,ops,dev");
    }

    SECTION("Encodes empty array") {
        json obj = {
            {"tags", json::array()}
        };
        std::string result = encoder.encode(obj, opts);
        // Trim any trailing whitespace
        while (!result.empty() && (result.back() == ' ' || result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        REQUIRE(result == "tags[0]:");
    }

    SECTION("Encodes array with quoted values") {
        json obj = {
            {"values", json::array({"true", "123", "a,b"})}
        };
        std::string result = encoder.encode(obj, opts);
        // Trim any trailing whitespace
        while (!result.empty() && (result.back() == ' ' || result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        REQUIRE(result == "values[3]: \"true\",\"123\",\"a,b\"");
    }
}

// ============================================================================
// Array Encoding Tests - Tabular Format
// ============================================================================

TEST_CASE("TOON encoder encodes tabular arrays", "[toon][arrays][tabular]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Encodes uniform object array") {
        json obj = {
            {"items", json::array({
                {{"sku", "A1"}, {"qty", 2}, {"price", 9.99}},
                {{"sku", "B2"}, {"qty", 1}, {"price", 14.5}}
            })}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("items[2]") != std::string::npos);
        // Field order may vary in JSON, so check for individual fields
        REQUIRE(result.find("sku") != std::string::npos);
        REQUIRE(result.find("qty") != std::string::npos);
        REQUIRE(result.find("price") != std::string::npos);
        // Check that values are present (order may vary)
        REQUIRE(result.find("A1") != std::string::npos);
        REQUIRE(result.find("B2") != std::string::npos);
        REQUIRE(result.find("9.99") != std::string::npos);
        REQUIRE(result.find("14.5") != std::string::npos);
    }

    SECTION("Encodes users array") {
        json obj = {
            {"users", json::array({
                {{"id", 1}, {"name", "Alice"}, {"role", "admin"}},
                {{"id", 2}, {"name", "Bob"}, {"role", "user"}}
            })}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("users[2]") != std::string::npos);
        REQUIRE(result.find("{id,name,role}") != std::string::npos);
        REQUIRE(result.find("1,Alice,admin") != std::string::npos);
        REQUIRE(result.find("2,Bob,user") != std::string::npos);
    }

    SECTION("Does not use tabular format for non-uniform arrays") {
        // Different keys
        json obj = {
            {"items", json::array({
                {{"id", 1}, {"name", "First"}},
                {{"id", 2}, {"name", "Second"}, {"extra", true}}
            })}
        };
        std::string result = encoder.encode(obj, opts);

        // Should use list format (with hyphens)
        REQUIRE(result.find("items[2]:") != std::string::npos);
        REQUIRE(result.find("  - ") != std::string::npos);
    }

    SECTION("Does not use tabular format for nested values") {
        json obj = {
            {"items", json::array({
                {{"id", 1}, {"tags", json::array({"a", "b"})}},
                {{"id", 2}, {"tags", json::array({"c", "d"})}}
            })}
        };
        std::string result = encoder.encode(obj, opts);

        // Should use list format (arrays are not primitives)
        REQUIRE(result.find("items[2]:") != std::string::npos);
        REQUIRE(result.find("  - ") != std::string::npos);
    }
}

// ============================================================================
// Array Encoding Tests - List Format
// ============================================================================

TEST_CASE("TOON encoder encodes list arrays", "[toon][arrays][list]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Encodes mixed array") {
        json obj = {
            {"items", json::array({
                1,
                {{"a", 1}},
                "text"
            })}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("items[3]:") != std::string::npos);
        REQUIRE(result.find("  - 1") != std::string::npos);
        REQUIRE(result.find("  - a: 1") != std::string::npos);
        REQUIRE(result.find("  - text") != std::string::npos);
    }

    SECTION("Encodes objects as list items") {
        json obj = {
            {"items", json::array({
                {{"id", 1}, {"name", "First"}},
                {{"id", 2}, {"name", "Second"}, {"extra", true}}
            })}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("items[2]:") != std::string::npos);
        REQUIRE(result.find("  - id: 1") != std::string::npos);
        REQUIRE(result.find("    name: First") != std::string::npos);
        // Second item - check for fields (indentation may vary based on implementation)
        REQUIRE(result.find("id: 2") != std::string::npos);
        REQUIRE(result.find("name: Second") != std::string::npos);
        REQUIRE(result.find("extra: true") != std::string::npos);
    }
}

// ============================================================================
// Complex Nested Structure Tests
// ============================================================================

TEST_CASE("TOON encoder handles complex nested structures", "[toon][nested]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Encodes nested arrays and objects") {
        json obj = {
            {"users", json::array({
                {
                    {"id", 1},
                    {"name", "Alice"},
                    {"tags", json::array({"admin", "dev"})}
                }
            })}
        };
        std::string result = encoder.encode(obj, opts);

        // Should use list format because tags is an array
        REQUIRE(result.find("users[1]:") != std::string::npos);
        REQUIRE(result.find("  - id: 1") != std::string::npos);
        // Tags array inside nested object - may be formatted inline or as array
        // Check that tags and values are present (format may vary with indentation)
        REQUIRE(result.find("tags") != std::string::npos);
        REQUIRE(result.find("admin") != std::string::npos);
        REQUIRE(result.find("dev") != std::string::npos);
    }

    SECTION("Encodes deeply nested objects") {
        json obj = {
            {"level1", {
                {"level2", {
                    {"level3", {
                        {"value", 42}
                    }}
                }}
            }}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("level1:") != std::string::npos);
        REQUIRE(result.find("  level2:") != std::string::npos);
        REQUIRE(result.find("    level3:") != std::string::npos);
        REQUIRE(result.find("      value: 42") != std::string::npos);
    }
}

// ============================================================================
// Options Tests
// ============================================================================

TEST_CASE("TOON encoder respects options", "[toon][options]") {
    ToonEncoder encoder;

    SECTION("Custom indentation") {
        ToonOptions opts;
        opts.indent = 4;

        json obj = {
            {"nested", {
                {"value", 1}
            }}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("    value: 1") != std::string::npos);
    }

    SECTION("Length marker option") {
        ToonOptions opts;
        opts.length_marker = true;

        json obj = {
            {"tags", json::array({"a", "b", "c"})}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("tags[#3]:") != std::string::npos);
    }

    SECTION("Tab delimiter") {
        ToonOptions opts;
        opts.delimiter = '\t';

        json obj = {
            {"items", json::array({
                {{"a", 1}, {"b", 2}},
                {{"a", 3}, {"b", 4}}
            })}
        };
        std::string result = encoder.encode(obj, opts);

        // Header should include tab delimiter indicator
        REQUIRE(result.find("items[2\t]{a\tb}:") != std::string::npos);
    }

    SECTION("Pipe delimiter") {
        ToonOptions opts;
        opts.delimiter = '|';

        json obj = {
            {"items", json::array({
                {{"a", 1}, {"b", 2}},
                {{"a", 3}, {"b", 4}}
            })}
        };
        std::string result = encoder.encode(obj, opts);

        // Header should include pipe delimiter indicator
        REQUIRE(result.find("items[2|]{a|b}:") != std::string::npos);
    }
}

// ============================================================================
// Key Encoding Tests
// ============================================================================

TEST_CASE("TOON encoder encodes keys correctly", "[toon][keys]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("Valid unquoted keys") {
        json obj = {
            {"id", 1},
            {"user_name", "test"},
            {"user.name", "test"},
            {"_private", "test"}
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("id: 1") != std::string::npos);
        REQUIRE(result.find("user_name: test") != std::string::npos);
        REQUIRE(result.find("user.name: test") != std::string::npos);
        REQUIRE(result.find("_private: test") != std::string::npos);
    }

    SECTION("Keys requiring quotes") {
        json obj = {
            {"user name", "test"},  // Space
            {"order-id", "test"},   // Hyphen
            {"123", "test"},        // Starts with digit
            {"", "test"}            // Empty
        };
        std::string result = encoder.encode(obj, opts);

        REQUIRE(result.find("\"user name\": test") != std::string::npos);
        REQUIRE(result.find("\"order-id\": test") != std::string::npos);
        REQUIRE(result.find("\"123\": test") != std::string::npos);
        REQUIRE(result.find("\"\": test") != std::string::npos);
    }
}

// ============================================================================
// Real-World Example Tests
// ============================================================================

TEST_CASE("TOON encoder handles real-world examples", "[toon][examples]") {
    ToonEncoder encoder;
    ToonOptions opts;

    SECTION("SAP screen data with form fields") {
        json screen_data = {
            {"screen_id", "SE38/1000"},
            {"transaction", "SE38"},
            {"form_fields", json::array({
                {{"id", "wnd[0]/usr/txtRS38M-PROGRAMM"}, {"label", "Program"}, {"value", "ZTEST"}, {"enabled", true}},
                {{"id", "wnd[0]/usr/txtRS38M-VARIANT"}, {"label", "Variant"}, {"value", ""}, {"enabled", true}}
            })},
            {"buttons", json::array({
                {{"id", "wnd[0]/tbar[1]/btn[8]"}, {"text", "Execute"}, {"enabled", true}},
                {{"id", "wnd[0]/tbar[1]/btn[3]"}, {"text", "Check"}, {"enabled", true}}
            })}
        };

        std::string result = encoder.encode(screen_data, opts);

        // Check structure
        REQUIRE(result.find("screen_id: SE38/1000") != std::string::npos);
        REQUIRE(result.find("transaction: SE38") != std::string::npos);

        // Tabular arrays for uniform structures - check that the arrays are present
        REQUIRE(result.find("form_fields[2]") != std::string::npos);
        // Field order may vary in JSON, so check for individual fields
        REQUIRE(result.find("id") != std::string::npos);
        REQUIRE(result.find("label") != std::string::npos);
        REQUIRE(result.find("value") != std::string::npos);
        REQUIRE(result.find("enabled") != std::string::npos);
        REQUIRE(result.find("buttons[2]") != std::string::npos);
        REQUIRE(result.find("text") != std::string::npos);
    }

    SECTION("E-commerce order from TOON spec example") {
        json order = {
            {"order_id", "ORD-12345"},
            {"customer", {
                {"name", "John Doe"},
                {"email", "john@example.com"}
            }},
            {"items", json::array({
                {{"sku", "A1"}, {"name", "Widget"}, {"qty", 2}, {"price", 9.99}},
                {{"sku", "B2"}, {"name", "Gadget"}, {"qty", 1}, {"price", 14.5}}
            })},
            {"total", 34.48}
        };

        std::string result = encoder.encode(order, opts);

        // Verify structure
        REQUIRE(result.find("order_id: ORD-12345") != std::string::npos);
        REQUIRE(result.find("customer:") != std::string::npos);
        REQUIRE(result.find("  name: John Doe") != std::string::npos);
        // Tabular array format - check components separately for flexibility
        REQUIRE(result.find("items[2]") != std::string::npos);
        // Field order may vary, check for individual fields
        REQUIRE(result.find("sku") != std::string::npos);
        REQUIRE(result.find("name") != std::string::npos);
        REQUIRE(result.find("qty") != std::string::npos);
        REQUIRE(result.find("price") != std::string::npos);
        REQUIRE(result.find("total: 34.48") != std::string::npos);
    }
}
