#include "json.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
	const std::vector<std::string> expected {
	    R"(\u0000)", R"(\u0001)", R"(\u0002)", R"(\u0003)", R"(\u0004)", R"(\u0005)", R"(\u0006)", R"(\u0007)",
	    R"(\b)",     R"(\t)",     R"(\n)",     R"(\u000b)", R"(\f)",     R"(\r)",     R"(\u000e)", R"(\u000f)",
	    R"(\u0010)", R"(\u0011)", R"(\u0012)", R"(\u0013)", R"(\u0014)", R"(\u0015)", R"(\u0016)", R"(\u0017)",
	    R"(\u0018)", R"(\u0019)", R"(\u001a)", R"(\u001b)", R"(\u001c)", R"(\u001d)", R"(\u001e)", R"(\u001f)",
	};
	REQUIRE(expected.size() == 0x20);

	for (std::size_t byte = 0; byte < expected.size(); ++byte) {
		CAPTURE(byte);
		CHECK(sqrail::JsonEscape(std::string(1, static_cast<char>(byte))) == expected[byte]);
	}
}

TEST_CASE("JSON escaping replaces each malformed UTF-8 byte with U+FFFD", "[json]") {
	// A byte that does not begin a complete, valid sequence becomes U+FFFD and
	// decoding resumes at the next byte, so a truncated sequence yields one
	// replacement per byte.
	const std::vector<std::pair<std::string, std::string>> cases {
	    {"\x80", R"(\ufffd)"},
	    {"\xBF", R"(\ufffd)"},
	    {"\x80\xBF", R"(\ufffd\ufffd)"},
	    {"\xC2", R"(\ufffd)"},
	    {"\xDF", R"(\ufffd)"},
	    {"\xE0\xA0", R"(\ufffd\ufffd)"},
	    {"\xE2\x82", R"(\ufffd\ufffd)"},
	    {"\xEF", R"(\ufffd)"},
	    {"\xF0\x9F\x98", R"(\ufffd\ufffd\ufffd)"},
	    {"\xF4\x8F", R"(\ufffd\ufffd)"},
	    {"\xF1", R"(\ufffd)"},
	    {"a\xC2z", R"(a\ufffdz)"},
	    {"\xE2\x82Z", R"(\ufffd\ufffdZ)"},
	    {"\xF0\x9F\x98\xC3\xA9", R"(\ufffd\ufffd\ufffd\u00e9)"},
	};

	for (std::size_t index = 0; index < cases.size(); ++index) {
		const auto actual = sqrail::JsonEscape(cases[index].first);
		CAPTURE(index, cases[index].second);
		CHECK(actual == cases[index].second);
		CHECK(std::all_of(actual.begin(), actual.end(),
		                  [](const char character) { return character >= 0x20 && character < 0x7F; }));
	}
}
