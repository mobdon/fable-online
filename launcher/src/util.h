#pragma once
#include <windows.h>
#include <string>

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);
std::wstring dir_of(const std::wstring& path);
std::wstring self_path();
std::wstring self_dir();
bool file_exists(const std::wstring& path);
bool dir_exists(const std::wstring& path);
// Creates every missing folder in path.
bool make_dirs(const std::wstring& path);
