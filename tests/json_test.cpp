#include "json.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int Fail(const std::string &name, const std::string &expected, const std::string &actual) {
	std::cerr << name << "\nexpected: " << expected << "\nactual:   " << actual << '\n';
	return 1;
}

} // namespace

int main() {
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
		if (actual != cases[index].second) {
			return Fail("strict JSON case " + std::to_string(index), cases[index].second, actual);
		}
		if (sqrail::StrictJson(actual) != actual) {
			return Fail("strict JSON idempotence " + std::to_string(index), actual, sqrail::StrictJson(actual));
		}
	}

	if (sqrail::JsonEscape("a\n\"b\\") != R"(a\n\"b\\)") {
		return Fail("JSON escaping", R"(a\n\"b\\)", sqrail::JsonEscape("a\n\"b\\"));
	}
	if (sqrail::JsonEscape("\xE6\x97\xA5\xF0\x9F\x98\x80") != R"(\u65e5\ud83d\ude00)") {
		return Fail("UTF-8 JSON escaping", R"(\u65e5\ud83d\ude00)", sqrail::JsonEscape("\xE6\x97\xA5\xF0\x9F\x98\x80"));
	}
	if (sqrail::JsonEscape(std::string(1, static_cast<char>(0xFF))) != R"(\ufffd)") {
		return Fail("invalid UTF-8 replacement", R"(\ufffd)",
		            sqrail::JsonEscape(std::string(1, static_cast<char>(0xFF))));
	}
	if (sqrail::JsonEscape("\xC0\xAF") != R"(\ufffd\ufffd)") {
		return Fail("overlong UTF-8 replacement", R"(\ufffd\ufffd)", sqrail::JsonEscape("\xC0\xAF"));
	}
	return 0;
}
