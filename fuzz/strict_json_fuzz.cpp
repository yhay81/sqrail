#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, const std::size_t size) {
	std::string input(reinterpret_cast<const char *>(data), size);
	const std::string escaped = sqrail::JsonEscape(input);
	if (!sqrail::IsValidUtf8(escaped)) {
		std::abort();
	}
	for (std::size_t index = 0; index < escaped.size(); ++index) {
		const auto byte = static_cast<unsigned char>(escaped[index]);
		if (byte < 0x20U) {
			std::abort();
		}
		if (escaped[index] == '\\') {
			if (index + 1 >= escaped.size()) {
				std::abort();
			}
			++index;
		} else if (escaped[index] == '"') {
			std::abort();
		}
	}
	const std::string structured =
	    "{\"text\":\"" + escaped + "\",\"values\":[NaN,Infinity,-Infinity],\"again\":\"" + escaped + "\"}";
	const std::string expected =
	    "{\"text\":\"" + escaped + "\",\"values\":[null,null,null],\"again\":\"" + escaped + "\"}";
	if (sqrail::StrictJson(structured) != expected) {
		std::abort();
	}

	const auto once = sqrail::StrictJson(std::move(input));
	const auto twice = sqrail::StrictJson(once);
	if (once != twice) {
		std::abort();
	}
	return 0;
}
