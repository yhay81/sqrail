#pragma once

#include "duckdb.hpp"
#include "engine.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace sqrail {

using ResultWriter = std::function<void(const char *, std::size_t)>;

std::filesystem::path TemporaryOutputPath(const std::filesystem::path &output);
std::string LimitQuery(const std::string &sql, uint64_t max_rows);
void CheckRowLimit(uint64_t rows, uint64_t max_rows);
uint64_t CopyToFile(duckdb::Connection &connection, const std::string &sql, const std::filesystem::path &temporary,
                    const std::filesystem::path &output);
void PrintStats(uint64_t rows, uint64_t bytes, std::size_t input_files, bool file_output,
                std::chrono::steady_clock::time_point started);
uint64_t StreamJson(duckdb::Connection &connection, const std::string &sql, bool array, uint64_t max_rows,
                    const ResultWriter &write);
uint64_t WriteJsonFile(duckdb::DuckDB &database, duckdb::Connection &connection, const std::string &sql,
                       const std::filesystem::path &temporary, const FileType &type, uint64_t max_rows);

} // namespace sqrail
