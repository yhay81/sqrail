#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sqrail {

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
	std::vector<std::filesystem::path> paths;
};

struct RunOptions {
	std::vector<TableBinding> tables;
	std::string sql;
	std::filesystem::path output;
	std::filesystem::path spill_directory;
	std::string memory_limit;
	std::string max_spill;
	std::chrono::milliseconds timeout {0};
	uint64_t threads = 0;
	uint64_t max_rows = 0;
	uint64_t max_output_bytes = 0;
	uint64_t max_input_files = 0;
	uint64_t max_sql_bytes = 0;
	bool has_output = false;
	bool stats = false;
	bool strict_schema = false;
};

struct SchemaOptions {
	RunOptions resources;
	std::vector<std::string> sources;
	bool strict_schema = false;
};

RunOptions ParseRun(int argc, char **argv, bool check_only);
SchemaOptions ParseSchema(int argc, char **argv);
std::string NormalizeQuery(std::string sql);

} // namespace sqrail
