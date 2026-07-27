#include "cli.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

template <class PARSE>
void Parse(std::vector<std::string> arguments, PARSE &&parse) {
	std::vector<char *> argv;
	argv.reserve(arguments.size());
	for (auto &argument : arguments) {
		argv.push_back(argument.data());
	}
	try {
		static_cast<void>(parse(static_cast<int>(argv.size()), argv.data()));
	} catch (const std::exception &) {
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, const std::size_t size) {
	if (size == 0) {
		return 0;
	}

	const auto mode = data[0] % 3U;
	std::vector<std::string> arguments {"sqrail", mode == 0 ? "run" : (mode == 1 ? "check" : "schema")};
	std::string current;
	for (std::size_t index = 1; index < size && arguments.size() < 32; ++index) {
		const char value = static_cast<char>(data[index]);
		if (value == '\0' || value == '\n') {
			if (current == "-" || current == "-o" || current == "--output") {
				current = "_";
			}
			arguments.push_back(current);
			current.clear();
		} else if (current.size() < 512) {
			current.push_back(value);
		}
	}
	if (!current.empty() && arguments.size() < 32) {
		if (current == "-" || current == "-o" || current == "--output") {
			current = "_";
		}
		arguments.push_back(std::move(current));
	}

	if (mode == 2) {
		Parse(std::move(arguments), [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });
	} else {
		Parse(std::move(arguments),
		      [mode](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, mode == 1); });
	}
	return 0;
}
