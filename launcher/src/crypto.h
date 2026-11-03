#pragma once
#include <string>

// Lower-case hex SHA-256 of a file, or "" if it can't be read.
std::string sha256_file(const std::wstring& path);
// DPAPI, tied to this Windows user. Hex in/out so it fits in an .ini file.
std::string protect(const std::string& plain);
std::string unprotect(const std::string& hex);
