#include "control.hpp"

#include "cli.hpp"
#include "duckdb.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace sqrail {
namespace {

constexpr int EXIT_QUERY = 4;
constexpr int EXIT_OUTPUT = 5;
static_assert(std::atomic<int>::is_always_lock_free, "signal state requires a lock-free atomic integer");
std::atomic<int> interrupted_signal {0};

extern "C" void HandleSignal(const int signal) {
	interrupted_signal.store(signal, std::memory_order_relaxed);
}

#if defined(_WIN32)
BOOL WINAPI HandleConsoleSignal(const DWORD signal) {
	if (signal == CTRL_C_EVENT) {
		interrupted_signal.store(SIGINT, std::memory_order_relaxed);
		return TRUE;
	}
	if (signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_LOGOFF_EVENT ||
	    signal == CTRL_SHUTDOWN_EVENT) {
		interrupted_signal.store(SIGTERM, std::memory_order_relaxed);
		return TRUE;
	}
	return FALSE;
}
#endif

} // namespace

class ExecutionControl::State final {
public:
	State(duckdb::Connection &connection_p, const std::chrono::steady_clock::time_point started,
	      const std::chrono::milliseconds timeout, fs::path monitored_output_p, const uint64_t max_output_bytes_p)
	    : connection(connection_p), deadline(started + timeout), timeout_enabled(timeout.count() > 0),
	      monitored_output(std::move(monitored_output_p)), max_output_bytes(max_output_bytes_p) {
		worker = std::thread([this]() { Watch(); });
	}

	~State() {
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

	void Checkpoint(const std::string &timeout_message) {
		const auto pending_signal = interrupted_signal.load(std::memory_order_relaxed);
		if (pending_signal != 0 && received_signal.load() == 0) {
			received_signal.store(pending_signal);
			connection.Interrupt();
		}
		if (timeout_enabled && std::chrono::steady_clock::now() >= deadline) {
			timed_out.store(true);
			connection.Interrupt();
		}
		CheckOutputLimit();

		if (output_limited.load()) {
			throw SqrailError(EXIT_OUTPUT, "OUTPUT_LIMIT",
			                  "output exceeded --max-output-bytes " + std::to_string(max_output_bytes));
		}
		if (timed_out.load()) {
			throw SqrailError(EXIT_QUERY, "QUERY_TIMEOUT", timeout_message);
		}
		const auto signal = received_signal.load();
		const auto current_signal = interrupted_signal.load(std::memory_order_relaxed);
		if (signal != 0 || current_signal != 0) {
			const auto effective_signal = signal != 0 ? signal : current_signal;
			throw SqrailError(EXIT_QUERY, "QUERY_INTERRUPTED",
			                  "query interrupted by signal " + std::to_string(effective_signal));
		}
	}

private:
	void CheckOutputLimit() {
		if (max_output_bytes == 0 || monitored_output.empty() || output_limited.load()) {
			return;
		}
		std::error_code error;
		const auto bytes = fs::file_size(monitored_output, error);
		if (!error && bytes > max_output_bytes) {
			output_limited.store(true);
			connection.Interrupt();
		}
	}

	void Watch() {
		std::unique_lock<std::mutex> lock(mutex);
		while (!complete) {
			const auto now = std::chrono::steady_clock::now();
			auto wake = now + std::chrono::milliseconds(5);
			if (timeout_enabled) {
				wake = std::min(wake, deadline);
			}
			condition.wait_until(lock, wake, [this]() { return complete; });
			if (complete) {
				return;
			}
			const auto pending_signal = interrupted_signal.load(std::memory_order_relaxed);
			if (pending_signal != 0) {
				received_signal.store(pending_signal);
				lock.unlock();
				connection.Interrupt();
				return;
			}
			if (timeout_enabled && std::chrono::steady_clock::now() >= deadline) {
				timed_out.store(true);
				lock.unlock();
				connection.Interrupt();
				return;
			}
			lock.unlock();
			CheckOutputLimit();
			if (output_limited.load()) {
				return;
			}
			lock.lock();
		}
	}

	duckdb::Connection &connection;
	std::chrono::steady_clock::time_point deadline;
	bool timeout_enabled;
	fs::path monitored_output;
	uint64_t max_output_bytes;
	std::atomic<bool> timed_out {false};
	std::atomic<bool> output_limited {false};
	std::atomic<int> received_signal {0};
	std::condition_variable condition;
	std::mutex mutex;
	std::thread worker;
	bool complete = false;
};

ExecutionControl::ExecutionControl(duckdb::Connection &connection, const std::chrono::steady_clock::time_point started,
                                   const std::chrono::milliseconds timeout, fs::path monitored_output,
                                   const uint64_t max_output_bytes)
    : state(std::make_unique<State>(connection, started, timeout, std::move(monitored_output), max_output_bytes)) {
}

ExecutionControl::~ExecutionControl() = default;

void ExecutionControl::Checkpoint(const std::string &timeout_message) {
	state->Checkpoint(timeout_message);
}

void ExecutionControl::Stop() {
	state->Stop();
}

void InstallSignalHandlers() {
#ifdef SIGPIPE
	if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		throw SqrailError(70, "SIGNAL_HANDLER", "cannot ignore SIGPIPE");
	}
#endif
	if (std::signal(SIGINT, HandleSignal) == SIG_ERR || std::signal(SIGTERM, HandleSignal) == SIG_ERR) {
		throw SqrailError(70, "SIGNAL_HANDLER", "cannot install interrupt handlers");
	}
#if defined(_WIN32)
	if (SetConsoleCtrlHandler(HandleConsoleSignal, TRUE) == 0) {
		throw SqrailError(70, "SIGNAL_HANDLER", "cannot install the Windows console control handler");
	}
#endif
}

} // namespace sqrail
