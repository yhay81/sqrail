#include "json.hpp"

#include <catch2/catch_test_macros.hpp>

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
