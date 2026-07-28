#include "cli.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <regex>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace fs = std::filesystem;

namespace sqrail {
namespace {

constexpr int EXIT_USAGE = 2;
constexpr int EXIT_OUTPUT = 5;

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

TableBinding ParseBinding(const std::string &raw) {
	const auto separator = raw.find('=');
	if (separator == std::string::npos || separator == 0 || separator + 1 >= raw.size()) {
		throw SqrailError(EXIT_USAGE, "INVALID_TABLE", "table binding must be NAME=PATH: " + raw);
	}

	const std::string name = raw.substr(0, separator);
	static const std::regex identifier("^[A-Za-z_][A-Za-z0-9_]*$");
	if (!std::regex_match(name, identifier)) {
		throw SqrailError(EXIT_USAGE, "INVALID_TABLE_NAME", "table name must match [A-Za-z_][A-Za-z0-9_]*: " + name);
	}
	return {name, raw.substr(separator + 1), {}};
}

std::string ReadStdin(const uint64_t max_bytes) {
	constexpr std::size_t chunk_size = std::size_t {16} * 1024U;
	std::string result;
	char chunk[chunk_size];
	while (std::cin) {
		std::cin.read(chunk, static_cast<std::streamsize>(chunk_size));
		const auto count = std::cin.gcount();
		if (count > 0) {
			const auto size = static_cast<uint64_t>(count);
			if (max_bytes != 0 && (result.size() > max_bytes || size > max_bytes - result.size())) {
				throw SqrailError(EXIT_USAGE, "SQL_LIMIT",
				                  "SQL input exceeded --max-sql-bytes " + std::to_string(max_bytes));
			}
			result.append(chunk, static_cast<std::size_t>(count));
		}
	}
	if (!std::cin.eof()) {
		throw SqrailError(EXIT_USAGE, "STDIN_READ", "cannot read SQL from stdin");
	}
	return result;
}

std::string NormalizeQueryInternal(std::string sql) {
	while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back()))) {
		sql.pop_back();
	}
	while (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
		while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back()))) {
			sql.pop_back();
		}
	}
	if (sql.empty()) {
		throw SqrailError(EXIT_USAGE, "EMPTY_SQL", "SQL query is empty");
	}
	return sql;
}

uint64_t ParseThreads(const std::string &raw) {
	uint64_t value = 0;
	const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
	if (raw.empty() || parsed.ec != std::errc() || parsed.ptr != raw.data() + raw.size()) {
		throw SqrailError(EXIT_USAGE, "INVALID_THREADS", "--threads must be an integer");
	}
	if (value == 0 || value > 1024) {
		throw SqrailError(EXIT_USAGE, "INVALID_THREADS", "--threads must be between 1 and 1024");
	}
	return value;
}

uint64_t ParseMaxRows(const std::string &raw) {
	uint64_t value = 0;
	const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
	if (raw.empty() || parsed.ec != std::errc() || parsed.ptr != raw.data() + raw.size()) {
		throw SqrailError(EXIT_USAGE, "INVALID_MAX_ROWS", "--max-rows must be an integer");
	}
	constexpr uint64_t maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - 1U;
	if (value == 0 || value > maximum) {
		throw SqrailError(EXIT_USAGE, "INVALID_MAX_ROWS", "--max-rows must be between 1 and 9223372036854775806");
	}
	return value;
}

uint64_t ParsePositiveInteger(const std::string &raw, const std::string &option, const std::string &code,
                              const uint64_t maximum) {
	uint64_t value = 0;
	const auto parsed = std::from_chars(raw.data(), raw.data() + raw.size(), value);
	if (raw.empty() || parsed.ec != std::errc() || parsed.ptr != raw.data() + raw.size()) {
		throw SqrailError(EXIT_USAGE, code, option + " must be an integer");
	}
	if (value == 0 || value > maximum) {
		throw SqrailError(EXIT_USAGE, code, option + " must be between 1 and " + std::to_string(maximum));
	}
	return value;
}

std::string ParseSize(const std::string &raw, const std::string &option, const std::string &code) {
	static const std::regex size_pattern("^[0-9]+([.][0-9]+)?(B|K|M|G|T|KB|MB|GB|TB|KiB|MiB|GiB|TiB)$",
	                                     std::regex::icase);
	if (!std::regex_match(raw, size_pattern)) {
		throw SqrailError(EXIT_USAGE, code, option + " must be a size such as 512M, 512MB, or 2GiB");
	}
	try {
		if (std::stod(raw) <= 0) {
			throw SqrailError(EXIT_USAGE, code, option + " must be greater than zero");
		}
	} catch (const std::invalid_argument &) {
		throw SqrailError(EXIT_USAGE, code, option + " must be a size such as 512M, 512MB, or 2GiB");
	} catch (const std::out_of_range &) {
		throw SqrailError(EXIT_USAGE, code, option + " value is too large");
	}
	return raw;
}

uint64_t ParseByteSize(const std::string &raw, const std::string &option, const std::string &code) {
	static const std::regex size_pattern("^([0-9]+)(?:[.]([0-9]{1,6}))?(B|KB|MB|GB|TB|KiB|MiB|GiB|TiB)$",
	                                     std::regex::icase);
	std::smatch match;
	if (!std::regex_match(raw, match, size_pattern)) {
		throw SqrailError(EXIT_USAGE, code, option + " must be a size such as 1MiB or 2.5GB (at most six decimals)");
	}

	uint64_t whole = 0;
	const auto whole_text = match[1].str();
	const auto parsed = std::from_chars(whole_text.data(), whole_text.data() + whole_text.size(), whole);
	if (parsed.ec != std::errc() || parsed.ptr != whole_text.data() + whole_text.size()) {
		throw SqrailError(EXIT_USAGE, code, option + " value is too large");
	}
	const std::string unit = Lower(match[3].str());
	uint64_t multiplier = 1;
	if (unit == "kb") {
		multiplier = 1000U;
	} else if (unit == "mb") {
		multiplier = uint64_t {1000} * 1000U;
	} else if (unit == "gb") {
		multiplier = uint64_t {1000} * 1000U * 1000U;
	} else if (unit == "tb") {
		multiplier = uint64_t {1000} * 1000U * 1000U * 1000U;
	} else if (unit == "kib") {
		multiplier = 1024U;
	} else if (unit == "mib") {
		multiplier = uint64_t {1024} * 1024U;
	} else if (unit == "gib") {
		multiplier = uint64_t {1024} * 1024U * 1024U;
	} else if (unit == "tib") {
		multiplier = uint64_t {1024} * 1024U * 1024U * 1024U;
	}
	constexpr auto maximum = std::numeric_limits<uint64_t>::max();
	if (whole > maximum / multiplier) {
		throw SqrailError(EXIT_USAGE, code, option + " must resolve to between 1 and 18446744073709551615 bytes");
	}
	uint64_t bytes = whole * multiplier;
	const auto fraction_text = match[2].str();
	if (!fraction_text.empty()) {
		uint64_t fraction = 0;
		const auto fraction_parsed =
		    std::from_chars(fraction_text.data(), fraction_text.data() + fraction_text.size(), fraction);
		if (fraction_parsed.ec != std::errc()) {
			throw SqrailError(EXIT_USAGE, code, option + " has an invalid decimal fraction");
		}
		uint64_t scale = 1;
		for (std::size_t index = 0; index < fraction_text.size(); ++index) {
			scale *= 10U;
		}
		const uint64_t fractional_bytes = (fraction * multiplier) / scale;
		if (bytes > maximum - fractional_bytes) {
			throw SqrailError(EXIT_USAGE, code, option + " must resolve to between 1 and 18446744073709551615 bytes");
		}
		bytes += fractional_bytes;
	}
	if (bytes == 0) {
		throw SqrailError(EXIT_USAGE, code, option + " must resolve to at least one byte");
	}
	return bytes;
}

std::chrono::milliseconds ParseTimeout(const std::string &raw) {
	static const std::regex duration_pattern("^([0-9]+([.][0-9]+)?)(ms|s|m)$", std::regex::icase);
	std::smatch match;
	if (!std::regex_match(raw, match, duration_pattern)) {
		throw SqrailError(EXIT_USAGE, "INVALID_TIMEOUT", "--timeout must be a duration such as 250ms, 30s, or 2m");
	}

	double value = 0;
	try {
		value = std::stod(match[1].str());
	} catch (const std::exception &) {
		throw SqrailError(EXIT_USAGE, "INVALID_TIMEOUT", "--timeout must be a finite positive duration");
	}
	const std::string unit = Lower(match[3].str());
	const double multiplier = unit == "ms" ? 1.0 : (unit == "s" ? 1000.0 : 60000.0);
	const double milliseconds = value * multiplier;
	constexpr double maximum = 7.0 * 24.0 * 60.0 * 60.0 * 1000.0;
	if (milliseconds < 1.0 || milliseconds > maximum) {
		throw SqrailError(EXIT_USAGE, "INVALID_TIMEOUT", "--timeout must be between 1ms and 7 days");
	}
	return std::chrono::milliseconds(static_cast<int64_t>(milliseconds));
}

std::string RequireValue(int &index, int argc, char **argv, const std::string &option) {
	if (index + 1 >= argc) {
		throw SqrailError(EXIT_USAGE, "MISSING_VALUE", option + " requires a value");
	}
	++index;
	return argv[index];
}

} // namespace

std::string NormalizeQuery(std::string sql) {
	return NormalizeQueryInternal(std::move(sql));
}

RunOptions ParseRun(int argc, char **argv, const bool check_only) {
	RunOptions options;
	std::vector<std::string> positional;
	std::unordered_set<std::string> table_names;
	bool has_memory = false;
	bool has_max_spill = false;
	bool has_threads = false;
	bool has_spill = false;
	bool has_timeout = false;
	bool has_max_rows = false;
	bool has_max_output_bytes = false;
	bool has_max_input_files = false;
	bool has_max_sql_bytes = false;
	bool has_stats = false;
	bool has_strict_schema = false;
	bool positional_only = false;

	for (int index = 2; index < argc; ++index) {
		const std::string argument = argv[index];
		if (!positional_only && argument == "--") {
			positional_only = true;
		} else if (!positional_only && (argument == "-t" || argument == "--table")) {
			auto table = ParseBinding(RequireValue(index, argc, argv, argument));
			if (!table_names.insert(Lower(table.name)).second) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_TABLE", "table name is bound more than once: " + table.name);
			}
			options.tables.push_back(std::move(table));
		} else if (!positional_only && (argument == "-o" || argument == "--output")) {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_OUTPUT", "check does not accept --output");
			}
			if (options.has_output) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--output may only be specified once");
			}
			options.output = fs::absolute(RequireValue(index, argc, argv, argument));
			options.has_output = true;
		} else if (!positional_only && argument == "--memory") {
			if (has_memory) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--memory may only be specified once");
			}
			options.memory_limit = ParseSize(RequireValue(index, argc, argv, argument), "--memory", "INVALID_MEMORY");
			has_memory = true;
		} else if (!positional_only && argument == "--threads") {
			if (has_threads) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--threads may only be specified once");
			}
			options.threads = ParseThreads(RequireValue(index, argc, argv, argument));
			has_threads = true;
		} else if (!positional_only && argument == "--spill") {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_SPILL", "check does not accept --spill");
			}
			if (has_spill) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--spill may only be specified once");
			}
			options.spill_directory = fs::absolute(RequireValue(index, argc, argv, argument));
			has_spill = true;
		} else if (!positional_only && argument == "--max-spill") {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_SPILL", "check does not accept --max-spill");
			}
			if (has_max_spill) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--max-spill may only be specified once");
			}
			options.max_spill =
			    ParseSize(RequireValue(index, argc, argv, argument), "--max-spill", "INVALID_MAX_SPILL");
			has_max_spill = true;
		} else if (!positional_only && argument == "--timeout") {
			if (has_timeout) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--timeout may only be specified once");
			}
			options.timeout = ParseTimeout(RequireValue(index, argc, argv, argument));
			has_timeout = true;
		} else if (!positional_only && argument == "--max-rows") {
			if (has_max_rows) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--max-rows may only be specified once");
			}
			options.max_rows = ParseMaxRows(RequireValue(index, argc, argv, argument));
			has_max_rows = true;
		} else if (!positional_only && argument == "--max-output-bytes") {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_OUTPUT_LIMIT", "check does not accept --max-output-bytes");
			}
			if (has_max_output_bytes) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--max-output-bytes may only be specified once");
			}
			options.max_output_bytes =
			    ParseByteSize(RequireValue(index, argc, argv, argument), "--max-output-bytes", "INVALID_MAX_OUTPUT");
			has_max_output_bytes = true;
		} else if (!positional_only && argument == "--max-input-files") {
			if (has_max_input_files) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--max-input-files may only be specified once");
			}
			options.max_input_files = ParsePositiveInteger(RequireValue(index, argc, argv, argument),
			                                               "--max-input-files", "INVALID_MAX_INPUT_FILES", 1000000000U);
			has_max_input_files = true;
		} else if (!positional_only && argument == "--max-sql-bytes") {
			if (has_max_sql_bytes) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--max-sql-bytes may only be specified once");
			}
			options.max_sql_bytes =
			    ParseByteSize(RequireValue(index, argc, argv, argument), "--max-sql-bytes", "INVALID_MAX_SQL");
			has_max_sql_bytes = true;
		} else if (!positional_only && argument == "--stats") {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_STATS", "check does not accept --stats");
			}
			if (has_stats) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--stats may only be specified once");
			}
			options.stats = true;
			has_stats = true;
		} else if (!positional_only && argument == "--strict-schema") {
			if (has_strict_schema) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--strict-schema may only be specified once");
			}
			options.strict_schema = true;
			has_strict_schema = true;
		} else if (positional_only || argument.empty() || argument == "-" || argument.front() != '-') {
			positional.push_back(argument);
		} else {
			throw SqrailError(EXIT_USAGE, "UNKNOWN_OPTION", "unknown option: " + argument);
		}
	}

	if (positional.size() != 1) {
		throw SqrailError(EXIT_USAGE, "SQL_ARGUMENT",
		                  std::string(check_only ? "check" : "run") +
		                      " expects exactly one SQL argument or '-' for stdin");
	}
	options.sql = positional.front() == "-" ? ReadStdin(options.max_sql_bytes) : positional.front();
	if (options.max_sql_bytes != 0 && options.sql.size() > options.max_sql_bytes) {
		throw SqrailError(EXIT_USAGE, "SQL_LIMIT",
		                  "SQL input exceeded --max-sql-bytes " + std::to_string(options.max_sql_bytes));
	}
	options.sql = NormalizeQuery(std::move(options.sql));

	if (has_max_spill && !has_spill) {
		throw SqrailError(EXIT_USAGE, "MAX_SPILL_REQUIRES_SPILL", "--max-spill requires --spill DIR");
	}

	if (options.has_output) {
		std::error_code error;
		if (fs::exists(options.output, error)) {
			throw SqrailError(EXIT_OUTPUT, "OUTPUT_EXISTS", "output already exists: " + options.output.string());
		}
		const fs::path parent = options.output.parent_path();
		if (!parent.empty() && !fs::is_directory(parent)) {
			throw SqrailError(EXIT_OUTPUT, "OUTPUT_DIRECTORY", "output directory does not exist: " + parent.string());
		}
	}

	return options;
}

SchemaOptions ParseSchema(int argc, char **argv) {
	SchemaOptions options;
	bool positional_only = false;
	bool has_memory = false;
	bool has_threads = false;
	bool has_timeout = false;
	bool has_max_input_files = false;
	bool has_strict_schema = false;

	for (int index = 2; index < argc; ++index) {
		const std::string argument = argv[index];
		if (!positional_only && argument == "--") {
			positional_only = true;
		} else if (!positional_only && argument == "--memory") {
			if (has_memory) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--memory may only be specified once");
			}
			options.resources.memory_limit =
			    ParseSize(RequireValue(index, argc, argv, argument), "--memory", "INVALID_MEMORY");
			has_memory = true;
		} else if (!positional_only && argument == "--threads") {
			if (has_threads) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--threads may only be specified once");
			}
			options.resources.threads = ParseThreads(RequireValue(index, argc, argv, argument));
			has_threads = true;
		} else if (!positional_only && argument == "--timeout") {
			if (has_timeout) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--timeout may only be specified once");
			}
			options.resources.timeout = ParseTimeout(RequireValue(index, argc, argv, argument));
			has_timeout = true;
		} else if (!positional_only && argument == "--max-input-files") {
			if (has_max_input_files) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--max-input-files may only be specified once");
			}
			options.resources.max_input_files = ParsePositiveInteger(
			    RequireValue(index, argc, argv, argument), "--max-input-files", "INVALID_MAX_INPUT_FILES", 1000000000U);
			has_max_input_files = true;
		} else if (!positional_only && argument == "--strict-schema") {
			if (has_strict_schema) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--strict-schema may only be specified once");
			}
			options.strict_schema = true;
			has_strict_schema = true;
		} else {
			options.sources.push_back(argument);
		}
	}
	if (options.sources.empty()) {
		throw SqrailError(EXIT_USAGE, "SCHEMA_ARGUMENT", "schema expects at least one input file");
	}
	return options;
}

} // namespace sqrail
