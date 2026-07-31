#pragma once

#include <string>

namespace sqrail {

int Execute(int argc, char **argv, bool check_only);
int Schema(int argc, char **argv);
void PrintHelp();
void PrintError(const std::string &code, const std::string &message);

} // namespace sqrail
