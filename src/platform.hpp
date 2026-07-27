#pragma once

#include <filesystem>

namespace sqrail {

class PrivateCreationMask final {
public:
	PrivateCreationMask();
	PrivateCreationMask(const PrivateCreationMask &) = delete;
	PrivateCreationMask &operator=(const PrivateCreationMask &) = delete;
	~PrivateCreationMask();

private:
	unsigned int previous = 0;
};

void ProtectPrivateFile(const std::filesystem::path &path);
void ProtectPrivateDirectory(const std::filesystem::path &path);
void CommitOutput(const std::filesystem::path &temporary, const std::filesystem::path &output);

} // namespace sqrail
