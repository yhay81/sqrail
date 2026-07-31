#include "json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("strict JSON replaces non-finite numeric tokens", "[json]") {
	const std::vector<std::pair<std::string, std::string>> cases {
	    {R"({"value":NaN})", R"({"value":null})"},
	    {R"({"value":Infinity})", R"({"value":null})"},
	    {R"({"value":-Infinity})", R"({"value":null})"},
	    {R"({"values":[NaN,Infinity,-Infinity,1.5]})", R"({"values":[null,null,null,1.5]})"},
	    {R"({"nested":{"value":NaN}})", R"({"nested":{"value":null}})"},
	    {R"({"text":"NaN Infinity -Infinity"})", R"({"text":"NaN Infinity -Infinity"})"},
	    {R"({"text":"escaped quote: \"NaN\""})", R"({"text":"escaped quote: \"NaN\""})"},
	    {R"({"word":"NotNaN","value":1})", R"({"word":"NotNaN","value":1})"},
	    {R"([NaN])", R"([null])"},
	    {"NaN", "null"},
	    {"Infinity", "null"},
	    {"-Infinity", "null"},
	};

	for (std::size_t index = 0; index < cases.size(); ++index) {
		const auto actual = sqrail::StrictJson(cases[index].first);
		CAPTURE(index, cases[index].first);
		CHECK(actual == cases[index].second);
		CHECK(sqrail::StrictJson(actual) == actual);
	}
}

TEST_CASE("JSON escaping emits portable Unicode and replacements", "[json]") {
	CHECK(sqrail::JsonEscape("a\n\"b\\") == R"(a\n\"b\\)");
	CHECK(sqrail::JsonEscape("\xE6\x97\xA5\xF0\x9F\x98\x80") == R"(\u65e5\ud83d\ude00)");
	CHECK(sqrail::JsonEscape(std::string(1, static_cast<char>(0xFF))) == R"(\ufffd)");
	CHECK(sqrail::JsonEscape("\xC0\xAF") == R"(\ufffd\ufffd)");
}

TEST_CASE("JSON escaping covers every ASCII control byte", "[json]") {
	// Short escapes for the five controls named by the JSON RFC.
	const std::vector<std::pair<char, std::string>> short_escapes {
	    {'\b', "\\b"}, {'\t', "\\t"}, {'\n', "\\n"}, {'\f', "\\f"}, {'\r', "\\r"},
	};
	for (const auto &entry : short_escapes) {
		CAPTURE(static_cast<int>(static_cast<unsigned char>(entry.first)));
		CHECK(sqrail::JsonEscape(std::string(1, entry.first)) == entry.second);
	}

	// Remaining 0x00–0x1F controls use \u00xx form.
	for (unsigned byte = 0; byte <= 0x1FU; ++byte) {
		if (byte == '\b' || byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r') {
			continue;
		}
		char hex[16];
		std::snprintf(hex, sizeof(hex), "\\u%04x", byte);
		const auto escaped = sqrail::JsonEscape(std::string(1, static_cast<char>(byte)));
		CAPTURE(byte, escaped);
		CHECK(escaped == hex);
		// Output must remain valid as a JSON string body (no raw controls).
		CHECK(escaped.find(static_cast<char>(byte)) == std::string::npos);
	}
}

TEST_CASE("JSON escaping replaces truncated UTF-8 and isolated continuations", "[json]") {
	const std::vector<std::pair<std::string, std::string>> cases {
	    // Truncated 2-byte sequence (lead only)
	    {std::string("\xC2", 1), R"(\ufffd)"},
	    // Truncated 3-byte sequences
	    {std::string("\xE2", 1), R"(\ufffd)"},
	    {std::string("\xE2\x82", 2), R"(\ufffd\ufffd)"},
	    // Truncated 4-byte sequences
	    {std::string("\xF0", 1), R"(\ufffd)"},
	    {std::string("\xF0\x9F", 2), R"(\ufffd\ufffd)"},
	    {std::string("\xF0\x9F\x98", 3), R"(\ufffd\ufffd\ufffd)"},
	    // Isolated continuation bytes
	    {std::string("\x80", 1), R"(\ufffd)"},
	    {std::string("\xBF", 1), R"(\ufffd)"},
	    // Invalid leads
	    {std::string("\xC0", 1), R"(\ufffd)"},
	    {std::string("\xF5", 1), R"(\ufffd)"},
	};
	for (std::size_t index = 0; index < cases.size(); ++index) {
		const auto actual = sqrail::JsonEscape(cases[index].first);
		CAPTURE(index);
		CHECK(actual == cases[index].second);
		CHECK(actual.find(R"(\ufffd)") != std::string::npos);
	}
}
