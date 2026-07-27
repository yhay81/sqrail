#pragma once

#include "cli.hpp"
#include "control.hpp"
#include "duckdb.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sqrail {

struct FileType {
	std::string extension;
	std::string compression;
};

std::string SqlString(const std::string &input);
std::string SqlIdentifier(const std::string &input);
FileType DetectFileType(const std::filesystem::path &path, int exit_code, const std::string &unsupported_code);
std::string ReaderExpression(const std::vector<std::filesystem::path> &paths, bool strict_schema = false);
bool IsJsonOutput(const FileType &type);
std::string UniqueToken();

class SpillWorkspace final {
public:
	explicit SpillWorkspace(const std::filesystem::path &requested_root);
	SpillWorkspace(const SpillWorkspace &) = delete;
	SpillWorkspace &operator=(const SpillWorkspace &) = delete;
	~SpillWorkspace();

	[[nodiscard]] const std::filesystem::path &Path() const;

private:
	std::filesystem::path path;
};

std::vector<std::filesystem::path> ResolveInputSet(duckdb::DuckDB &database, const std::string &source,
                                                   ExecutionControl &control, std::size_t previously_resolved,
                                                   uint64_t max_input_files);
void CheckResult(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &result,
                 const std::string &code = "QUERY_FAILED");
void CheckResult(const duckdb::unique_ptr<duckdb::QueryResult> &result, const std::string &code = "QUERY_FAILED");
void ValidateStrictSchemas(duckdb::Connection &connection, const std::vector<std::filesystem::path> &paths,
                           const std::string &source);
void Configure(duckdb::Connection &connection, const RunOptions &options,
               const std::vector<std::filesystem::path> &allowed_paths);
void BindTables(duckdb::Connection &connection, const std::vector<TableBinding> &tables, bool strict_schema);
std::string ValidateSelectQuery(duckdb::Connection &connection, const std::string &sql);
std::string ResultColumnsJson(duckdb::Connection &connection, const std::string &sql);
std::string InputBindingsJson(const std::vector<TableBinding> &tables);

} // namespace sqrail
