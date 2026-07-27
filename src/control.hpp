#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace duckdb {
class Connection;
}

namespace sqrail {

void InstallSignalHandlers();

class ExecutionControl final {
public:
	ExecutionControl(duckdb::Connection &connection, std::chrono::steady_clock::time_point started,
	                 std::chrono::milliseconds timeout, std::filesystem::path monitored_output = {},
	                 uint64_t max_output_bytes = 0);
	ExecutionControl(const ExecutionControl &) = delete;
	ExecutionControl &operator=(const ExecutionControl &) = delete;
	~ExecutionControl();

	void Checkpoint(const std::string &timeout_message);
	void Stop();

private:
	class State;
	std::unique_ptr<State> state;
};

} // namespace sqrail
