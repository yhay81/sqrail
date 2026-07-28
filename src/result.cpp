#include "result.hpp"

#include "cli.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/file_system.hpp"
#include "json.hpp"
#include "platform.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace sqrail {
namespace {

constexpr int EXIT_QUERY = 4;
constexpr int EXIT_OUTPUT = 5;
constexpr int EXIT_INTERNAL = 70;

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

duckdb::FileCompressionType CompressionTypeFor(const FileType &type) {
	if (type.compression == "GZIP") {
		return duckdb::FileCompressionType::GZIP;
	}
	if (type.compression == "ZSTD") {
		return duckdb::FileCompressionType::ZSTD;
	}
	return duckdb::FileCompressionType::UNCOMPRESSED;
}

} // namespace

fs::path TemporaryOutputPath(const fs::path &output) {
	fs::path temporary = output;
	temporary += ".sqrail-tmp-" + UniqueToken();
	return temporary;
}

std::string LimitQuery(const std::string &sql, const uint64_t max_rows) {
	if (max_rows == 0) {
		return sql;
	}
	return "SELECT * FROM (" + sql + ") AS __sqrail_limited LIMIT " + std::to_string(max_rows + 1U);
}

void CheckRowLimit(const uint64_t rows, const uint64_t max_rows) {
	if (max_rows != 0 && rows > max_rows) {
		throw SqrailError(EXIT_QUERY, "RESULT_LIMIT", "query result exceeded --max-rows " + std::to_string(max_rows));
	}
}

uint64_t CopyToFile(duckdb::Connection &connection, const std::string &sql, const fs::path &temporary,
                    const fs::path &output) {
	PrivateCreationMask private_creation;
	const std::string copy_sql =
	    "COPY (" + sql + ") TO " + SqlString(temporary.string()) + " (" + CopyOptionsFor(output) + ")";
	auto result = connection.Query(copy_sql);
	CheckResult(result);
	if (result->RowCount() != 1 || result->ColumnCount() != 1) {
		throw SqrailError(EXIT_INTERNAL, "INTERNAL", "DuckDB COPY returned an unexpected result shape");
	}
	const auto rows = result->GetValue(0, 0).GetValue<int64_t>();
	if (rows < 0) {
		throw SqrailError(EXIT_INTERNAL, "INTERNAL", "DuckDB COPY returned a negative row count");
	}
	return static_cast<uint64_t>(rows);
}

void PrintStats(const uint64_t rows, const uint64_t bytes, const std::size_t input_files, const bool file_output,
                const std::chrono::steady_clock::time_point started) {
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	std::cerr << "{\"schema_version\":1,\"sqrail_version\":\"" << SQRAIL_VERSION
	          << "\",\"ok\":true,\"command\":\"run\",\"rows\":" << rows << ",\"bytes\":" << bytes
	          << ",\"elapsed_ms\":" << elapsed << ",\"input_files\":" << input_files << ",\"destination\":\""
	          << (file_output ? "file" : "stdout") << "\"}\n";
}

uint64_t StreamJson(duckdb::Connection &connection, const std::string &sql, const bool array, const uint64_t max_rows,
                    const ResultWriter &write) {
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
	try {
		while (true) {
			auto chunk = result->Fetch();
			if (!chunk) {
				break;
			}
			duckdb::UnifiedVectorFormat vector_format;
			chunk->data[0].ToUnifiedFormat(chunk->size(), vector_format);
			const auto *values = duckdb::UnifiedVectorFormat::GetData<duckdb::string_t>(vector_format);
			for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
				if (max_rows != 0 && rows >= max_rows) {
					throw SqrailError(EXIT_QUERY, "RESULT_LIMIT",
					                  "query result exceeded --max-rows " + std::to_string(max_rows));
				}
				if (array && rows != 0) {
					buffer.push_back(',');
				}
				const auto index = vector_format.sel->get_index(row);
				if (!vector_format.validity.RowIsValid(index)) {
					append("null");
				} else {
					const auto &value = values[index];
					const std::string_view json(value.GetData(), value.GetSize());
					if (json.find("NaN") == std::string_view::npos && json.find("Infinity") == std::string_view::npos) {
						append(json);
					} else {
						append(StrictJson(std::string(json)));
					}
				}
				if (!array) {
					buffer.push_back('\n');
				}
				++rows;
			}
		}
	} catch (...) {
		connection.Interrupt();
		(void)result->Fetch();
		throw;
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
                       const fs::path &temporary, const FileType &type, const uint64_t max_rows) {
	auto flags = duckdb::FileFlags::FILE_FLAGS_WRITE | duckdb::FileFlags::FILE_FLAGS_FILE_CREATE |
	             duckdb::FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE | duckdb::FileFlags::FILE_FLAGS_PRIVATE |
	             duckdb::FileOpenFlags(CompressionTypeFor(type));
	auto &file_system = database.instance->GetFileSystem();
	auto handle = file_system.OpenFile(temporary.string(), flags);
	const ResultWriter write = [&](const char *data, const std::size_t size) {
		handle->Write(const_cast<char *>(data), size);
	};
	const auto rows = StreamJson(connection, sql, type.extension == ".json", max_rows, write);
	handle->Close();
	return rows;
}

} // namespace sqrail
