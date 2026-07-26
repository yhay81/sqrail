#include "duckdb.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

struct TableBinding {
	std::string name;
	fs::path path;
};

struct RunOptions {
	std::vector<TableBinding> tables;
	std::string sql;
	fs::path output;
	fs::path spill_directory;
	std::string memory_limit;
	uint64_t threads = 0;
	bool has_output = false;
};

std::string JsonEscape(const std::string &input) {
	std::ostringstream out;
	for (const unsigned char byte : input) {
		switch (byte) {
		case '"':
			out << "\\\"";
			break;
		case '\\':
			out << "\\\\";
			break;
		case '\b':
			out << "\\b";
			break;
		case '\f':
			out << "\\f";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			if (byte < 0x20) {
				constexpr char hex[] = "0123456789abcdef";
				out << "\\u00" << hex[(byte >> 4U) & 0x0FU] << hex[byte & 0x0FU];
			} else {
				out << static_cast<char>(byte);
			}
		}
	}
	return out.str();
}

std::string SqlString(const std::string &input) {
	std::string escaped;
	escaped.reserve(input.size() + 2);
	escaped.push_back('\'');
	for (const char ch : input) {
		escaped.push_back(ch);
		if (ch == '\'') {
			escaped.push_back('\'');
		}
	}
	escaped.push_back('\'');
	return escaped;
}

std::string SqlIdentifier(const std::string &input) {
	std::string escaped;
	escaped.reserve(input.size() + 2);
	escaped.push_back('"');
	for (const char ch : input) {
		escaped.push_back(ch);
		if (ch == '"') {
			escaped.push_back('"');
		}
	}
	escaped.push_back('"');
	return escaped;
}

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

bool EndsWith(const std::string &value, const std::string &suffix) {
	return value.size() >= suffix.size() &&
	       value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string StripCompressionSuffix(std::string path) {
	for (const std::string &suffix : {".gz", ".zst", ".bz2", ".xz"}) {
		if (EndsWith(path, suffix)) {
			path.resize(path.size() - suffix.size());
			break;
		}
	}
	return path;
}

std::string ReaderExpression(const fs::path &path) {
	const std::string quoted = SqlString(path.string());
	const std::string uncompressed = StripCompressionSuffix(Lower(path.filename().string()));

	if (EndsWith(uncompressed, ".parquet")) {
		return "read_parquet(" + quoted + ")";
	}
	if (EndsWith(uncompressed, ".json") || EndsWith(uncompressed, ".jsonl") ||
	    EndsWith(uncompressed, ".ndjson")) {
		return "read_json_auto(" + quoted + ")";
	}
	if (EndsWith(uncompressed, ".tsv") || EndsWith(uncompressed, ".tab")) {
		return "read_csv_auto(" + quoted + ", delim='	')";
	}
	if (EndsWith(uncompressed, ".csv")) {
		return "read_csv_auto(" + quoted + ")";
	}

	throw SqrailError(EXIT_INPUT, "UNSUPPORTED_FORMAT",
	                  "unsupported input format: " + path.string() +
	                      " (expected csv, tsv, json, jsonl, ndjson, or parquet)");
}

std::string CopyOptionsFor(const fs::path &path) {
	const std::string extension = Lower(path.extension().string());
	if (extension == ".parquet") {
		return "FORMAT PARQUET";
	}
	if (extension == ".csv") {
		return "FORMAT CSV, HEADER true";
	}
	if (extension == ".tsv" || extension == ".tab") {
		return "FORMAT CSV, HEADER true, DELIMITER '\t'";
	}
	if (extension == ".json") {
		return "FORMAT JSON, ARRAY true";
	}
	if (extension == ".jsonl" || extension == ".ndjson") {
		return "FORMAT JSON, ARRAY false";
	}
	throw SqrailError(EXIT_OUTPUT, "UNSUPPORTED_OUTPUT",
	                  "unsupported output extension: " + path.string() +
	                      " (expected csv, tsv, json, jsonl, ndjson, or parquet)");
}

fs::path ExistingInput(const std::string &raw_path) {
	std::error_code error;
	fs::path path = fs::absolute(fs::path(raw_path), error);
	if (error || !fs::exists(path) || !fs::is_regular_file(path)) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND", "input file not found: " + raw_path);
	}
	return fs::weakly_canonical(path);
}

TableBinding ParseBinding(const std::string &raw) {
	const auto separator = raw.find('=');
	if (separator == std::string::npos || separator == 0 || separator + 1 >= raw.size()) {
		throw SqrailError(EXIT_USAGE, "INVALID_TABLE",
		                  "table binding must be NAME=FILE: " + raw);
	}

	const std::string name = raw.substr(0, separator);
	static const std::regex identifier("^[A-Za-z_][A-Za-z0-9_]*$");
	if (!std::regex_match(name, identifier)) {
		throw SqrailError(EXIT_USAGE, "INVALID_TABLE_NAME",
		                  "table name must match [A-Za-z_][A-Za-z0-9_]*: " + name);
	}
	return {name, ExistingInput(raw.substr(separator + 1))};
}

std::string ReadStdin() {
	std::ostringstream buffer;
	buffer << std::cin.rdbuf();
	return buffer.str();
}

std::string NormalizeQuery(std::string sql) {
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
	static const std::regex integer("^[0-9]+$");
	if (!std::regex_match(raw, integer)) {
		throw SqrailError(EXIT_USAGE, "INVALID_THREADS", "--threads must be an integer");
	}
	const auto value = std::stoull(raw);
	if (value == 0 || value > 1024) {
		throw SqrailError(EXIT_USAGE, "INVALID_THREADS", "--threads must be between 1 and 1024");
	}
	return value;
}

std::string ParseMemory(const std::string &raw) {
	static const std::regex size_pattern("^[0-9]+([.][0-9]+)?(B|KB|MB|GB|TB|KiB|MiB|GiB|TiB)$",
	                                     std::regex::icase);
	if (!std::regex_match(raw, size_pattern)) {
		throw SqrailError(EXIT_USAGE, "INVALID_MEMORY",
		                  "--memory must be a size such as 512MB or 2GB");
	}
	return raw;
}

std::string RequireValue(int &index, int argc, char **argv, const std::string &option) {
	if (index + 1 >= argc) {
		throw SqrailError(EXIT_USAGE, "MISSING_VALUE", option + " requires a value");
	}
	++index;
	return argv[index];
}

RunOptions ParseRun(int argc, char **argv) {
	RunOptions options;
	std::vector<std::string> positional;

	for (int index = 2; index < argc; ++index) {
		const std::string argument = argv[index];
		if (argument == "-t" || argument == "--table") {
			options.tables.push_back(ParseBinding(RequireValue(index, argc, argv, argument)));
		} else if (argument == "-o" || argument == "--output") {
			options.output = fs::absolute(RequireValue(index, argc, argv, argument));
			options.has_output = true;
		} else if (argument == "--memory") {
			options.memory_limit = ParseMemory(RequireValue(index, argc, argv, argument));
		} else if (argument == "--threads") {
			options.threads = ParseThreads(RequireValue(index, argc, argv, argument));
		} else if (argument == "--spill") {
			options.spill_directory = fs::absolute(RequireValue(index, argc, argv, argument));
		} else if (argument == "-") {
			positional.push_back(argument);
		} else if (!argument.empty() && argument.front() == '-') {
			throw SqrailError(EXIT_USAGE, "UNKNOWN_OPTION", "unknown option: " + argument);
		} else {
			positional.push_back(argument);
		}
	}

	if (positional.size() != 1) {
		throw SqrailError(EXIT_USAGE, "SQL_ARGUMENT",
		                  "run expects exactly one SQL argument or '-' for stdin");
	}
	options.sql = positional.front() == "-" ? ReadStdin() : positional.front();
	options.sql = NormalizeQuery(std::move(options.sql));

	if (options.has_output) {
		std::error_code error;
		if (fs::exists(options.output, error)) {
			throw SqrailError(EXIT_OUTPUT, "OUTPUT_EXISTS",
			                  "output already exists: " + options.output.string());
		}
		const fs::path parent = options.output.parent_path();
		if (!parent.empty() && !fs::is_directory(parent)) {
			throw SqrailError(EXIT_OUTPUT, "OUTPUT_DIRECTORY",
			                  "output directory does not exist: " + parent.string());
		}
	}

	return options;
}

void CheckResult(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result,
                 const std::string &code = "QUERY_FAILED") {
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, code, result->GetError());
	}
}

void CheckResult(const duckdb::unique_ptr<duckdb::QueryResult> &result,
                 const std::string &code = "QUERY_FAILED") {
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, code, result->GetError());
	}
}

void Configure(duckdb::Connection &connection, const RunOptions &options) {
	CheckResult(connection.Query("SET autoinstall_known_extensions = false"));
	CheckResult(connection.Query("SET autoload_known_extensions = false"));
	CheckResult(connection.Query("SET preserve_insertion_order = false"));

	if (!options.memory_limit.empty()) {
		CheckResult(connection.Query("SET memory_limit = " + SqlString(options.memory_limit)));
	}
	if (options.threads != 0) {
		CheckResult(connection.Query("SET threads = " + std::to_string(options.threads)));
	}
	if (!options.spill_directory.empty()) {
		std::error_code error;
		fs::create_directories(options.spill_directory, error);
		if (error) {
			throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
			                  "cannot create spill directory: " + options.spill_directory.string());
		}
		CheckResult(connection.Query("SET temp_directory = " +
		                             SqlString(options.spill_directory.string())));
	}
}

void BindTables(duckdb::Connection &connection, const std::vector<TableBinding> &tables) {
	for (const auto &table : tables) {
		const std::string sql = "CREATE OR REPLACE TEMP VIEW " + SqlIdentifier(table.name) +
		                        " AS SELECT * FROM " + ReaderExpression(table.path);
		CheckResult(connection.Query(sql), "TABLE_BIND_FAILED");
	}
}

int Run(int argc, char **argv) {
	RunOptions options = ParseRun(argc, argv);
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	Configure(connection, options);
	BindTables(connection, options.tables);

	if (options.has_output) {
		const auto nonce =
		    std::chrono::steady_clock::now().time_since_epoch().count();
		fs::path temporary_output = options.output;
		temporary_output += ".sqrail-tmp-" + std::to_string(nonce);
		const std::string copy_sql = "COPY (" + options.sql + ") TO " +
		                             SqlString(temporary_output.string()) + " (" +
		                             CopyOptionsFor(options.output) + ")";
		try {
			CheckResult(connection.Query(copy_sql));
			std::error_code error;
			if (fs::exists(options.output, error)) {
				fs::remove(temporary_output, error);
				throw SqrailError(EXIT_OUTPUT, "OUTPUT_EXISTS",
				                  "output appeared during execution: " + options.output.string());
			}
			fs::rename(temporary_output, options.output, error);
			if (error) {
				fs::remove(temporary_output, error);
				throw SqrailError(EXIT_OUTPUT, "OUTPUT_COMMIT",
				                  "cannot commit output: " + options.output.string());
			}
		} catch (...) {
			std::error_code ignored;
			fs::remove(temporary_output, ignored);
			throw;
		}
		return 0;
	}

	// DuckDB exposes a whole result row as a STRUCT when its relation alias is
	// selected. to_json therefore gives us correct typed JSON without collecting
	// the result or maintaining a second serializer in sqrail.
	const std::string stream_sql =
	    "SELECT to_json(__sqrail_row) FROM (" + options.sql + ") AS __sqrail_row";
	auto result = connection.SendQuery(stream_sql);
	CheckResult(result);

	while (true) {
		auto chunk = result->Fetch();
		if (!chunk) {
			break;
		}
		for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
			const auto value = chunk->GetValue(0, row);
			if (value.IsNull()) {
				std::cout << "null\n";
			} else {
				std::cout << value.GetValue<std::string>() << '\n';
			}
		}
	}
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, "QUERY_FAILED", result->GetError());
	}
	return 0;
}

int Schema(int argc, char **argv) {
	if (argc < 3) {
		throw SqrailError(EXIT_USAGE, "SCHEMA_ARGUMENT", "schema expects at least one input file");
	}

	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	RunOptions defaults;
	Configure(connection, defaults);

	for (int index = 2; index < argc; ++index) {
		const fs::path path = ExistingInput(argv[index]);
		const std::string view_name = "__sqrail_schema_" + std::to_string(index - 2);
		const std::string bind_sql = "CREATE TEMP VIEW " + SqlIdentifier(view_name) +
		                             " AS SELECT * FROM " + ReaderExpression(path);
		CheckResult(connection.Query(bind_sql), "SCHEMA_INFERENCE_FAILED");

		auto result = connection.Query("DESCRIBE SELECT * FROM " + SqlIdentifier(view_name));
		CheckResult(result, "SCHEMA_INFERENCE_FAILED");

		std::cout << "{\"file\":\"" << JsonEscape(path.string()) << "\",\"columns\":[";
		bool first = true;
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk) {
				break;
			}
			for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
				if (!first) {
					std::cout << ',';
				}
				first = false;
				const auto name = chunk->GetValue(0, row).ToString();
				const auto type = chunk->GetValue(1, row).ToString();
				const auto nullable = chunk->GetValue(2, row).ToString();
				std::cout << "{\"name\":\"" << JsonEscape(name) << "\",\"type\":\""
				          << JsonEscape(type) << "\",\"nullable\":"
				          << (nullable == "YES" ? "true" : "false") << '}';
			}
		}
		std::cout << "]}\n";
	}
	return 0;
}

void PrintAgentHelp() {
	std::cout
	    << "sqrail executes DuckDB SQL over local CSV, TSV, JSONL, and Parquet files.\n"
	    << "\n"
	    << "sqrail schema FILE...\n"
	    << "sqrail run [-t NAME=FILE]... [-o FILE] [--memory SIZE] [--threads N]\n"
	    << "           [--spill DIR] [SQL|-]\n"
	    << "\n"
	    << "-t binds a read-only file to a SQL table name. '-' reads SQL from stdin.\n"
	    << "Without -o, rows are JSONL on stdout. With -o, format follows the extension.\n"
	    << "Outputs are never overwritten. Diagnostics are JSON on stderr.\n"
	    << "SQL is DuckDB SQL. Row order is undefined without ORDER BY.\n"
	    << "Exit codes: 0 success, 2 usage, 3 input, 4 SQL, 5 output, 70 internal.\n";
}

void PrintHumanHelp() {
	std::cout << "sqrail " << SQRAIL_VERSION << " — SQL in, files out.\n\n";
	PrintAgentHelp();
	std::cout
	    << "\nExamples:\n"
	    << "  sqrail schema sales.csv\n"
	    << "  sqrail run -t sales=sales.csv 'SELECT count(*) AS n FROM sales'\n"
	    << "  sqrail run -t sales=sales.csv -o result.parquet - < query.sql\n";
}

void PrintError(const std::string &code, const std::string &message) {
	std::cerr << "{\"ok\":false,\"code\":\"" << JsonEscape(code) << "\",\"message\":\""
	          << JsonEscape(message) << "\"}\n";
}

} // namespace

int main(int argc, char **argv) {
	try {
		if (argc < 2) {
			PrintHumanHelp();
			return EXIT_USAGE;
		}

		const std::string command = argv[1];
		if (command == "--help" || command == "-h" || command == "help") {
			PrintHumanHelp();
			return 0;
		}
		if (command == "--agent-help") {
			PrintAgentHelp();
			return 0;
		}
		if (command == "--version" || command == "-V") {
			std::cout << "sqrail " << SQRAIL_VERSION << " (DuckDB " << SQRAIL_DUCKDB_VERSION << ")\n";
			return 0;
		}
		if (command == "schema") {
			return Schema(argc, argv);
		}
		if (command == "run") {
			return Run(argc, argv);
		}

		throw SqrailError(EXIT_USAGE, "UNKNOWN_COMMAND", "unknown command: " + command);
	} catch (const SqrailError &error) {
		PrintError(error.code, error.what());
		return error.exit_code;
	} catch (const std::exception &error) {
		PrintError("INTERNAL", error.what());
		return EXIT_INTERNAL;
	} catch (...) {
		PrintError("INTERNAL", "unknown internal error");
		return EXIT_INTERNAL;
	}
}
