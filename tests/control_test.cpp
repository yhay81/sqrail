#include "cli.hpp"
#include "control.hpp"
#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

TEST_CASE("an expired deadline interrupts a statement that starts afterwards", "[control]") {
	duckdb::DuckDB database(nullptr);
	duckdb::Connection connection(database);
	sqrail::ExecutionControl control(connection, std::chrono::steady_clock::now(), std::chrono::milliseconds(1));

	// Let the monitor request the interrupt while no statement is active.
	// DuckDB clears that request when the next statement starts.
	const auto wait_limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!connection.context->IsInterrupted() && std::chrono::steady_clock::now() < wait_limit) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(connection.context->IsInterrupted());

	const auto started = std::chrono::steady_clock::now();
	const auto result = connection.Query(
	    "SELECT sum(left_side.i * right_side.i) FROM range(1000000) left_side(i), range(1000000) right_side(i)");
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

	CAPTURE(elapsed.count());
	REQUIRE(result->HasError());
	CHECK(result->GetErrorType() == duckdb::ExceptionType::INTERRUPT);
	CHECK(elapsed < std::chrono::seconds(5));

	bool timed_out = false;
	try {
		control.Checkpoint("query exceeded --timeout");
	} catch (const sqrail::SqrailError &error) {
		timed_out = error.code == "QUERY_TIMEOUT";
	}
	CHECK(timed_out);
}
