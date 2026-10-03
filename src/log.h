#pragma once
#include <string>

void LogInit(const std::wstring& dir);
void Log(const char* fmt, ...);

// Directory that holds dinput8.dll (the game's x64 folder), with trailing backslash.
const std::wstring& ModuleDir();
