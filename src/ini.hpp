//ini.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// The settings file: [Section] headers and Key=Value lines, read and written the way Windows'
// GetPrivateProfileString and WritePrivateProfileString do it (section and key names ignore case,
// spaces around names and values don't count), so files from older versions keep working.

#pragma once
#include <optional>
#include <string>

// The value, or nothing if the file, section, or key is missing.
std::optional<std::string> ini_read(const std::string& path, const std::string& section, const std::string& key);

// The value as a whole number, or `fallback` if it is missing or not a number.
int ini_read_int(const std::string& path, const std::string& section, const std::string& key, int fallback);

// Sets the key, adding the section and file if needed. No value removes the key.
void ini_write(const std::string& path, const std::string& section, const std::string& key, const std::optional<std::string>& value);
