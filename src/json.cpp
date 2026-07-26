#include "json.hpp"

#include <cctype>
#include <cstdint>
#include <string_view>

namespace sqrail {
namespace {

void AppendUnicodeEscape(std::string &output, const std::uint32_t codepoint) {
	constexpr char hex[] = "0123456789abcdef";
	output += "\\u";
	for (int shift = 12; shift >= 0; shift -= 4) {
		output.push_back(hex[(codepoint >> static_cast<unsigned int>(shift)) & 0x0FU]);
	}
}

bool IsContinuation(const unsigned char byte) {
	return (byte & 0xC0U) == 0x80U;
}

bool DecodeUtf8(const std::string &input, const std::size_t offset, std::uint32_t &codepoint, std::size_t &length) {
	const auto first = static_cast<unsigned char>(input[offset]);
	const auto remaining = input.size() - offset;

	if (first >= 0xC2U && first <= 0xDFU && remaining >= 2) {
		const auto second = static_cast<unsigned char>(input[offset + 1]);
		if (IsContinuation(second)) {
			codepoint = (static_cast<std::uint32_t>(first & 0x1FU) << 6U) | static_cast<std::uint32_t>(second & 0x3FU);
			length = 2;
			return true;
		}
	}

	if (first >= 0xE0U && first <= 0xEFU && remaining >= 3) {
		const auto second = static_cast<unsigned char>(input[offset + 1]);
		const auto third = static_cast<unsigned char>(input[offset + 2]);
		const bool valid_second =
		    (first == 0xE0U && second >= 0xA0U && second <= 0xBFU) ||
		    (first == 0xEDU && second >= 0x80U && second <= 0x9FU) ||
		    (((first >= 0xE1U && first <= 0xECU) || (first >= 0xEEU && first <= 0xEFU)) && IsContinuation(second));
		if (valid_second && IsContinuation(third)) {
			codepoint = (static_cast<std::uint32_t>(first & 0x0FU) << 12U) |
			            (static_cast<std::uint32_t>(second & 0x3FU) << 6U) | static_cast<std::uint32_t>(third & 0x3FU);
			length = 3;
			return true;
		}
	}

	if (first >= 0xF0U && first <= 0xF4U && remaining >= 4) {
		const auto second = static_cast<unsigned char>(input[offset + 1]);
		const auto third = static_cast<unsigned char>(input[offset + 2]);
		const auto fourth = static_cast<unsigned char>(input[offset + 3]);
		const bool valid_second = (first == 0xF0U && second >= 0x90U && second <= 0xBFU) ||
		                          (first == 0xF4U && second >= 0x80U && second <= 0x8FU) ||
		                          (first >= 0xF1U && first <= 0xF3U && IsContinuation(second));
		if (valid_second && IsContinuation(third) && IsContinuation(fourth)) {
			codepoint = (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
			            (static_cast<std::uint32_t>(second & 0x3FU) << 12U) |
			            (static_cast<std::uint32_t>(third & 0x3FU) << 6U) | static_cast<std::uint32_t>(fourth & 0x3FU);
			length = 4;
			return true;
		}
	}

	return false;
}

bool IsValueBoundary(const char character) {
	return std::isspace(static_cast<unsigned char>(character)) != 0 || character == ':' || character == ',' ||
	       character == '[' || character == ']' || character == '{' || character == '}';
}

bool IsBareTokenAt(const std::string &input, const std::size_t offset, const std::string_view token) {
	if (offset + token.size() > input.size() || input.compare(offset, token.size(), token) != 0) {
		return false;
	}
	const bool valid_left = offset == 0 || IsValueBoundary(input[offset - 1]);
	const auto after = offset + token.size();
	const bool valid_right = after == input.size() || IsValueBoundary(input[after]);
	return valid_left && valid_right;
}

} // namespace

std::string JsonEscape(const std::string &input) {
	std::string output;
	output.reserve(input.size());

	for (std::size_t index = 0; index < input.size();) {
		const auto byte = static_cast<unsigned char>(input[index]);
		if (byte >= 0x80U) {
			std::uint32_t codepoint = 0;
			std::size_t length = 0;
			if (!DecodeUtf8(input, index, codepoint, length)) {
				AppendUnicodeEscape(output, 0xFFFDU);
				++index;
				continue;
			}
			if (codepoint <= 0xFFFFU) {
				AppendUnicodeEscape(output, codepoint);
			} else {
				const auto supplementary = codepoint - 0x10000U;
				AppendUnicodeEscape(output, 0xD800U + (supplementary >> 10U));
				AppendUnicodeEscape(output, 0xDC00U + (supplementary & 0x3FFU));
			}
			index += length;
			continue;
		}

		switch (byte) {
		case '"':
			output += "\\\"";
			break;
		case '\\':
			output += "\\\\";
			break;
		case '\b':
			output += "\\b";
			break;
		case '\f':
			output += "\\f";
			break;
		case '\n':
			output += "\\n";
			break;
		case '\r':
			output += "\\r";
			break;
		case '\t':
			output += "\\t";
			break;
		default:
			if (byte < 0x20) {
				AppendUnicodeEscape(output, byte);
			} else {
				output.push_back(static_cast<char>(byte));
			}
		}
		++index;
	}
	return output;
}

std::string StrictJson(std::string input) {
	if (input.find("NaN") == std::string::npos && input.find("Infinity") == std::string::npos) {
		return input;
	}

	std::string output;
	output.reserve(input.size());
	bool inside_string = false;
	bool escaped = false;

	for (std::size_t index = 0; index < input.size();) {
		const char character = input[index];
		if (inside_string) {
			output.push_back(character);
			if (escaped) {
				escaped = false;
			} else if (character == '\\') {
				escaped = true;
			} else if (character == '"') {
				inside_string = false;
			}
			++index;
			continue;
		}

		if (character == '"') {
			inside_string = true;
			output.push_back(character);
			++index;
			continue;
		}
		if (IsBareTokenAt(input, index, "-Infinity")) {
			output += "null";
			index += std::string_view("-Infinity").size();
			continue;
		}
		if (IsBareTokenAt(input, index, "Infinity")) {
			output += "null";
			index += std::string_view("Infinity").size();
			continue;
		}
		if (IsBareTokenAt(input, index, "NaN")) {
			output += "null";
			index += std::string_view("NaN").size();
			continue;
		}

		output.push_back(character);
		++index;
	}
	return output;
}

} // namespace sqrail
