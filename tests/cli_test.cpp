#include "cli.hpp"
#include "error.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <tuple>
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
	                        "--all-varchar",
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
	CHECK(run.all_varchar);
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
	                           "--max-input-files", "9", "--strict-schema", "--all-varchar", "--", "--data.csv"},
	                          [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });

	CHECK(schema.resources.memory_limit == "32MB");
	CHECK(schema.resources.threads == 1);
	CHECK(schema.resources.timeout == std::chrono::seconds(2));
	CHECK(schema.resources.strict_schema);
	CHECK(schema.resources.all_varchar);
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
	CheckError("DUPLICATE_OPTION", []() {
		Parse({"sqrail", "run", "--all-varchar", "--all-varchar", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	});
	CheckError("DUPLICATE_OPTION", []() {
		Parse({"sqrail", "schema", "--all-varchar", "--all-varchar", "data.csv"},
		      [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });
	});
	CheckError("UNKNOWN_OPTION", []() {
		Parse({"sqrail", "schema", "--memroy", "1GB", "data.csv"},
		      [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });
	});
	CheckError("UNKNOWN_OPTION", []() {
		Parse({"sqrail", "schema", "-t", "data=data.csv"},
		      [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); });
	});
	for (const auto &help_flag : {"-h", "--help", "--agent-help"}) {
		CAPTURE(help_flag);
		try {
			static_cast<void>(Parse({"sqrail", "schema", help_flag},
			                        [](const int argc, char **argv) { return sqrail::ParseSchema(argc, argv); }));
			FAIL("expected SqrailError for subcommand help flag");
		} catch (const sqrail::SqrailError &error) {
			CHECK(error.exit_code == sqrail::EXIT_USAGE);
			CHECK(error.code == "UNKNOWN_OPTION");
			CHECK(std::string(error.what()).find("(use 'sqrail --help')") != std::string::npos);
		}
		try {
			static_cast<void>(Parse({"sqrail", "run", help_flag},
			                        [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); }));
			FAIL("expected SqrailError for subcommand help flag");
		} catch (const sqrail::SqrailError &error) {
			CHECK(error.exit_code == sqrail::EXIT_USAGE);
			CHECK(error.code == "UNKNOWN_OPTION");
			CHECK(std::string(error.what()).find("(use 'sqrail --help')") != std::string::npos);
		}
	}
	CheckError("EMPTY_SQL", []() { static_cast<void>(sqrail::NormalizeQuery(" ; \n")); });
	CheckError("EMPTY_SQL", []() { static_cast<void>(sqrail::NormalizeQuery(" ; \r\n")); });

	const auto saved_cin_state = std::cin.rdstate();
	std::cin.setstate(std::ios::badbit);
	try {
		CheckError("MAX_SPILL_REQUIRES_SPILL", []() {
			Parse({"sqrail", "run", "--max-spill", "64MiB", "-"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		});
	} catch (...) {
		std::cin.clear(saved_cin_state);
		throw;
	}
	std::cin.clear(saved_cin_state);
}

TEST_CASE("CRLF query text normalizes like LF", "[cli]") {
	const std::vector<std::pair<std::string, std::string>> cases {
	    {"SELECT 1;\r\n", "SELECT 1"},
	    {"SELECT 1 ;\r\n;\r\n \r\n", "SELECT 1"},
	    {"\r\nSELECT\r\n  1\r\n", "\r\nSELECT\r\n  1"},
	    {"SELECT 1; -- note\r\n", "SELECT 1; -- note"},
	};
	for (std::size_t index = 0; index < cases.size(); ++index) {
		CAPTURE(index);
		CHECK(sqrail::NormalizeQuery(cases[index].first) == cases[index].second);
	}
	CheckError("EMPTY_SQL", []() { static_cast<void>(sqrail::NormalizeQuery("\r\n;\r\n;\r\n")); });
	CheckError("EMPTY_SQL", []() { static_cast<void>(sqrail::NormalizeQuery("\r\n \r\n")); });
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

TEST_CASE("numeric resource limits enforce their documented boundaries", "[cli]") {
	const auto accepted = Parse({"sqrail", "run", "--threads", "1024", "--max-rows", "9223372036854775806",
	                             "--max-input-files", "1000000000", "--timeout", "10080m", "SELECT 1"},
	                            [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(accepted.threads == 1024);
	CHECK(accepted.max_rows == 9223372036854775806ULL);
	CHECK(accepted.max_input_files == 1000000000U);
	CHECK(accepted.timeout == std::chrono::hours(24 * 7));

	const auto hours_timeout = Parse({"sqrail", "run", "--timeout", "1.5h", "SELECT 1"},
	                                 [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(hours_timeout.timeout == std::chrono::minutes(90));

	const auto days_timeout = Parse({"sqrail", "run", "--timeout", "7d", "SELECT 1"},
	                                [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(days_timeout.timeout == std::chrono::hours(24 * 7));

	const std::vector<std::tuple<std::string, std::string, std::string>> rejected {
	    {"--threads", "0", "INVALID_THREADS"},
	    {"--threads", "1025", "INVALID_THREADS"},
	    {"--threads", "-1", "INVALID_THREADS"},
	    {"--max-rows", "9223372036854775807", "INVALID_MAX_ROWS"},
	    {"--max-rows", "18446744073709551616", "INVALID_MAX_ROWS"},
	    {"--max-input-files", "1000000001", "INVALID_MAX_INPUT_FILES"},
	    {"--timeout", "0.999ms", "INVALID_TIMEOUT"},
	    {"--timeout", "10080.001m", "INVALID_TIMEOUT"},
	    {"--timeout", "168.001h", "INVALID_TIMEOUT"},
	    {"--timeout", "7.001d", "INVALID_TIMEOUT"},
	};
	for (const auto &[option, value, code] : rejected) {
		CAPTURE(option, value);
		CheckError(code, [&]() {
			Parse({"sqrail", "run", option, value, "SELECT 1"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		});
	}
}

TEST_CASE("exact byte limits truncate fractions and reject invalid or overflowing values", "[cli]") {
	const std::vector<std::pair<std::string, uint64_t>> accepted {
	    {"1.999999B", 1},      {"1K", 1000},
	    {"0.001KB", 1},        {"64M", 64000000},
	    {"2G", 2000000000ULL}, {"1T", 1000000000000ULL},
	    {"1.234567KiB", 1264}, {"16777215.999999TiB", 18446744073708452104ULL},
	};
	for (const auto &[value, expected] : accepted) {
		CAPTURE(value);
		const auto parsed = Parse({"sqrail", "run", "--max-output-bytes", value, "SELECT 1"},
		                          [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		CHECK(parsed.max_output_bytes == expected);
	}
	for (const auto &value : {"0B", "0.999999B", "0K", "1P", "1.0000001MiB", "16777216TiB"}) {
		CAPTURE(value);
		CheckError("INVALID_MAX_OUTPUT", [&]() {
			Parse({"sqrail", "run", "--max-output-bytes", value, "SELECT 1"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		});
	}
}

TEST_CASE("deterministic ASCII parsers enforce identifier, size, and timeout edge cases", "[cli]") {
	const auto valid_binding = Parse({"sqrail", "run", "-t", "_sales_2=dir/a=b.csv", "--timeout", "0.001s", "SELECT 1"},
	                                 [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	REQUIRE(valid_binding.tables.size() == 1);
	CHECK(valid_binding.tables[0].name == "_sales_2");
	CHECK(valid_binding.tables[0].source == "dir/a=b.csv");
	CHECK(valid_binding.timeout == std::chrono::milliseconds(1));

	const auto exact_timeout = Parse({"sqrail", "run", "--timeout", "1.005s", "SELECT 1"},
	                                 [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(exact_timeout.timeout == std::chrono::milliseconds(1005));

	for (const auto &raw : {"1sales=a.csv", "my-sales=a.csv", "sales$=a.csv"}) {
		CAPTURE(raw);
		CheckError("INVALID_TABLE_NAME", [&]() {
			Parse({"sqrail", "run", "-t", raw, "SELECT 1"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		});
	}
	for (const auto &raw : {"=a.csv", "sales=", "sales"}) {
		CAPTURE(raw);
		CheckError("INVALID_TABLE", [&]() {
			Parse({"sqrail", "run", "-t", raw, "SELECT 1"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		});
	}
	for (const auto &raw : {"0M", "0.000GiB", ".5MB", "128.MB", "128MBx", "100000000000000000000B"}) {
		CAPTURE(raw);
		CheckError("INVALID_MEMORY", [&]() {
			Parse({"sqrail", "run", "--memory", raw, "SELECT 1"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		});
	}
	for (const auto &raw : {"0.000999s", "10080.0000001m", ".5s", "5.s", "+5s"}) {
		CAPTURE(raw);
		CheckError("INVALID_TIMEOUT", [&]() {
			Parse({"sqrail", "run", "--timeout", raw, "SELECT 1"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		});
	}
}

TEST_CASE("output path validation rejects empty paths and dangling symlinks and normalizes parents", "[cli]") {
	CheckError("OUTPUT_DIRECTORY", []() {
		Parse({"sqrail", "run", "-o", "", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	});
	CheckError("SPILL_DIRECTORY", []() {
		Parse({"sqrail", "run", "--spill", "", "SELECT 1"},
		      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	});

	const auto temp_root =
	    std::filesystem::temp_directory_path() /
	    ("sqrail-cli-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(temp_root / "real" / "child");
	struct Cleanup {
		std::filesystem::path root;
		~Cleanup() {
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
		}
	} cleanup {temp_root};

	const auto relative_parent = temp_root / "real" / "child" / ".." / "out.parquet";
	const auto parsed_relative = Parse({"sqrail", "run", "-o", relative_parent.string(), "SELECT 1"},
	                                   [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
	CHECK(parsed_relative.output == std::filesystem::weakly_canonical(temp_root / "real") / "out.parquet");

	std::error_code symlink_error;
	const auto dangling = temp_root / "dangling.parquet";
	std::filesystem::create_symlink(temp_root / "missing-target.parquet", dangling, symlink_error);
	if (!symlink_error) {
		try {
			static_cast<void>(Parse({"sqrail", "run", "-o", dangling.string(), "SELECT 1"},
			                        [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); }));
			FAIL("expected SqrailError for dangling output symlink");
		} catch (const sqrail::SqrailError &error) {
			CHECK(error.exit_code == sqrail::EXIT_OUTPUT);
			CHECK(error.code == "OUTPUT_EXISTS");
		}
	}

	symlink_error.clear();
	const auto symlink_parent = temp_root / "linked-dir";
	std::filesystem::create_directory_symlink(temp_root / "real", symlink_parent, symlink_error);
	if (!symlink_error) {
		const auto parsed_symlink_parent =
		    Parse({"sqrail", "run", "-o", (symlink_parent / "out.parquet").string(), "SELECT 1"},
			      [](const int argc, char **argv) { return sqrail::ParseRun(argc, argv, false); });
		CHECK(parsed_symlink_parent.output == std::filesystem::weakly_canonical(temp_root / "real") / "out.parquet");
	}
}
