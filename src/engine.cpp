#include "engine.hpp"

#include "duckdb/common/file_system.hpp"
#include "json.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace sqrail {
namespace {

constexpr int EXIT_INPUT = 3;
constexpr int EXIT_QUERY = 4;
constexpr int EXIT_OUTPUT = 5;

std::string Lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(),
	               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

bool EndsWith(const std::string &value, const std::string &suffix) {
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
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

void CheckInputFileLimit(const std::size_t resolved, const uint64_t max_input_files) {
	if (max_input_files != 0 && resolved > max_input_files) {
		throw SqrailError(EXIT_INPUT, "INPUT_LIMIT",
		                  "input dataset exceeded --max-input-files " + std::to_string(max_input_files));
	}
}

using ColumnSignature = std::vector<std::pair<std::string, std::string>>;

ColumnSignature ReadColumnSignature(duckdb::Connection &connection, const fs::path &path) {
	const std::vector<fs::path> single_path {path};
	auto result = connection.Query("DESCRIBE SELECT * FROM " + ReaderExpression(single_path, true));
	CheckResult(result, "SCHEMA_INFERENCE_FAILED");

	ColumnSignature signature;
	signature.reserve(result->RowCount());
	for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
		signature.emplace_back(result->GetValue(0, row).ToString(), result->GetValue(1, row).ToString());
	}
	return signature;
}

} // namespace

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

FileType DetectFileType(const fs::path &path, const int exit_code, const std::string &unsupported_code) {
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

std::string ReaderExpression(const std::vector<fs::path> &paths, const bool strict_schema) {
	if (paths.empty()) {
		throw SqrailError(EXIT_INPUT, "EMPTY_DATASET", "input dataset has no files");
	}
	const std::string argument = SqlPathArgument(paths);
	const auto type = DetectFileType(paths.front(), EXIT_INPUT, "UNSUPPORTED_FORMAT");
	const std::string union_option = paths.size() > 1 && !strict_schema ? ", union_by_name=true" : "";

	if (type.extension == ".parquet") {
		if (!type.compression.empty()) {
			throw SqrailError(EXIT_INPUT, "UNSUPPORTED_COMPRESSION",
			                  "externally compressed Parquet is not supported: " + paths.front().string());
		}
		return "read_parquet(" + argument + union_option + ")";
	}
	if (type.extension == ".json" || type.extension == ".jsonl" || type.extension == ".ndjson") {
		return "read_json_auto(" + argument + union_option + ")";
	}
	if (type.extension == ".tsv" || type.extension == ".tab") {
		return "read_csv_auto(" + argument + ", delim='\t'" + union_option + ")";
	}
	if (type.extension == ".csv") {
		return "read_csv_auto(" + argument + union_option + ")";
	}

	throw SqrailError(EXIT_INPUT, "UNSUPPORTED_FORMAT",
	                  "unsupported input format: " + paths.front().string() +
	                      " (expected csv, tsv, json, jsonl, ndjson, or parquet)");
}

bool IsJsonOutput(const FileType &type) {
	return type.extension == ".json" || type.extension == ".jsonl" || type.extension == ".ndjson";
}

std::vector<fs::path> ResolveInputSet(duckdb::DuckDB &database, const std::string &source, ExecutionControl &control,
                                      const std::size_t previously_resolved, const uint64_t max_input_files) {
	control.Checkpoint("input resolution exceeded --timeout");
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
				control.Checkpoint("input resolution exceeded --timeout");
				std::error_code entry_error;
				if (iterator->is_regular_file(entry_error) &&
				    EndsWith(Lower(iterator->path().filename().string()), ".parquet")) {
					paths.push_back(ExistingInput(iterator->path().string()));
					CheckInputFileLimit(previously_resolved + paths.size(), max_input_files);
				}
				iterator.increment(error);
			}
			if (error) {
				throw SqrailError(EXIT_INPUT, "DATASET_SCAN_FAILED",
				                  "cannot enumerate Parquet dataset: " + source + ": " + error.message());
			}
		} else {
			paths.push_back(ExistingInput(source));
			CheckInputFileLimit(previously_resolved + paths.size(), max_input_files);
		}
	} else {
		try {
			control.Checkpoint("input resolution exceeded --timeout");
			auto matches = database.instance->GetFileSystem().GlobFiles(absolute.string());
			paths.reserve(matches.size());
			for (const auto &match : matches) {
				control.Checkpoint("input resolution exceeded --timeout");
				paths.push_back(ExistingInput(match.path));
				CheckInputFileLimit(previously_resolved + paths.size(), max_input_files);
			}
		} catch (const SqrailError &) {
			throw;
		} catch (const std::exception &exception) {
			throw SqrailError(EXIT_INPUT, "DATASET_GLOB_FAILED",
			                  "cannot expand input dataset: " + source + ": " + exception.what());
		}
	}

	std::sort(paths.begin(), paths.end());
	paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
	CheckInputFileLimit(previously_resolved + paths.size(), max_input_files);
	ValidateInputSet(paths, source);
	control.Checkpoint("input resolution exceeded --timeout");
	return paths;
}

std::string UniqueToken() {
	std::random_device random;
	const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::to_string(timestamp) + "-" + std::to_string(static_cast<uint64_t>(random()));
}

SpillWorkspace::SpillWorkspace(const fs::path &requested_root) {
	if (requested_root.empty()) {
		return;
	}

	std::error_code error;
	fs::create_directories(requested_root, error);
	if (error || !fs::is_directory(requested_root, error)) {
		throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY", "cannot create spill directory: " + requested_root.string());
	}
	const auto root = fs::weakly_canonical(requested_root, error);
	if (error) {
		throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY", "cannot resolve spill directory: " + requested_root.string());
	}

	for (int attempt = 0; attempt < 32; ++attempt) {
		auto candidate = root / (".sqrail-spill-" + UniqueToken());
		error.clear();
		if (!fs::create_directory(candidate, error)) {
			if (error && error != std::errc::file_exists) {
				throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
				                  "cannot create private spill directory: " + root.string() + ": " + error.message());
			}
			continue;
		}

		try {
			ProtectPrivateDirectory(candidate);
		} catch (...) {
			std::error_code ignored;
			fs::remove_all(candidate, ignored);
			throw;
		}
		path = std::move(candidate);
		return;
	}
	throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
	                  "cannot allocate a unique private spill directory under: " + root.string());
}

SpillWorkspace::~SpillWorkspace() {
	if (!path.empty()) {
		std::error_code ignored;
		fs::remove_all(path, ignored);
	}
}

const fs::path &SpillWorkspace::Path() const {
	return path;
}

void CheckResult(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result, const std::string &code) {
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, code, result->GetError());
	}
}

void CheckResult(const duckdb::unique_ptr<duckdb::QueryResult> &result, const std::string &code) {
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, code, result->GetError());
	}
}

void ValidateStrictSchemas(duckdb::Connection &connection, const std::vector<fs::path> &paths,
                           const std::string &source) {
	if (paths.size() < 2) {
		return;
	}
	const auto expected = ReadColumnSignature(connection, paths.front());
	for (std::size_t index = 1; index < paths.size(); ++index) {
		if (ReadColumnSignature(connection, paths[index]) != expected) {
			throw SqrailError(EXIT_INPUT, "SCHEMA_MISMATCH",
			                  "input dataset contains different column names, order, or types: " + source);
		}
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

void BindTables(duckdb::Connection &connection, const std::vector<TableBinding> &tables, const bool strict_schema) {
	for (const auto &table : tables) {
		if (strict_schema) {
			ValidateStrictSchemas(connection, table.paths, table.source);
		}
		const std::string sql = "CREATE OR REPLACE TEMP VIEW " + SqlIdentifier(table.name) + " AS SELECT * FROM " +
		                        ReaderExpression(table.paths, strict_schema);
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

std::string ResultColumnsJson(duckdb::Connection &connection, const std::string &sql) {
	auto result = connection.Query("DESCRIBE " + sql);
	CheckResult(result, "PLAN_FAILED");

	std::ostringstream output;
	output << '[';
	for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
		if (row != 0) {
			output << ',';
		}
		const auto name = result->GetValue(0, row).ToString();
		const auto type = result->GetValue(1, row).ToString();
		const auto nullable = result->GetValue(2, row).ToString();
		output << "{\"name\":\"" << JsonEscape(name) << "\",\"type\":\"" << JsonEscape(type)
		       << "\",\"nullable\":" << (nullable == "YES" ? "true" : "false") << '}';
	}
	output << ']';
	return output.str();
}

std::string InputBindingsJson(const std::vector<TableBinding> &tables) {
	std::ostringstream output;
	output << '[';
	for (std::size_t index = 0; index < tables.size(); ++index) {
		if (index != 0) {
			output << ',';
		}
		output << "{\"table\":\"" << JsonEscape(tables[index].name) << "\",\"files\":" << tables[index].paths.size()
		       << '}';
	}
	output << ']';
	return output.str();
}

} // namespace sqrail
