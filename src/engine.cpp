#include "engine.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/hive_partitioning.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "error.hpp"
#include "json.hpp"
#include "platform.hpp"

#include <chrono>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace sqrail {
namespace {

bool EndsWith(const std::string &value, const std::string &suffix) {
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string EscapeGlobDirectory(const fs::path &directory) {
	const auto literal = directory.relative_path().string();
	std::string escaped = directory.root_path().string();
	escaped.reserve(escaped.size() + literal.size());
	for (const char ch : literal) {
		if (ch == '[' || ch == '*' || ch == '?') {
			escaped.push_back('[');
			escaped.push_back(ch);
			escaped.push_back(']');
		} else {
			escaped.push_back(ch);
		}
	}
	return escaped;
}

duckdb::unique_ptr<duckdb::MultiFileList> MatchInputGlob(duckdb::FileSystem &file_system, const fs::path &source,
                                                         ExecutionControl &control) {
	// Keep the implicit working directory literal. Resolve the prefix on disk before
	// DuckDB can simplify a symlink followed by ".." in the glob string.
	const auto relative = source.relative_path();
	fs::path prefix = source.root_path();
	std::vector<fs::path> patterns;
	for (auto part = relative.begin(); part != relative.end(); ++part) {
		const auto segment = part->string();
		if (duckdb::FileSystem::HasGlob(segment)) {
			std::error_code error;
			const auto directory = fs::canonical(prefix.empty() ? fs::path(".") : prefix, error);
			if (error || !fs::is_directory(directory, error)) {
				break;
			}
			fs::path pattern = EscapeGlobDirectory(directory);
			for (auto suffix = part; suffix != relative.end(); ++suffix) {
				pattern /= *suffix;
			}
			patterns.push_back(std::move(pattern));
			if (segment.find_first_of("*?") != std::string::npos || !fs::is_directory(prefix / *part, error)) {
				break;
			}
		}
		prefix /= *part;
	}

	// Prefer matches under an existing bracket-named directory. If it has no
	// matches, retry with that component interpreted as a character class.
	for (auto pattern = patterns.rbegin(); pattern != patterns.rend(); ++pattern) {
		control.Checkpoint("input resolution exceeded --timeout");
		auto matches = file_system.GlobFileList(pattern->string(), duckdb::FileGlobOptions::ALLOW_EMPTY);
		// DuckDB also tries a failed pattern as a literal filename. Only the
		// original source may select a literal file, never an escaped pattern.
		if (!matches->IsEmpty() && fs::path(matches->GetFirstFile().path) != *pattern) {
			return matches;
		}
	}
	std::error_code error;
	if (!duckdb::FileSystem::HasGlob(source.filename().string()) && fs::is_directory(source.parent_path(), error)) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND", "input file not found: " + source.string());
	}
	throw SqrailError(EXIT_INPUT, "DATASET_GLOB_FAILED", "input dataset matched no files: " + source.string());
}

std::string SqlPathList(const std::vector<fs::path> &paths, const bool literal_files = false) {
	std::string result = "[";
	for (std::size_t index = 0; index < paths.size(); ++index) {
		if (index != 0) {
			result.push_back(',');
		}
		const auto path = SqlString(paths[index].string());
		// DuckDB opens a file struct literally. A plain string is expanded as a glob.
		result += literal_files ? "{'filename':" + path + "}" : path;
	}
	result.push_back(']');
	return result;
}

fs::path ExistingInput(const fs::path &candidate, const std::string &display_path) {
	std::error_code error;
	fs::path path = candidate.is_absolute() ? candidate : fs::absolute(candidate, error);
	const auto status = error ? fs::file_status() : fs::status(path, error);
	if (fs::status_known(status) && !fs::is_regular_file(status)) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND", "input file not found: " + display_path);
	}
	if (!error) {
		path = fs::weakly_canonical(path, error);
	}
	if (error) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND",
		                  "cannot access input file: " + display_path + ": " + error.message());
	}
	return path;
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

struct ReaderOptions {
	bool strict_schema = false;
	bool all_varchar = false;
	bool disable_hive_partitioning = false;
};

struct FileSchemaSignature {
	ColumnSignature columns;
	std::vector<std::string> hive_keys;

	bool operator==(const FileSchemaSignature &other) const = default;
};

std::string BuildReaderExpression(const std::vector<fs::path> &paths, const ReaderOptions options) {
	if (paths.empty()) {
		throw SqrailError(EXIT_INPUT, "EMPTY_DATASET", "input dataset has no files");
	}
	const std::string argument = SqlPathList(paths, true);
	const auto type = DetectFileType(paths.front(), EXIT_INPUT, "UNSUPPORTED_FORMAT");
	const std::string union_option = paths.size() > 1 && !options.strict_schema ? ", union_by_name=true" : "";
	const std::string hive_option = options.disable_hive_partitioning ? ", hive_partitioning=false" : "";
	const std::string varchar_option = options.all_varchar ? ", all_varchar=true" : "";

	if (type.extension == ".parquet") {
		if (!type.compression.empty()) {
			throw SqrailError(EXIT_INPUT, "UNSUPPORTED_COMPRESSION",
			                  "externally compressed Parquet is not supported: " + paths.front().string());
		}
		return "read_parquet(" + argument + union_option + hive_option + ")";
	}
	if (type.extension == ".json" || type.extension == ".jsonl" || type.extension == ".ndjson") {
		return "read_json_auto(" + argument + union_option + hive_option + ")";
	}
	if (type.extension == ".tsv" || type.extension == ".tab") {
		return "read_csv_auto(" + argument + ", delim='\t'" + varchar_option + union_option + hive_option + ")";
	}
	if (type.extension == ".csv") {
		return "read_csv_auto(" + argument + varchar_option + union_option + hive_option + ")";
	}

	throw SqrailError(EXIT_INPUT, "UNSUPPORTED_FORMAT",
	                  "unsupported input format: " + paths.front().string() +
	                      " (expected csv, tsv, json, jsonl, ndjson, or parquet)");
}

FileSchemaSignature ReadFileSchemaSignature(duckdb::Connection &connection, const fs::path &path,
                                            const bool all_varchar) {
	const std::vector<fs::path> single_path {path};
	const ReaderOptions reader_options {true, all_varchar, true};
	auto result = connection.Query("DESCRIBE SELECT * FROM " + BuildReaderExpression(single_path, reader_options));
	CheckResult(result, "SCHEMA_INFERENCE_FAILED");

	const auto &rows = result->Collection();
	ColumnSignature columns;
	columns.reserve(rows.Count());
	for (duckdb::idx_t row = 0; row < rows.Count(); ++row) {
		columns.emplace_back(rows.GetValue(0, row).ToString(), rows.GetValue(1, row).ToString());
	}

	const auto partitions = duckdb::HivePartitioning::Parse(path.string());
	std::vector<std::string> hive_keys;
	hive_keys.reserve(partitions.size());
	for (const auto &partition : partitions) {
		hive_keys.push_back(partition.first);
	}
	return {std::move(columns), std::move(hive_keys)};
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
	std::string filename = LowerAscii(path.filename().string());
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

std::string ReaderExpression(const std::vector<fs::path> &paths, const bool strict_schema, const bool all_varchar) {
	return BuildReaderExpression(paths, {strict_schema, all_varchar, false});
}

bool IsJsonOutput(const FileType &type) {
	return type.extension == ".json" || type.extension == ".jsonl" || type.extension == ".ndjson";
}

std::vector<fs::path> ResolveInputSet(duckdb::DuckDB &database, const std::string &source, ExecutionControl &control,
                                      const std::size_t previously_resolved, const uint64_t max_input_files) {
	control.Checkpoint("input resolution exceeded --timeout");
	if (source.empty()) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND", "input path must not be empty");
	}
	std::error_code error;
	const fs::path source_path(source);
	const fs::path absolute = fs::absolute(source_path, error);
	if (error) {
		throw SqrailError(EXIT_INPUT, "INPUT_NOT_FOUND", "cannot resolve input path: " + source);
	}

	std::set<fs::path> unique_paths;
	const auto add_path = [&](const fs::path &candidate, const std::string &display_path) {
		auto canonical = ExistingInput(candidate, display_path);
		if (unique_paths.insert(std::move(canonical)).second) {
			CheckInputFileLimit(previously_resolved + unique_paths.size(), max_input_files);
		}
	};

	std::error_code status_error;
	const auto literal_status = fs::symlink_status(absolute, status_error);
	const bool is_literal_path = !duckdb::FileSystem::HasGlob(source) || fs::exists(literal_status);

	if (is_literal_path) {
		if (fs::is_directory(absolute, error)) {
			const auto options = fs::directory_options::none;
			fs::recursive_directory_iterator iterator(absolute, options, error);
			const fs::recursive_directory_iterator end;
			while (!error && iterator != end) {
				control.Checkpoint("input resolution exceeded --timeout");
				std::error_code entry_error;
				if (iterator->is_regular_file(entry_error) &&
				    EndsWith(LowerAscii(iterator->path().filename().string()), ".parquet")) {
					add_path(iterator->path(), iterator->path().string());
				}
				iterator.increment(error);
			}
			if (error) {
				throw SqrailError(EXIT_INPUT, "DATASET_SCAN_FAILED",
				                  "cannot enumerate Parquet dataset: " + source + ": " + error.message());
			}
		} else {
			add_path(absolute, source);
		}
	} else {
		try {
			control.Checkpoint("input resolution exceeded --timeout");
			auto matches = MatchInputGlob(database.instance->GetFileSystem(), source_path, control);
			for (const auto &match : matches->Files()) {
				control.Checkpoint("input resolution exceeded --timeout");
				add_path(fs::path(match.path), match.path);
			}
		} catch (const SqrailError &) {
			throw;
		} catch (const std::exception &exception) {
			throw SqrailError(EXIT_INPUT, "DATASET_GLOB_FAILED",
			                  "cannot expand input dataset: " + source + ": " + duckdb::ErrorData(exception).Message());
		}
	}

	std::vector<fs::path> paths(unique_paths.begin(), unique_paths.end());
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

void CheckResult(const duckdb::unique_ptr<duckdb::QueryResult> &result, const std::string &code) {
	if (result->HasError()) {
		throw SqrailError(EXIT_QUERY, code, result->GetError());
	}
}

void ValidateStrictSchemas(duckdb::Connection &connection, const std::vector<fs::path> &paths,
                           const std::string &source, ExecutionControl &control, const bool all_varchar) {
	if (paths.size() < 2) {
		return;
	}
	control.Checkpoint("schema inference exceeded --timeout");
	const auto expected = ReadFileSchemaSignature(connection, paths.front(), all_varchar);
	for (std::size_t index = 1; index < paths.size(); ++index) {
		control.Checkpoint("schema inference exceeded --timeout");
		if (ReadFileSchemaSignature(connection, paths[index], all_varchar) != expected) {
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
		const fs::path canonical_spill = fs::weakly_canonical(options.spill_directory, error);
		if (error || !fs::is_directory(canonical_spill, error)) {
			throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
			                  "cannot resolve spill directory: " + options.spill_directory.string());
		}
		CheckResult(connection.Query("SET temp_directory = " + SqlString(canonical_spill.string())));
		if (!options.max_spill.empty()) {
			CheckResult(connection.Query("SET max_temp_directory_size = " + SqlString(options.max_spill)));
		}
		const std::vector<fs::path> allowed_directories {canonical_spill};
		CheckResult(connection.Query("SET allowed_directories = " + SqlPathList(allowed_directories)));
	} else {
		CheckResult(connection.Query("SET temp_directory = ''"));
	}

	CheckResult(connection.Query("SET allowed_paths = " + SqlPathList(allowed_paths)));
	CheckResult(connection.Query("SET enable_external_access = false"));
	CheckResult(connection.Query("SET lock_configuration = true"));
}

void BindTables(duckdb::Connection &connection, const std::vector<TableBinding> &tables, ExecutionControl &control,
                const bool strict_schema, const bool all_varchar) {
	for (const auto &table : tables) {
		if (strict_schema) {
			ValidateStrictSchemas(connection, table.paths, table.source, control, all_varchar);
		}
		const std::string sql = "CREATE OR REPLACE TEMP VIEW " + SqlIdentifier(table.name) + " AS SELECT * FROM " +
		                        ReaderExpression(table.paths, strict_schema, all_varchar);
		CheckResult(connection.Query(sql), "TABLE_BIND_FAILED");
	}
}

std::string ValidateSelectQuery(duckdb::Connection &connection, const std::string &sql) {
	std::vector<duckdb::unique_ptr<duckdb::SQLStatement>> statements;
	try {
		statements = connection.ExtractStatements(sql);
	} catch (const std::exception &error) {
		throw SqrailError(EXIT_QUERY, "QUERY_FAILED", duckdb::ErrorData(error).Message());
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

	const auto &rows = result->Collection();
	std::string output = "[";
	output.reserve(rows.Count() * 64U + 2U);
	for (duckdb::idx_t row = 0; row < rows.Count(); ++row) {
		if (row != 0) {
			output.push_back(',');
		}
		const auto name = rows.GetValue(0, row).ToString();
		const auto type = rows.GetValue(1, row).ToString();
		const auto nullable = rows.GetValue(2, row).ToString();
		output += "{\"name\":\"";
		output += JsonEscape(name);
		output += "\",\"type\":\"";
		output += JsonEscape(type);
		output += "\",\"nullable\":";
		output += (nullable == "YES" ? "true" : "false");
		output.push_back('}');
	}
	output.push_back(']');
	return output;
}

std::string InputBindingsJson(const std::vector<TableBinding> &tables) {
	std::string output = "[";
	output.reserve(tables.size() * 40U + 2U);
	for (std::size_t index = 0; index < tables.size(); ++index) {
		if (index != 0) {
			output.push_back(',');
		}
		output += "{\"table\":\"";
		output += JsonEscape(tables[index].name);
		output += "\",\"files\":";
		output += std::to_string(tables[index].paths.size());
		output.push_back('}');
	}
	output.push_back(']');
	return output;
}

} // namespace sqrail
