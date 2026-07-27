#include "cli.hpp"

#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

template <class PARSE>
auto Parse(std::vector<std::string> arguments, PARSE &&parse) {
	std::vector<char *> argv;
	argv.reserve(arguments.size());
	for (auto &argument : arguments) {
		argv.push_back(argument.data());
	}
	return parse(static_cast<int>(argv.size()), argv.data());
}

int Fail(const std::string &name, const std::string &message) {
	std::cerr << name << ": " << message << '\n';
	return 1;
}

bool ExpectError(const std::string &code, const std::function<void()> &operation) {
	try {
		operation();
	} catch (const sqrail::SqrailError &error) {
		return error.code == code && error.exit_code != 0;
	}
	return false;
}

} // namespace

int main() {
	const auto run = Parse({"sqrail",
	                        "run",
	                        "--max-rows",
	                        "42",
	                        "--max-output-bytes",
	                        "2MiB",
	                        "--max-input-files",
	                        "7",
	                        "--max-sql-bytes",
	                        "8KiB",
	                        "--stats",
	                        "--strict-schema",
	                        "--memory",
	                        "64MiB",
	                        "--threads",
	                        "2",
	                        "--timeout",
	                        "1.5s",
	                        "-t",
	                        "Data=data.csv",
	                        "SELECT 1;"},
	                       [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	if (run.max_rows != 42 || !run.stats || !run.strict_schema || run.memory_limit != "64MiB" || run.threads != 2 ||
	    run.timeout != std::chrono::milliseconds(1500) || run.tables.size() != 1 || run.tables[0].name != "Data" ||
	    run.tables[0].source != "data.csv" || run.sql != "SELECT 1" ||
	    run.max_output_bytes != uint64_t {2} * 1024U * 1024U || run.max_input_files != 7 ||
	    run.max_sql_bytes != uint64_t {8} * 1024U) {
		return Fail("valid run options", "parsed values differ");
	}

	const auto schema = Parse({"sqrail", "schema", "--memory", "32MB", "--threads", "1", "--timeout", "2s",
	                           "--max-input-files", "9", "--strict-schema", "--", "--data.csv"},
	                          [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });
	if (schema.resources.memory_limit != "32MB" || schema.resources.threads != 1 ||
	    schema.resources.timeout != std::chrono::seconds(2) || !schema.strict_schema || schema.sources.size() != 1 ||
	    schema.sources[0] != "--data.csv" || schema.resources.max_input_files != 9) {
		return Fail("valid schema options", "parsed values differ");
	}

	if (!ExpectError("DUPLICATE_TABLE", []() {
		    Parse({"sqrail", "run", "-t", "Data=a.csv", "-t", "data=b.csv", "SELECT 1"},
		          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	    })) {
		return Fail("duplicate table", "expected DUPLICATE_TABLE");
	}
	if (!ExpectError("INVALID_MAX_ROWS", []() {
		    Parse({"sqrail", "run", "--max-rows", "0", "SELECT 1"},
		          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	    })) {
		return Fail("invalid max rows", "expected INVALID_MAX_ROWS");
	}
	if (!ExpectError("CHECK_STATS", []() {
		    Parse({"sqrail", "check", "--stats", "SELECT 1"},
		          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, true); });
	    })) {
		return Fail("check stats", "expected CHECK_STATS");
	}
	if (!ExpectError("CHECK_OUTPUT_LIMIT", []() {
		    Parse({"sqrail", "check", "--max-output-bytes", "1MiB", "SELECT 1"},
		          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, true); });
	    })) {
		return Fail("check output limit", "expected CHECK_OUTPUT_LIMIT");
	}
	if (!ExpectError("DUPLICATE_OPTION", []() {
		    Parse({"sqrail", "schema", "--timeout", "1s", "--timeout", "2s", "data.csv"},
		          [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });
	    })) {
		return Fail("duplicate schema option", "expected DUPLICATE_OPTION");
	}
	if (!ExpectError("EMPTY_SQL", []() { static_cast<void>(sqrail::NormalizeQuery(" ; \n")); })) {
		return Fail("empty SQL", "expected EMPTY_SQL");
	}
	const auto leading_comment = Parse({"sqrail", "run", "--", "-- comment\nSELECT 1"},
	                                   [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	if (leading_comment.sql != "-- comment\nSELECT 1") {
		return Fail("option terminator", "did not preserve SQL beginning with --");
	}
	const auto exact_sizes =
	    Parse({"sqrail", "run", "--max-output-bytes", "1.5KiB", "--max-sql-bytes", "18446744073709551615B", "SELECT 1"},
	          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	if (exact_sizes.max_output_bytes != 1536 || exact_sizes.max_sql_bytes != std::numeric_limits<uint64_t>::max()) {
		return Fail("exact byte sizes", "decimal or maximum byte value did not parse exactly");
	}
	const auto short_resource_sizes =
	    Parse({"sqrail", "run", "--memory", "128M", "--spill", "spill", "--max-spill", "2G", "SELECT 1"},
	          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	if (short_resource_sizes.memory_limit != "128M" || short_resource_sizes.max_spill != "2G") {
		return Fail("short resource sizes", "common K/M/G/T suffixes did not parse");
	}
	if (!ExpectError("INVALID_MAX_OUTPUT",
	                 []() {
		                 Parse({"sqrail", "run", "--max-output-bytes", "18446744073709551616B", "SELECT 1"},
		                       [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	                 }) ||
	    !ExpectError("INVALID_MAX_OUTPUT", []() {
		    Parse({"sqrail", "run", "--max-output-bytes", "1.0000001MiB", "SELECT 1"},
		          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	    })) {
		return Fail("invalid byte sizes", "overflow or excessive precision was accepted");
	}

	for (uint64_t value = 1; value <= 128; ++value) {
		const auto raw = std::to_string(value);
		const auto parsed =
		    Parse({"sqrail", "run", "--max-input-files", raw, "--max-output-bytes", raw + "B", "SELECT 1"},
		          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		if (parsed.max_input_files != value || parsed.max_output_bytes != value) {
			return Fail("bounded option property", "positive integer or byte size did not round-trip");
		}
	}
	return 0;
}
