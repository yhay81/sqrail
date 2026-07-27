#pragma once

#include <string>

namespace sqrail {

int Execute(int argc, char **argv, bool check_only);
int Schema(int argc, char **argv);
void PrintAgentHelp();
void PrintHumanHelp();
void PrintError(const std::string &code, const std::string &message);

} // namespace sqrail
