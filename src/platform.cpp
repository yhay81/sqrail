#include "platform.hpp"

#include "cli.hpp"

#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <aclapi.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace sqrail {
namespace {

constexpr int EXIT_OUTPUT = 5;

#if defined(_WIN32)
std::string WindowsMessage(const DWORD code) {
	char *raw = nullptr;
	const auto length =
	    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                   nullptr, code, 0, reinterpret_cast<char *>(&raw), 0, nullptr);
	std::string message =
	    length != 0 && raw != nullptr ? std::string(raw, length) : "Windows error " + std::to_string(code);
	if (raw != nullptr) {
		LocalFree(raw);
	}
	while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
		message.pop_back();
	}
	return message;
}

void ProtectWindowsPath(const fs::path &path, const bool directory) {
	HANDLE token = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_PERMISSIONS",
		                  "cannot read the Windows process token: " + WindowsMessage(GetLastError()));
	}

	DWORD required = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &required);
	std::string buffer(required, '\0');
	if (required == 0 || GetTokenInformation(token, TokenUser, buffer.data(), required, &required) == 0) {
		const auto code = GetLastError();
		CloseHandle(token);
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_PERMISSIONS",
		                  "cannot read the Windows user SID: " + WindowsMessage(code));
	}
	CloseHandle(token);

	const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer.data());
	EXPLICIT_ACCESSW access {};
	access.grfAccessPermissions = FILE_ALL_ACCESS;
	access.grfAccessMode = SET_ACCESS;
	access.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
	access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
	access.Trustee.TrusteeType = TRUSTEE_IS_USER;
	access.Trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);

	PACL acl = nullptr;
	const auto acl_status = SetEntriesInAclW(1, &access, nullptr, &acl);
	if (acl_status != ERROR_SUCCESS) {
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_PERMISSIONS",
		                  "cannot create a private Windows ACL: " + WindowsMessage(acl_status));
	}
	const auto status = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
	                                          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
	                                          nullptr, acl, nullptr);
	LocalFree(acl);
	if (status != ERROR_SUCCESS) {
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_PERMISSIONS",
		                  "cannot protect path " + path.string() + ": " + WindowsMessage(status));
	}
}
#endif

} // namespace

PrivateCreationMask::PrivateCreationMask() {
#if !defined(_WIN32)
	previous = static_cast<unsigned int>(::umask(0077));
#endif
}

PrivateCreationMask::~PrivateCreationMask() {
#if !defined(_WIN32)
	::umask(static_cast<mode_t>(previous));
#endif
}

void ProtectPrivateFile(const fs::path &path) {
#if defined(_WIN32)
	ProtectWindowsPath(path, false);
#else
	std::error_code error;
	fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, error);
	if (error) {
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_PERMISSIONS",
		                  "cannot protect output " + path.string() + ": " + error.message());
	}
#endif
}

void ProtectPrivateDirectory(const fs::path &path) {
#if defined(_WIN32)
	ProtectWindowsPath(path, true);
#else
	std::error_code error;
	fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace, error);
	if (error) {
		throw SqrailError(EXIT_OUTPUT, "SPILL_DIRECTORY",
		                  "cannot protect private spill directory: " + path.string() + ": " + error.message());
	}
#endif
}

void CommitOutput(const fs::path &temporary, const fs::path &output) {
#if defined(_WIN32)
	if (MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH) != 0) {
		return;
	}
	const auto code = GetLastError();
	std::error_code ignored;
	if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS || fs::exists(output, ignored)) {
		fs::remove(temporary, ignored);
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_EXISTS", "output appeared during execution: " + output.string());
	}
	fs::remove(temporary, ignored);
	throw SqrailError(EXIT_OUTPUT, "OUTPUT_COMMIT",
	                  "cannot commit output: " + output.string() + ": " + WindowsMessage(code));
#else
	std::error_code error;
	fs::create_hard_link(temporary, output, error);
	if (error) {
		std::error_code ignored;
		fs::remove(temporary, ignored);
		if (fs::exists(output, ignored)) {
			throw SqrailError(EXIT_OUTPUT, "OUTPUT_EXISTS", "output appeared during execution: " + output.string());
		}
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_COMMIT",
		                  "cannot commit output: " + output.string() + ": " + error.message());
	}
	fs::remove(temporary, error);
	if (error) {
		std::error_code rollback_error;
		fs::remove(output, rollback_error);
		throw SqrailError(EXIT_OUTPUT, "OUTPUT_COMMIT",
		                  "output was linked but its temporary name could not be removed: " + temporary.string() +
		                      ": " + error.message());
	}
#endif
}

} // namespace sqrail
