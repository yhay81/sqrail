#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace sqrail {

// Exit codes are part of the stable contract in docs/CONTRACT.md.
constexpr int EXIT_USAGE = 2;
constexpr int EXIT_INPUT = 3;
constexpr int EXIT_QUERY = 4;
constexpr int EXIT_OUTPUT = 5;
constexpr int EXIT_INTERNAL = 70;

struct SqrailError final : std::runtime_error {
	SqrailError(int exit_code_p, std::string code_p, const std::string &message)
	    : std::runtime_error(message), exit_code(exit_code_p), code(std::move(code_p)) {
	}

	int exit_code;
	std::string code;
};

} // namespace sqrail
