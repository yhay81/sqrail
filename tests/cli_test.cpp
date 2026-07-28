#include "cli.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
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

void CheckError(const std::string &expected_code, const std::function<void()> &operation) {
	bool threw = false;
	try {
		operation();
	} catch (const sqrail::SqrailError &error) {
		threw = true;
		CAPTURE(expected_code, error.code, error.exit_code);
		CHECK(error.code == expected_code);
		CHECK(error.exit_code != 0);
	}
	CHECK(threw);
}

} // namespace

TEST_CASE("run options preserve the agent-facing contract", "[cli]") {
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

	CHECK(run.max_rows == 42);
	CHECK(run.stats);
	CHECK(run.strict_schema);
	CHECK(run.memory_limit == "64MiB");
	CHECK(run.threads == 2);
	CHECK(run.timeout == std::chrono::milliseconds(1500));
	REQUIRE(run.tables.size() == 1);
	CHECK(run.tables[0].name == "Data");
	CHECK(run.tables[0].source == "data.csv");
	CHECK(run.sql == "SELECT 1");
	CHECK(run.max_output_bytes == uint64_t {2} * 1024U * 1024U);
	CHECK(run.max_input_files == 7);
	CHECK(run.max_sql_bytes == uint64_t {8} * 1024U);
}

TEST_CASE("schema options preserve paths after the option terminator", "[cli]") {
	const auto schema = Parse({"sqrail", "schema", "--memory", "32MB", "--threads", "1", "--timeout", "2s",
	                           "--max-input-files", "9", "--strict-schema", "--", "--data.csv"},
	                          [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });

	CHECK(schema.resources.memory_limit == "32MB");
	CHECK(schema.resources.threads == 1);
	CHECK(schema.resources.timeout == std::chrono::seconds(2));
	CHECK(schema.strict_schema);
	REQUIRE(schema.sources.size() == 1);
	CHECK(schema.sources[0] == "--data.csv");
	CHECK(schema.resources.max_input_files == 9);
}

TEST_CASE("invalid and conflicting options have stable error codes", "[cli]") {
	CheckError("DUPLICATE_TABLE", []() {
		Parse({"sqrail", "run", "-t", "Data=a.csv", "-t", "data=b.csv", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	});
	CheckError("INVALID_MAX_ROWS", []() {
		Parse({"sqrail", "run", "--max-rows", "0", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	});
	CheckError("CHECK_STATS", []() {
		Parse({"sqrail", "check", "--stats", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, true); });
	});
	CheckError("CHECK_OUTPUT_LIMIT", []() {
		Parse({"sqrail", "check", "--max-output-bytes", "1MiB", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, true); });
	});
	CheckError("DUPLICATE_OPTION", []() {
		Parse({"sqrail", "schema", "--timeout", "1s", "--timeout", "2s", "data.csv"},
		      [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });
	});
	CheckError("EMPTY_SQL", []() { static_cast<void>(sqrail::NormalizeQuery(" ; \n")); });
}

TEST_CASE("query text and exact resource sizes round-trip", "[cli]") {
	const auto leading_comment = Parse({"sqrail", "run", "--", "-- comment\nSELECT 1"},
	                                   [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(leading_comment.sql == "-- comment\nSELECT 1");

	const auto exact_sizes =
	    Parse({"sqrail", "run", "--max-output-bytes", "1.5KiB", "--max-sql-bytes", "18446744073709551615B", "SELECT 1"},
	          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(exact_sizes.max_output_bytes == 1536);
	CHECK(exact_sizes.max_sql_bytes == std::numeric_limits<uint64_t>::max());

	const auto short_resource_sizes =
	    Parse({"sqrail", "run", "--memory", "128M", "--spill", "spill", "--max-spill", "2G", "SELECT 1"},
	          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(short_resource_sizes.memory_limit == "128M");
	CHECK(short_resource_sizes.max_spill == "2G");
}

TEST_CASE("byte-size parsing rejects overflow and excessive precision", "[cli]") {
	CheckError("INVALID_MAX_OUTPUT", []() {
		Parse({"sqrail", "run", "--max-output-bytes", "18446744073709551616B", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	});
	CheckError("INVALID_MAX_OUTPUT", []() {
		Parse({"sqrail", "run", "--max-output-bytes", "1.0000001MiB", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	});
}

TEST_CASE("bounded numeric options round-trip", "[cli][property]") {
	for (uint64_t value = 1; value <= 128; ++value) {
		const auto raw = std::to_string(value);
		const auto parsed =
		    Parse({"sqrail", "run", "--max-input-files", raw, "--max-output-bytes", raw + "B", "SELECT 1"},
		          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		CAPTURE(value);
		CHECK(parsed.max_input_files == value);
		CHECK(parsed.max_output_bytes == value);
	}
}
