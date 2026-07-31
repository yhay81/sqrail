#include "commands.hpp"

#include "cli.hpp"
#include "control.hpp"
#include "duckdb.hpp"
#include "engine.hpp"
#include "json.hpp"
#include "platform.hpp"
#include "result.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace sqrail {
namespace {

constexpr int EXIT_OUTPUT = 5;
constexpr int EXIT_QUERY = 4;

} // namespace

int Execute(int argc, char **argv, const bool check_only) {
	const auto started = std::chrono::steady_clock::now();
	RunOptions options = ParseRun(argc, argv, check_only);
	SpillWorkspace spill_workspace(options.spill_directory);
	options.spill_directory = spill_workspace.Path();
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	const fs::path temporary_output = options.has_output ? TemporaryOutputPath(options.output) : fs::path();
	ExecutionControl control(connection, started, options.timeout, temporary_output, options.max_output_bytes);
	bool committed_output = false;
	try {
		std::vector<fs::path> allowed_paths;
		std::size_t input_files = 0;
		for (auto &table : options.tables) {
			table.paths = ResolveInputSet(database, table.source, control, input_files, options.max_input_files);
			input_files += table.paths.size();
			allowed_paths.insert(allowed_paths.end(), table.paths.begin(), table.paths.end());
		}
		control.Checkpoint("input resolution exceeded --timeout");
		if (options.has_output) {
			allowed_paths.push_back(temporary_output);
		}
		Configure(connection, options, allowed_paths);
		control.Checkpoint("query configuration exceeded --timeout");
		BindTables(connection, options.tables, options.strict_schema);
		control.Checkpoint("schema inference exceeded --timeout");
		options.sql = ValidateSelectQuery(connection, options.sql);
		const std::string execution_sql = LimitQuery(options.sql, options.max_rows);

		if (check_only) {
			const auto columns = ResultColumnsJson(connection, execution_sql);
			auto result = connection.Query("EXPLAIN (FORMAT JSON) " + execution_sql);
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
			control.Checkpoint("query planning exceeded --timeout");
			if (plan.empty()) {
				throw SqrailError(EXIT_QUERY, "PLAN_FAILED", "DuckDB returned an empty physical plan");
			}
			std::cout << "{\"schema_version\":1,\"sqrail_version\":\"" << SQRAIL_VERSION
			          << "\",\"ok\":true,\"columns\":" << columns << ",\"inputs\":" << InputBindingsJson(options.tables)
			          << ",\"plan\":" << StrictJson(plan) << "}\n";
			if (!std::cout) {
				throw SqrailError(EXIT_OUTPUT, "STDOUT_WRITE", "cannot write query plan to stdout");
			}
			control.Checkpoint("query planning exceeded --timeout");
			control.Stop();
			return 0;
		}

		uint64_t rows = 0;
		uint64_t bytes = 0;
		if (options.has_output) {
			const auto type = DetectFileType(options.output, EXIT_OUTPUT, "UNSUPPORTED_OUTPUT");
			if (IsJsonOutput(type)) {
				rows = WriteJsonFile(database, connection, execution_sql, temporary_output, type, options.max_rows);
			} else {
				rows = CopyToFile(connection, execution_sql, temporary_output, options.output);
				CheckRowLimit(rows, options.max_rows);
			}
			control.Checkpoint("query exceeded --timeout");
			ProtectPrivateFile(temporary_output);
			std::error_code size_error;
			bytes = fs::file_size(temporary_output, size_error);
			if (size_error) {
				throw SqrailError(EXIT_OUTPUT, "OUTPUT_STAT",
				                  "cannot measure completed output: " + temporary_output.string() + ": " +
				                      size_error.message());
			}
			if (options.max_output_bytes != 0 && bytes > options.max_output_bytes) {
				throw SqrailError(EXIT_OUTPUT, "OUTPUT_LIMIT",
				                  "output exceeded --max-output-bytes " + std::to_string(options.max_output_bytes));
			}
			control.Checkpoint("output finalization exceeded --timeout");
			CommitOutput(temporary_output, options.output);
			committed_output = true;
			if (options.stats) {
				PrintStats(rows, bytes, input_files, true, started);
			}
			control.Checkpoint("output finalization exceeded --timeout");
			control.Stop();
			return 0;
		}

		const ResultWriter write = [&bytes, &options](const char *data, const std::size_t size) {
			if (options.max_output_bytes != 0 &&
			    (bytes > options.max_output_bytes || size > options.max_output_bytes - bytes)) {
				throw SqrailError(EXIT_OUTPUT, "OUTPUT_LIMIT",
				                  "output exceeded --max-output-bytes " + std::to_string(options.max_output_bytes));
			}
			std::cout.write(data, static_cast<std::streamsize>(size));
			if (!std::cout) {
				throw SqrailError(EXIT_OUTPUT, "STDOUT_WRITE", "cannot write query result to stdout");
			}
			bytes += size;
		};
		rows = StreamJson(connection, execution_sql, false, options.max_rows, write);
		control.Checkpoint("query exceeded --timeout");
		if (options.stats) {
			PrintStats(rows, bytes, input_files, false, started);
		}
		control.Checkpoint("query exceeded --timeout");
		control.Stop();
	} catch (...) {
		control.Stop();
		if (options.has_output) {
			std::error_code ignored;
			fs::remove(temporary_output, ignored);
			if (committed_output) {
				fs::remove(options.output, ignored);
			}
		}
		control.Checkpoint("query exceeded --timeout");
		throw;
	}
	return 0;
}

int Schema(int argc, char **argv) {
	const auto started = std::chrono::steady_clock::now();
	const auto options = ParseSchema(argc, argv);
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	ExecutionControl control(connection, started, options.resources.timeout);
	struct SchemaInput {
		std::string source;
		std::vector<fs::path> paths;
	};
	std::vector<SchemaInput> inputs;
	std::vector<fs::path> allowed_paths;
	inputs.reserve(options.sources.size());

	try {
		for (const auto &raw_source : options.sources) {
			const auto source = fs::absolute(fs::path(raw_source)).lexically_normal().string();
			auto paths =
			    ResolveInputSet(database, source, control, allowed_paths.size(), options.resources.max_input_files);
			allowed_paths.insert(allowed_paths.end(), paths.begin(), paths.end());
			inputs.push_back({source, std::move(paths)});
		}
		control.Checkpoint("input resolution exceeded --timeout");
		Configure(connection, options.resources, allowed_paths);
		control.Checkpoint("schema configuration exceeded --timeout");

		for (std::size_t index = 0; index < inputs.size(); ++index) {
			control.Checkpoint("schema inference exceeded --timeout");
			const auto &input = inputs[index];
			if (options.strict_schema) {
				ValidateStrictSchemas(connection, input.paths, input.source);
			}
			const std::string view_name = "__sqrail_schema_" + std::to_string(index);
			const std::string bind_sql = "CREATE TEMP VIEW " + SqlIdentifier(view_name) + " AS SELECT * FROM " +
			                             ReaderExpression(input.paths, options.strict_schema);
			CheckResult(connection.Query(bind_sql), "SCHEMA_INFERENCE_FAILED");

			auto result = connection.Query("DESCRIBE SELECT * FROM " + SqlIdentifier(view_name));
			CheckResult(result, "SCHEMA_INFERENCE_FAILED");

			std::ostringstream output;
			output << "{\"schema_version\":1,\"sqrail_version\":\"" << SQRAIL_VERSION << "\",\"file\":\""
			       << JsonEscape(input.source) << "\",\"files\":" << input.paths.size() << ",\"columns\":[";
			bool first = true;
			while (true) {
				auto chunk = result->Fetch();
				if (!chunk) {
					break;
				}
				for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
					if (!first) {
						output << ',';
					}
					first = false;
					const auto name = chunk->GetValue(0, row).ToString();
					const auto type = chunk->GetValue(1, row).ToString();
					const auto nullable = chunk->GetValue(2, row).ToString();
					output << "{\"name\":\"" << JsonEscape(name) << "\",\"type\":\"" << JsonEscape(type)
					       << "\",\"nullable\":" << (nullable == "YES" ? "true" : "false") << '}';
				}
			}
			output << "]}\n";
			std::cout << output.str();
			if (!std::cout) {
				throw SqrailError(EXIT_OUTPUT, "STDOUT_WRITE", "cannot write schema result to stdout");
			}
		}
		control.Checkpoint("schema inference exceeded --timeout");
		control.Stop();
	} catch (...) {
		control.Stop();
		control.Checkpoint("schema inference exceeded --timeout");
		throw;
	}
	return 0;
}

void PrintHelp() {
	std::cout
	    << "sqrail runs read-only SQL over CSV, TSV, JSON, and Parquet.\n"
	    << "\n"
	    << "sqrail schema [--memory SIZE] [--threads N] [--timeout DURATION]\n"
	    << "              [--max-input-files N] [--strict-schema] FILE...\n"
	    << "sqrail check [-t NAME=PATH]... [--memory SIZE] [--threads N] [--timeout DURATION]\n"
	    << "             [--max-rows N] [--max-input-files N] [--max-sql-bytes SIZE]\n"
	    << "             [--strict-schema] [SQL|-]\n"
	    << "sqrail run [-t NAME=PATH]... [-o FILE] [--memory SIZE] [--threads N]\n"
	    << "           [--spill DIR [--max-spill SIZE]] [--timeout DURATION] [--max-rows N]\n"
	    << "           [--max-output-bytes SIZE] [--max-input-files N] [--max-sql-bytes SIZE]\n"
	    << "           [--stats] [--strict-schema] [SQL|-]\n"
	    << "sqrail --help\n"
	    << "sqrail --version\n"
	    << "\n"
	    << "Names/types result: schema once. Otherwise trust stated names/types, run once, and stop after success.\n"
	    << "-t binds a file, Parquet directory, or glob; '-' reads SQL; '--' ends options.\n"
	    << "Files union columns by name; --strict-schema requires exact schemas.\n"
	    << "check emits columns, inputs, and plan without execution.\n"
	    << "No -o: JSONL stdout. -o: extension selects format.\n"
	    << "Limits fail closed; --stats writes versioned success JSON to stderr.\n"
	    << "Text supports .gz/.zst. Outputs are private, atomic, never overwritten.\n"
	    << "SQL is one SELECT, VALUES, or WITH; order needs ORDER BY.\n"
	    << "Errors are one stderr JSON. Exit: 0 success, 2 usage, 3 input, 4 SQL, 5 output, 70 internal.\n"
	    << "\n"
	    << "Examples:\n"
	    << "  sqrail schema sales.csv\n"
	    << "  sqrail run -t sales=sales.csv 'SELECT count(*) AS n FROM sales'\n"
	    << "  sqrail run -t sales=sales.csv -o result.parquet - < query.sql\n";
}

void PrintError(const std::string &code, const std::string &message) {
	std::cerr << "{\"schema_version\":1,\"sqrail_version\":\"" << SQRAIL_VERSION << "\",\"ok\":false,\"code\":\""
	          << JsonEscape(code) << "\",\"message\":\"" << JsonEscape(message) << "\"}\n";
}

} // namespace sqrail
