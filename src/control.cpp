#include "control.hpp"

#include "duckdb.hpp"
#include "error.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sqrail {
namespace {

static_assert(std::atomic<int>::is_always_lock_free, "signal state requires a lock-free atomic integer");
std::atomic<int> interrupted_signal {0};

#if defined(_WIN32)
std::atomic<HANDLE> signal_event {nullptr};
#else
std::atomic<int> signal_pipe_read {-1};
std::atomic<int> signal_pipe_write {-1};

bool SetNonBlockingCloexec(const int fd) {
	const int fd_flags = ::fcntl(fd, F_GETFD);
	if (fd_flags == -1) {
		return false;
	}
	const auto cloexec = static_cast<int>(static_cast<unsigned int>(fd_flags) | static_cast<unsigned int>(FD_CLOEXEC));
	if (::fcntl(fd, F_SETFD, cloexec) == -1) {
		return false;
	}
	const int status_flags = ::fcntl(fd, F_GETFL);
	if (status_flags == -1) {
		return false;
	}
	const auto nonblock =
	    static_cast<int>(static_cast<unsigned int>(status_flags) | static_cast<unsigned int>(O_NONBLOCK));
	if (::fcntl(fd, F_SETFL, nonblock) == -1) {
		return false;
	}
	return true;
}

bool CreateWakePipe(std::array<int, 2> &fds) {
	if (::pipe(fds.data()) != 0) {
		fds = {-1, -1};
		return false;
	}
	if (!SetNonBlockingCloexec(fds[0]) || !SetNonBlockingCloexec(fds[1])) {
		(void)::close(fds[0]);
		(void)::close(fds[1]);
		fds = {-1, -1};
		return false;
	}
	return true;
}
#endif

extern "C" void HandleSignal(const int signal) {
	interrupted_signal.store(signal, std::memory_order_relaxed);
#if defined(_WIN32)
	const HANDLE event = signal_event.load(std::memory_order_relaxed);
	if (event != nullptr) {
		(void)SetEvent(event);
	}
#else
	const int fd = signal_pipe_write.load(std::memory_order_relaxed);
	if (fd >= 0) {
		const int saved_errno = errno;
		const char byte = 1;
		const ssize_t ignored = ::write(fd, &byte, 1U);
		(void)ignored;
		errno = saved_errno;
	}
#endif
}

#if defined(_WIN32)
BOOL WINAPI HandleConsoleSignal(const DWORD signal) {
	if (signal == CTRL_C_EVENT) {
		interrupted_signal.store(SIGINT, std::memory_order_relaxed);
		if (const HANDLE event = signal_event.load(std::memory_order_relaxed); event != nullptr) {
			(void)SetEvent(event);
		}
		return TRUE;
	}
	if (signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_LOGOFF_EVENT ||
	    signal == CTRL_SHUTDOWN_EVENT) {
		interrupted_signal.store(SIGTERM, std::memory_order_relaxed);
		if (const HANDLE event = signal_event.load(std::memory_order_relaxed); event != nullptr) {
			(void)SetEvent(event);
		}
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
	      file_monitoring_enabled(max_output_bytes_p != 0 && !monitored_output_p.empty()),
	      monitored_output(std::move(monitored_output_p)), max_output_bytes(max_output_bytes_p) {
#if defined(_WIN32)
		stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (stop_event == nullptr) {
			throw SqrailError(EXIT_INTERNAL, "INTERNAL", "cannot create the Windows execution control event");
		}
#else
		if (!CreateWakePipe(stop_pipe)) {
			throw SqrailError(EXIT_INTERNAL, "INTERNAL", "cannot create execution control wake pipe");
		}
#endif
		worker = std::thread([this]() { Watch(); });
	}

	~State() {
		Stop();
#if defined(_WIN32)
		if (stop_event != nullptr) {
			CloseHandle(stop_event);
			stop_event = nullptr;
		}
#else
		if (stop_pipe[0] >= 0) {
			(void)::close(stop_pipe[0]);
		}
		if (stop_pipe[1] >= 0) {
			(void)::close(stop_pipe[1]);
		}
#endif
	}

	void Stop() {
		std::lock_guard<std::mutex> lock(stop_mutex);
		bool expected = false;
		if (complete.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
#if defined(_WIN32)
			(void)SetEvent(stop_event);
#else
			const char byte = 1;
			const ssize_t ignored = ::write(stop_pipe[1], &byte, 1U);
			(void)ignored;
#endif
		}
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
	int ComputeWaitTimeoutMs() const {
		int timeout_ms = -1;
		if (file_monitoring_enabled) {
			timeout_ms = 5;
		}
		if (timeout_enabled) {
			const auto now = std::chrono::steady_clock::now();
			int remaining_ms = 0;
			if (now < deadline) {
				const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
				constexpr auto max_int = static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max());
				remaining_ms = remaining > max_int ? std::numeric_limits<int>::max() : static_cast<int>(remaining);
			}
			timeout_ms = timeout_ms < 0 ? remaining_ms : std::min(timeout_ms, remaining_ms);
		}
		return timeout_ms;
	}

	void WaitForEvent(const int timeout_ms) const {
#if defined(_WIN32)
		std::array<HANDLE, 2> handles {stop_event, nullptr};
		DWORD count = 1;
		const HANDLE sig_event = signal_event.load(std::memory_order_acquire);
		if (sig_event != nullptr) {
			handles[1] = sig_event;
			count = 2;
		}
		const DWORD wait_ms = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
		(void)WaitForMultipleObjects(count, handles.data(), FALSE, wait_ms);
#else
		std::array<struct pollfd, 2> fds {};
		fds[0].fd = stop_pipe[0];
		fds[0].events = POLLIN;
		nfds_t count = 1;
		const int sig_fd = signal_pipe_read.load(std::memory_order_acquire);
		if (sig_fd >= 0) {
			fds[1].fd = sig_fd;
			fds[1].events = POLLIN;
			count = 2;
		}
		const int ready = ::poll(fds.data(), count, timeout_ms);
		const bool signal_ready =
		    count == 2 && (static_cast<unsigned int>(fds[1].revents) & static_cast<unsigned int>(POLLIN)) != 0U;
		if (ready > 0 && signal_ready && interrupted_signal.load(std::memory_order_relaxed) == 0) {
			std::array<char, 64> scratch {};
			while (::read(sig_fd, scratch.data(), scratch.size()) > 0) {
			}
		}
#endif
	}

	void WaitOnStopOnly(const int timeout_ms) const {
#if defined(_WIN32)
		const DWORD wait_ms = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
		(void)WaitForSingleObject(stop_event, wait_ms);
#else
		struct pollfd fd {};
		fd.fd = stop_pipe[0];
		fd.events = POLLIN;
		(void)::poll(&fd, 1, timeout_ms);
#endif
	}

	void CheckOutputLimit() {
		if (!file_monitoring_enabled || output_limited.load()) {
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
		while (!complete.load(std::memory_order_acquire)) {
			const auto pending_signal = interrupted_signal.load(std::memory_order_relaxed);
			if (pending_signal != 0) {
				received_signal.store(pending_signal);
				break;
			}
			if (timeout_enabled && std::chrono::steady_clock::now() >= deadline) {
				timed_out.store(true);
				break;
			}
			CheckOutputLimit();
			if (output_limited.load()) {
				break;
			}
			WaitForEvent(ComputeWaitTimeoutMs());
		}
		// DuckDB clears a pending interrupt whenever a statement starts, so a
		// single request is lost between statements. Repeat it until Stop().
		while (!complete.load(std::memory_order_acquire)) {
			connection.Interrupt();
			WaitOnStopOnly(5);
		}
	}

	duckdb::Connection &connection;
	std::chrono::steady_clock::time_point deadline;
	bool timeout_enabled;
	bool file_monitoring_enabled;
	fs::path monitored_output;
	uint64_t max_output_bytes;
	std::atomic<bool> timed_out {false};
	std::atomic<bool> output_limited {false};
	std::atomic<int> received_signal {0};
	std::atomic<bool> complete {false};
#if defined(_WIN32)
	HANDLE stop_event = nullptr;
#else
	std::array<int, 2> stop_pipe {-1, -1};
#endif
	std::mutex stop_mutex;
	std::thread worker;
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
#if defined(_WIN32)
	if (signal_event.load(std::memory_order_acquire) == nullptr) {
		const HANDLE created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (created == nullptr) {
			throw SqrailError(EXIT_INTERNAL, "SIGNAL_HANDLER", "cannot create the Windows signal event");
		}
		HANDLE expected = nullptr;
		if (!signal_event.compare_exchange_strong(expected, created, std::memory_order_acq_rel)) {
			CloseHandle(created);
		}
	}
#else
	if (signal_pipe_read.load(std::memory_order_acquire) < 0) {
		std::array<int, 2> fds {-1, -1};
		if (!CreateWakePipe(fds)) {
			throw SqrailError(EXIT_INTERNAL, "SIGNAL_HANDLER", "cannot create signal wake pipe");
		}
		int expected = -1;
		if (signal_pipe_read.compare_exchange_strong(expected, fds[0], std::memory_order_acq_rel)) {
			signal_pipe_write.store(fds[1], std::memory_order_release);
		} else {
			(void)::close(fds[0]);
			(void)::close(fds[1]);
		}
	}
#endif
#ifdef SIGPIPE
	if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		throw SqrailError(EXIT_INTERNAL, "SIGNAL_HANDLER", "cannot ignore SIGPIPE");
	}
#endif
	if (std::signal(SIGINT, HandleSignal) == SIG_ERR || std::signal(SIGTERM, HandleSignal) == SIG_ERR) {
		throw SqrailError(EXIT_INTERNAL, "SIGNAL_HANDLER", "cannot install interrupt handlers");
	}
#if defined(_WIN32)
	if (SetConsoleCtrlHandler(HandleConsoleSignal, TRUE) == 0) {
		throw SqrailError(EXIT_INTERNAL, "SIGNAL_HANDLER", "cannot install the Windows console control handler");
	}
#endif
}

} // namespace sqrail
