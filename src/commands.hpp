#ifndef SQRAIL_COMMANDS_HPP
#define SQRAIL_COMMANDS_HPP

#include <string_view>

namespace sqrail {

int Execute(int argc, char **argv, bool check_only);
int Schema(int argc, char **argv);
void PrintHelp();
void PrintError(std::string_view code, std::string_view message) noexcept;

} // namespace sqrail

#endif // SQRAIL_COMMANDS_HPP
