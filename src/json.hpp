#ifndef SQRAIL_JSON_HPP
#define SQRAIL_JSON_HPP

#include <string>
#include <string_view>

namespace sqrail {

std::string JsonEscape(std::string_view input);
bool IsValidUtf8(std::string_view input);

bool HasNonFiniteJsonToken(std::string_view input);
std::string RewriteNonFiniteJson(std::string_view input);

// DuckDB follows the common JavaScript extension that emits NaN and infinities
// as bare tokens. RFC 8259 does not permit those tokens. sqrail maps them to
// null while leaving occurrences inside JSON strings untouched.
std::string StrictJson(std::string input);

} // namespace sqrail

#endif // SQRAIL_JSON_HPP
