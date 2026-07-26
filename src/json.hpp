#pragma once

#include <string>

namespace sqrail {

std::string JsonEscape(const std::string &input);

// DuckDB follows the common JavaScript extension that emits NaN and infinities
// as bare tokens. RFC 8259 does not permit those tokens. sqrail maps them to
// null while leaving occurrences inside JSON strings untouched.
std::string StrictJson(std::string input);

} // namespace sqrail
