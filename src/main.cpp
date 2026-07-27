#include "duckdb.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
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
	std::string source;
	std::vector<fs::path> paths;
};

struct RunOptions {
	std::vector<TableBinding> tables;
	std::string sql;
	fs::path output;
	fs::path spill_directory;
	std::string memory_limit;
	std::string max_spill;
	std::chrono::milliseconds timeout {0};
	uint64_t threads = 0;
	bool has_output = false;
};

struct FileType {
	std::string extension;
	std::string compression;
};

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
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

FileType DetectFileType(const fs::path &path, int exit_code, const std::string &unsupported_code) {
	std::string filename = Lower(path.filename().string());
	std::string compression;

	if (EndsWith(filename, ".gz")) {
		filename.resize(filename.size() - 3);
		compression = "GZIP";
	} else if (EndsWith(filename, ".zst")) {
		filename.resize(filename.size() - 4);
		compression = "ZSTD";
	} else if (EndsWith(filename, ".bz2") || EndsWith(filename, ".xz")) {
		throw SqrailError(exit_code, "UNSUPPORTED_COMPRESSION",
		                  "unsupported compression: " + path.string() + " (expected gzip or zstd)");
	}

	const std::string extension = fs::path(filename).extension().string();
	if (extension.empty()) {
		throw SqrailError(exit_code, unsupported_code, "file has no supported extension: " + path.string());
	}
	return {extension, compression};
}

std::string SqlPathList(const std::vector<fs::path> &paths) {
	std::string result = "[";
	for (std::size_t index = 0; index < paths.size(); ++index) {
		if (index != 0) {
			result.push_back(',');
		}
		result += SqlString(paths[index].string());
	}
	result.push_back(']');
	return result;
}

std::string SqlPathArgument(const std::vector<fs::path> &paths) {
	return paths.size() == 1 ? SqlString(paths.front().string()) : SqlPathList(paths);
}

std::string ReaderExpression(const std::vector<fs::path> &paths) {
	if (paths.empty()) {
		throw SqrailError(EXIT_INPUT, "EMPTY_DATASET", "input dataset has no files");
	}
	const std::string argument = SqlPathArgument(paths);
	const auto type = DetectFileType(paths.front(), EXIT_INPUT, "UNSUPPORTED_FORMAT");

	if (type.extension == ".parquet") {
		if (!type.compression.empty()) {
			throw SqrailError(EXIT_INPUT, "UNSUPPORTED_COMPRESSION",
			                  "externally compressed Parquet is not supported: " + paths.front().string());
		}
		return "read_parquet(" + argument + ")";
	}
	if (type.extension == ".json" || type.extension == ".jsonl" || type.extension == ".ndjson") {
		return "read_json_auto(" + argument + ")";
	}
	if (type.extension == ".tsv" || type.extension == ".tab") {
		return "read_csv_auto(" + argument + ", delim='	')";
	}
	if (type.extension == ".csv") {
		return "read_csv_auto(" + argument + ")";
	}

	throw SqrailError(EXIT_INPUT, "UNSUPPORTED_FORMAT",
	                  "unsupported input format: " + paths.front().string() +
	                      " (expected csv, tsv, json, jsonl, ndjson, or parquet)");
}

std::string CopyOptionsFor(const fs::path &path) {
	const auto type = DetectFileType(path, EXIT_OUTPUT, "UNSUPPORTED_OUTPUT");
	std::string options;

	if (type.extension == ".parquet") {
		if (!type.compression.empty()) {
			throw SqrailError(EXIT_OUTPUT, "UNSUPPORTED_COMPRESSION",
			                  "externally compressed Parquet is not supported: " + path.string());
		}
		return "FORMAT PARQUET";
	}
	if (type.extension == ".csv") {
		options = "FORMAT CSV, HEADER true";
	} else if (type.extension == ".tsv" || type.extension == ".tab") {
		options = "FORMAT CSV, HEADER true, DELIMITER '\t'";
	} else if (type.extension == ".json") {
		options = "FORMAT JSON, ARRAY true";
	} else if (type.extension == ".jsonl" || type.extension == ".ndjson") {
		options = "FORMAT JSON, ARRAY false";
	} else {
		throw SqrailError(EXIT_OUTPUT, "UNSUPPORTED_OUTPUT",
		                  "unsupported output extension: " + path.string() +
		                      " (expected csv, tsv, json, jsonl, ndjson, or parquet)");
	}

	if (!type.compression.empty()) {
		options += ", COMPRESSION " + type.compression;
	}
	return options;
}

bool IsJsonOutput(const FileType &type) {
	return type.extension == ".json" || type.extension == ".jsonl" || type.extension == ".ndjson";
}

duckdb::FileCompressionType CompressionTypeFor(const FileType &type) {
	if (type.compression == "GZIP") {
		return duckdb::FileCompressionType::GZIP;
	}
	if (type.compression == "ZSTD") {
		return duckdb::FileCompressionType::ZSTD;
	}
	return duckdb::FileCompressionType::UNCOMPRESSED;
}

fs::path ExistingInput(const std::string &raw_path) {
	std::error_code error;
	fs::path path = fs::absolute(fs::path(raw_path), error);
	if (error || !fs::exists(path) || !fs::is_regular_file(path)) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND", "input file not found: " + raw_path);
	}
	return fs::weakly_canonical(path);
}

void ValidateInputSet(const std::vector<fs::path> &paths, const std::string &source) {
	if (paths.empty()) {
		throw SqrailError(EXIT_INPUT, "EMPTY_DATASET", "input dataset matched no files: " + source);
	}
	const auto expected = DetectFileType(paths.front(), EXIT_INPUT, "UNSUPPORTED_FORMAT");
	for (const auto &path : paths) {
		const auto actual = DetectFileType(path, EXIT_INPUT, "UNSUPPORTED_FORMAT");
		if (actual.extension != expected.extension) {
			throw SqrailError(EXIT_INPUT, "MIXED_DATASET", "input dataset contains different file formats: " + source);
		}
		if (actual.extension == ".parquet" && !actual.compression.empty()) {
			throw SqrailError(EXIT_INPUT, "UNSUPPORTED_COMPRESSION",
			                  "externally compressed Parquet is not supported: " + path.string());
		}
	}
}

std::vector<fs::path> ResolveInputSet(duckdb::DuckDB &database, const std::string &source) {
	std::error_code error;
	const fs::path absolute = fs::absolute(fs::path(source), error).lexically_normal();
	if (error) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND", "cannot resolve input path: " + source);
	}

	std::vector<fs::path> paths;
	if (!duckdb::FileSystem::HasGlob(absolute.string())) {
		if (fs::is_directory(absolute, error)) {
			const auto options = fs::directory_options::none;
			fs::recursive_directory_iterator iterator(absolute, options, error);
			const fs::recursive_directory_iterator end;
			while (!error && iterator != end) {
				std::error_code entry_error;
				if (iterator->is_regular_file(entry_error) &&
				    EndsWith(Lower(iterator->path().filename().string()), ".parquet")) {
					paths.push_back(ExistingInput(iterator->path().string()));
				}
				iterator.increment(error);
			}
			if (error) {
				throw SqrailError(EXIT_INPUT, "DATASET_SCAN_FAILED",
				                  "cannot enumerate Parquet dataset: " + source + ": " + error.message());
			}
		} else {
			paths.push_back(ExistingInput(source));
		}
	} else {
		try {
			auto matches = database.instance->GetFileSystem().GlobFiles(absolute.string());
			paths.reserve(matches.size());
			for (const auto &match : matches) {
				paths.push_back(ExistingInput(match.path));
			}
		} catch (const std::exception &exception) {
			throw SqrailError(EXIT_INPUT, "DATASET_GLOB_FAILED",
			                  "cannot expand input dataset: " + source + ": " + exception.what());
		}
	}

	std::sort(paths.begin(), paths.end());
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	ValidateInputSet(paths, source);
	return paths;
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

std::string ParseSize(const std::string &raw, const std::string &option, const std::string &code) {
	static const std::regex size_pattern("^[0-9]+([.][0-9]+)?(B|KB|MB|GB|TB|KiB|MiB|GiB|TiB)$", std::regex::icase);
	if (!std::regex_match(raw, size_pattern)) {
		throw SqrailError(EXIT_USAGE, code, option + " must be a size such as 512MB or 2GB");
	}
	try {
		if (std::stod(raw) <= 0) {
			throw SqrailError(EXIT_USAGE, code, option + " must be greater than zero");
		}
	} catch (const std::invalid_argument &) {
		throw SqrailError(EXIT_USAGE, code, option + " must be a size such as 512MB or 2GB");
	} catch (const std::out_of_range &) {
		throw SqrailError(EXIT_USAGE, code, option + " value is too large");
	}
	return raw;
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

RunOptions ParseRun(int argc, char **argv, const bool check_only) {
	RunOptions options;
	std::vector<std::string> positional;
	std::unordered_set<std::string> table_names;
	bool has_memory = false;
	bool has_max_spill = false;
	bool has_threads = false;
	bool has_spill = false;
	bool has_timeout = false;

	for (int index = 2; index < argc; ++index) {
		const std::string argument = argv[index];
		if (argument == "-t" || argument == "--table") {
			auto table = ParseBinding(RequireValue(index, argc, argv, argument));
			if (!table_names.insert(Lower(table.name)).second) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_TABLE", "table name is bound more than once: " + table.name);
			}
			options.tables.push_back(std::move(table));
		} else if (argument == "-o" || argument == "--output") {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_OUTPUT", "check does not accept --output");
			}
			if (options.has_output) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--output may only be specified once");
			}
			options.output = fs::absolute(RequireValue(index, argc, argv, argument));
			options.has_output = true;
		} else if (argument == "--memory") {
			if (has_memory) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--memory may only be specified once");
			}
			options.memory_limit = ParseSize(RequireValue(index, argc, argv, argument), "--memory", "INVALID_MEMORY");
			has_memory = true;
		} else if (argument == "--threads") {
			if (has_threads) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--threads may only be specified once");
			}
			options.threads = ParseThreads(RequireValue(index, argc, argv, argument));
			has_threads = true;
		} else if (argument == "--spill") {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_SPILL", "check does not accept --spill");
			}
			if (has_spill) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--spill may only be specified once");
			}
			options.spill_directory = fs::absolute(RequireValue(index, argc, argv, argument));
			has_spill = true;
		} else if (argument == "--max-spill") {
			if (check_only) {
				throw SqrailError(EXIT_USAGE, "CHECK_SPILL", "check does not accept --max-spill");
			}
			if (has_max_spill) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--max-spill may only be specified once");
			}
			options.max_spill =
			    ParseSize(RequireValue(index, argc, argv, argument), "--max-spill", "INVALID_MAX_SPILL");
			has_max_spill = true;
		} else if (argument == "--timeout") {
			if (has_timeout) {
				throw SqrailError(EXIT_USAGE, "DUPLICATE_OPTION", "--timeout may only be specified once");
			}
			options.timeout = ParseTimeout(RequireValue(index, argc, argv, argument));
			has_timeout = true;
		} else if (argument.empty() || argument == "-" || argument.front() != '-') {
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
	options.sql = positional.front() == "-" ? ReadStdin() : positional.front();
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

std::string UniqueToken() {
	std::random_device random;
	const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::to_string(timestamp) + "-" + std::to_string(static_cast<uint64_t>(random()));
}

class SpillWorkspace final {
public:
	explicit SpillWorkspace(const fs::path &requested_root) {
		if (requested_root.empty()) {
			return;
		}

		std::error_code error;
		fs::create_directories(requested_root, error);
		if (error || !fs::is_directory(requested_root, error)) {
			throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
			                  "cannot create spill directory: " + requested_root.string());
		}
		const auto root = fs::weakly_canonical(requested_root, error);
		if (error) {
			throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
			                  "cannot resolve spill directory: " + requested_root.string());
		}

		for (int attempt = 0; attempt < 32; ++attempt) {
			auto candidate = root / (".sqrail-spill-" + UniqueToken());
			error.clear();
			if (!fs::create_directory(candidate, error)) {
				if (error && error != std::errc::file_exists) {
					throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
					                  "cannot create private spill directory: " + root.string() + ": " +
					                      error.message());
				}
				continue;
			}

			fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace, error);
			if (error) {
				const auto message = error.message();
				std::error_code ignored;
				fs::remove_all(candidate, ignored);
				throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
				                  "cannot protect private spill directory: " + candidate.string() + ": " + message);
			}
			path = std::move(candidate);
			return;
		}
		throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
		                  "cannot allocate a unique private spill directory under: " + root.string());
	}

	SpillWorkspace(const SpillWorkspace &) = delete;
	SpillWorkspace &operator=(const SpillWorkspace &) = delete;

	~SpillWorkspace() {
		if (!path.empty()) {
			std::error_code ignored;
			fs::remove_all(path, ignored);
		}
	}

	[[nodiscard]] const fs::path &Path() const {
		return path;
	}

private:
	fs::path path;
};

void CheckResult(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result,
                 const std::string &code = "QUERY_FAILED") {
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, code, result->GetError());
	}
}

void CheckResult(const duckdb::unique_ptr<duckdb::QueryResult> &result, const std::string &code = "QUERY_FAILED") {
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, code, result->GetError());
	}
}

void Configure(duckdb::Connection &connection, const RunOptions &options, const std::vector<fs::path> &allowed_paths) {
	CheckResult(connection.Query("SET autoinstall_known_extensions = false"));
	CheckResult(connection.Query("SET autoload_known_extensions = false"));
	CheckResult(connection.Query("SET allow_community_extensions = false"));
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
		CheckResult(connection.Query("SET temp_directory = " + SqlString(options.spill_directory.string())));
		if (!options.max_spill.empty()) {
			CheckResult(connection.Query("SET max_temp_directory_size = " + SqlString(options.max_spill)));
		}
		const std::vector<fs::path> allowed_directories {fs::weakly_canonical(options.spill_directory)};
		CheckResult(connection.Query("SET allowed_directories = " + SqlPathList(allowed_directories)));
	} else {
		CheckResult(connection.Query("SET temp_directory = ''"));
	}

	CheckResult(connection.Query("SET allowed_paths = " + SqlPathList(allowed_paths)));
	CheckResult(connection.Query("SET enable_external_access = false"));
	CheckResult(connection.Query("SET lock_configuration = true"));
}

class QueryDeadline final {
public:
	QueryDeadline(duckdb::Connection &connection_p, const std::chrono::milliseconds timeout)
	    : connection(connection_p) {
		if (timeout.count() > 0) {
			worker = std::thread([this, timeout]() {
				std::unique_lock<std::mutex> lock(mutex);
				if (condition.wait_for(lock, timeout, [this]() { return complete; })) {
					return;
				}
				timed_out.store(true);
				lock.unlock();
				connection.Interrupt();
			});
		}
	}

	QueryDeadline(const QueryDeadline &) = delete;
	QueryDeadline &operator=(const QueryDeadline &) = delete;

	~QueryDeadline() {
		Stop();
	}

	void Stop() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			complete = true;
		}
		condition.notify_one();
		if (worker.joinable()) {
			worker.join();
		}
	}

	[[nodiscard]] bool TimedOut() const {
		return timed_out.load();
	}

private:
	duckdb::Connection &connection;
	std::atomic<bool> timed_out {false};
	std::condition_variable condition;
	std::mutex mutex;
	std::thread worker;
	bool complete = false;
};

void BindTables(duckdb::Connection &connection, const std::vector<TableBinding> &tables) {
	for (const auto &table : tables) {
		const std::string sql = "CREATE OR REPLACE TEMP VIEW " + SqlIdentifier(table.name) + " AS SELECT * FROM " +
		                        ReaderExpression(table.paths);
		CheckResult(connection.Query(sql), "TABLE_BIND_FAILED");
	}
}

std::string ValidateSelectQuery(duckdb::Connection &connection, const std::string &sql) {
	std::vector<duckdb::unique_ptr<duckdb::SQLStatement>> statements;
	try {
		statements = connection.ExtractStatements(sql);
	} catch (const std::exception &error) {
		throw SqrailError(EXIT_QUERY, "QUERY_FAILED", error.what());
	}

	if (statements.size() != 1) {
		throw SqrailError(EXIT_QUERY, "MULTIPLE_STATEMENTS", "exactly one SQL statement is required");
	}
	if (statements.front()->type != duckdb::StatementType::SELECT_STATEMENT) {
		throw SqrailError(EXIT_QUERY, "READ_ONLY_QUERY", "only a SELECT, VALUES, or WITH query is allowed");
	}
	return NormalizeQuery(statements.front()->ToString());
}

fs::path TemporaryOutputPath(const fs::path &output) {
	fs::path temporary = output;
	temporary += ".sqrail-tmp-" + UniqueToken();
	return temporary;
}

void CommitOutput(const fs::path &temporary, const fs::path &output) {
	std::error_code error;
	fs::create_hard_link(temporary, output, error);
	if (error) {
		std::error_code ignored;
		fs::remove(temporary, ignored);
		if (fs::exists(output, ignored)) {
			throw SqrailError(EXIT_OUTPUT, "OUTPUT_EXISTS", "output appeared during execution: " + output.string());
		}
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_COMMIT",
		                  "cannot commit output: " + output.string() + ": " + error.message());
	}

	// The destination now names the completed inode. Removing the private name
	// cannot invalidate the committed output, so cleanup is deliberately best effort.
	fs::remove(temporary, error);
}

template <class WRITE>
uint64_t StreamJson(duckdb::Connection &connection, const std::string &sql, const bool array, WRITE &&write) {
	const std::string stream_sql = "SELECT to_json(__sqrail_row) FROM (" + sql + ") AS __sqrail_row";
	auto result = connection.SendQuery(stream_sql);
	CheckResult(result);

	constexpr std::size_t buffer_capacity = std::size_t {1024} * 1024U;
	std::string buffer;
	buffer.reserve(buffer_capacity);
	uint64_t rows = 0;

	const auto flush = [&]() {
		if (!buffer.empty()) {
			write(buffer.data(), buffer.size());
			buffer.clear();
		}
	};
	const auto append = [&](const std::string_view value) {
		if (buffer.size() + value.size() > buffer_capacity) {
			flush();
		}
		if (value.size() > buffer_capacity) {
			write(value.data(), value.size());
		} else {
			buffer.append(value);
		}
	};

	if (array) {
		buffer.push_back('[');
	}
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk) {
			break;
		}
		duckdb::UnifiedVectorFormat vector_format;
		chunk->data[0].ToUnifiedFormat(chunk->size(), vector_format);
		const auto *values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(vector_format);
		for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
			if (array && rows != 0) {
				buffer.push_back(',');
			}
			const auto index = vector_format.sel->get_index(row);
			if (!vector_format.validity.RowIsValid(index)) {
				append("null");
			} else {
				const auto &value = values[index];
				const std::string_view json(value.GetData(), value.GetSize());
				if (json.find("NaN") == std::string_view::npos &&
				    json.find("Infinity") == std::string_view::npos) {
					append(json);
				} else {
					append(sqrail::StrictJson(std::string(json)));
				}
			}
			if (!array) {
				buffer.push_back('\n');
			}
			++rows;
		}
	}
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, "QUERY_FAILED", result->GetError());
	}
	if (array) {
		buffer += "]\n";
	}
	flush();
	return rows;
}

uint64_t WriteJsonFile(duckdb::DuckDB &database, duckdb::Connection &connection, const std::string &sql,
                       const fs::path &temporary, const FileType &type) {
	auto flags = duckdb::FileFlags::FILE_FLAGS_WRITE | duckdb::FileFlags::FILE_FLAGS_FILE_CREATE |
	             duckdb::FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE | duckdb::FileFlags::FILE_FLAGS_PRIVATE |
	             duckdb::FileOpenFlags(CompressionTypeFor(type));
	auto &file_system = database.instance->GetFileSystem();
	auto handle = file_system.OpenFile(temporary.string(), flags);
	const auto write = [&](const char *data, const std::size_t size) {
		handle->Write(const_cast<char *>(data), size);
	};
	const auto rows = StreamJson(connection, sql, type.extension == ".json", write);
	handle->Close();
	return rows;
}

int Execute(int argc, char **argv, const bool check_only) {
	RunOptions options = ParseRun(argc, argv, check_only);
	SpillWorkspace spill_workspace(options.spill_directory);
	options.spill_directory = spill_workspace.Path();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);

	std::vector<fs::path> allowed_paths;
	for (auto &table : options.tables) {
		table.paths = ResolveInputSet(database, table.source);
		allowed_paths.insert(allowed_paths.end(), table.paths.begin(), table.paths.end());
	}
	const fs::path temporary_output = options.has_output ? TemporaryOutputPath(options.output) : fs::path();
	if (options.has_output) {
		allowed_paths.push_back(temporary_output);
	}
	Configure(connection, options, allowed_paths);
	QueryDeadline deadline(connection, options.timeout);
	try {
		BindTables(connection, options.tables);
		options.sql = ValidateSelectQuery(connection, options.sql);

		if (check_only) {
			auto result = connection.Query("EXPLAIN (FORMAT JSON) " + options.sql);
			CheckResult(result, "PLAN_FAILED");
			std::string plan;
			while (true) {
				auto chunk = result->Fetch();
				if (!chunk) {
					break;
				}
				for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
					const auto column = chunk->ColumnCount() - 1;
					plan = chunk->GetValue(column, row).ToString();
				}
			}
			deadline.Stop();
			if (deadline.TimedOut()) {
				throw SqrailError(EXIT_QUERY, "QUERY_TIMEOUT", "query planning exceeded --timeout");
			}
			if (plan.empty()) {
				throw SqrailError(EXIT_QUERY, "PLAN_FAILED", "DuckDB returned an empty physical plan");
			}
			std::cout << "{\"ok\":true,\"plan\":" << sqrail::StrictJson(plan) << "}\n";
			return 0;
		}

		if (options.has_output) {
			const auto type = DetectFileType(options.output, EXIT_OUTPUT, "UNSUPPORTED_OUTPUT");
			if (IsJsonOutput(type)) {
				WriteJsonFile(database, connection, options.sql, temporary_output, type);
			} else {
				const std::string copy_sql = "COPY (" + options.sql + ") TO " + SqlString(temporary_output.string()) +
				                             " (" + CopyOptionsFor(options.output) + ")";
				CheckResult(connection.Query(copy_sql));
			}
			deadline.Stop();
			if (deadline.TimedOut()) {
				throw SqrailError(EXIT_QUERY, "QUERY_TIMEOUT", "query exceeded --timeout");
			}
			CommitOutput(temporary_output, options.output);
			return 0;
		}

		// A whole result row is a STRUCT, so DuckDB performs typed JSON conversion.
		// sqrail applies the strict RFC 8259 boundary and batches writes to reduce
		// iostream overhead without collecting the full result.
		const auto write = [](const char *data, const std::size_t size) {
			std::cout.write(data, static_cast<std::streamsize>(size));
			if (!std::cout) {
				throw SqrailError(EXIT_OUTPUT, "STDOUT_WRITE", "cannot write query result to stdout");
			}
		};
		StreamJson(connection, options.sql, false, write);
		deadline.Stop();
		if (deadline.TimedOut()) {
			throw SqrailError(EXIT_QUERY, "QUERY_TIMEOUT", "query exceeded --timeout");
		}
	} catch (...) {
		deadline.Stop();
		if (options.has_output) {
			std::error_code ignored;
			fs::remove(temporary_output, ignored);
		}
		if (deadline.TimedOut()) {
			throw SqrailError(EXIT_QUERY, "QUERY_TIMEOUT", "query exceeded --timeout");
		}
		throw;
	}
	return 0;
}

int Schema(int argc, char **argv) {
	if (argc < 3) {
		throw SqrailError(EXIT_USAGE, "SCHEMA_ARGUMENT", "schema expects at least one input file");
	}

	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	struct SchemaInput {
		std::string source;
		std::vector<fs::path> paths;
	};
	std::vector<SchemaInput> inputs;
	std::vector<fs::path> allowed_paths;
	inputs.reserve(static_cast<std::size_t>(argc - 2));
	for (int index = 2; index < argc; ++index) {
		const auto source = fs::absolute(fs::path(argv[index])).lexically_normal().string();
		auto paths = ResolveInputSet(database, source);
		allowed_paths.insert(allowed_paths.end(), paths.begin(), paths.end());
		inputs.push_back({source, std::move(paths)});
	}
	RunOptions defaults;
	Configure(connection, defaults, allowed_paths);

	for (std::size_t index = 0; index < inputs.size(); ++index) {
		const auto &input = inputs[index];
		const std::string view_name = "__sqrail_schema_" + std::to_string(index);
		const std::string bind_sql =
		    "CREATE TEMP VIEW " + SqlIdentifier(view_name) + " AS SELECT * FROM " + ReaderExpression(input.paths);
		CheckResult(connection.Query(bind_sql), "SCHEMA_INFERENCE_FAILED");

		auto result = connection.Query("DESCRIBE SELECT * FROM " + SqlIdentifier(view_name));
		CheckResult(result, "SCHEMA_INFERENCE_FAILED");

		std::cout << "{\"file\":\"" << sqrail::JsonEscape(input.source) << "\",\"files\":" << input.paths.size()
		          << ",\"columns\":[";
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
				std::cout << "{\"name\":\"" << sqrail::JsonEscape(name) << "\",\"type\":\"" << sqrail::JsonEscape(type)
				          << "\",\"nullable\":" << (nullable == "YES" ? "true" : "false") << '}';
			}
		}
		std::cout << "]}\n";
	}
	return 0;
}

void PrintAgentHelp() {
	std::cout << "sqrail runs read-only SQL over CSV, TSV, JSON, and Parquet.\n"
	          << "\n"
	          << "sqrail schema FILE...\n"
	          << "sqrail check [-t NAME=PATH]... [--memory SIZE] [--threads N] [--timeout DURATION] [SQL|-]\n"
	          << "sqrail run [-t NAME=PATH]... [-o FILE] [--memory SIZE] [--threads N]\n"
	          << "           [--spill DIR [--max-spill SIZE]] [--timeout DURATION] [SQL|-]\n"
	          << "sqrail --version\n"
	          << "\n"
	          << "-t binds a read-only file, Parquet directory, or glob. '-' reads SQL from stdin.\n"
	          << "check validates SQL and emits its JSON physical plan without executing it.\n"
	          << "Without -o, rows are JSONL on stdout. With -o, format follows the extension.\n"
	          << "Text files may use .gz or .zst. Outputs are atomic, never overwritten.\n"
	          << "SQL is one SELECT, VALUES, or WITH query. Row order is undefined without ORDER BY.\n"
	          << "Errors are one JSON object on stderr.\n"
	          << "Exit codes: 0 success, 2 usage, 3 input, 4 SQL, 5 output, 70 internal.\n";
}

void PrintHumanHelp() {
	std::cout << "sqrail " << SQRAIL_VERSION << " — SQL in, files out.\n\n";
	PrintAgentHelp();
	std::cout << "\nExamples:\n"
	          << "  sqrail schema sales.csv\n"
	          << "  sqrail run -t sales=sales.csv 'SELECT count(*) AS n FROM sales'\n"
	          << "  sqrail run -t sales=sales.csv -o result.parquet - < query.sql\n";
}

void PrintError(const std::string &code, const std::string &message) {
	std::cerr << "{\"ok\":false,\"code\":\"" << sqrail::JsonEscape(code) << "\",\"message\":\""
	          << sqrail::JsonEscape(message) << "\"}\n";
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
			return Execute(argc, argv, false);
		}
		if (command == "check") {
			return Execute(argc, argv, true);
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
