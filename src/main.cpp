#include "commands.hpp"
#include "control.hpp"
#include "duckdb.hpp"
#include "error.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
	try {
		sqrail::InstallSignalHandlers();
		if (argc < 2) {
			sqrail::PrintHelp();
			return sqrail::EXIT_USAGE;
		}

		const std::string command = argv[1];
		if (command == "--help" || command == "-h" || command == "help") {
			sqrail::PrintHelp();
			return 0;
		}
		if (command == "--version" || command == "-V") {
			std::cout << "sqrail " << SQRAIL_VERSION << " (DuckDB " << duckdb::DuckDB::LibraryVersion() << ")\n";
			return 0;
		}
		if (command == "schema") {
			return sqrail::Schema(argc, argv);
		}
		if (command == "run") {
			return sqrail::Execute(argc, argv, false);
		}
		if (command == "check") {
			return sqrail::Execute(argc, argv, true);
		}

		throw sqrail::SqrailError(sqrail::EXIT_USAGE, "UNKNOWN_COMMAND", "unknown command: " + command);
	} catch (const sqrail::SqrailError &error) {
		sqrail::PrintError(error.code, error.what());
		return error.exit_code;
	} catch (const std::exception &error) {
		sqrail::PrintError("INTERNAL", error.what());
		return sqrail::EXIT_INTERNAL;
	} catch (...) {
		sqrail::PrintError("INTERNAL", "unknown internal error");
		return sqrail::EXIT_INTERNAL;
	}
}
