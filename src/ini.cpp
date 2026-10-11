//ini.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "ini.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
static const char* const NEWLINE = "\r\n";
#else
static const char* const NEWLINE = "\n";
#endif

static std::string trim(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return "";
    }
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

static bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream file(path, std::ios::binary);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// The section name if the line is a [Section] header.
static std::optional<std::string> section_of(const std::string& line) {
    const std::string text = trim(line);
    if (text.size() < 2 || text.front() != '[') {
        return std::nullopt;
    }
    const size_t end = text.find(']');
    return end == std::string::npos ? std::nullopt : std::optional(trim(text.substr(1, end - 1)));
}

// Whether the line is Key=Value for this key.
static bool is_key(const std::string& line, const std::string& key) {
    const size_t equals = line.find('=');
    return equals != std::string::npos && same_name(trim(line.substr(0, equals)), key);
}

std::optional<std::string> ini_read(const std::string& path, const std::string& section, const std::string& key) {
    bool in_section = false;
    for (const std::string& line : read_lines(path)) {
        if (const auto name = section_of(line)) {
            in_section = same_name(*name, section);
        } else if (in_section && is_key(line, key)) {
            return trim(line.substr(line.find('=') + 1));
        }
    }
    return std::nullopt;
}

int ini_read_int(const std::string& path, const std::string& section, const std::string& key, int fallback) {
    const auto value = ini_read(path, section, key);
    if (!value) {
        return fallback;
    }
    char* end;
    const long number = std::strtol(value->c_str(), &end, 10);
    return end == value->c_str() ? fallback : static_cast<int>(number);
}

void ini_write(const std::string& path, const std::string& section, const std::string& key, const std::optional<std::string>& value) {
    std::vector<std::string> lines = read_lines(path);
    const std::string entry = key + "=" + value.value_or("");

    // Find the section, and the key or the end of the section's entries within it.
    size_t section_line = lines.size(), insert_at = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        if (const auto name = section_of(lines[i])) {
            if (section_line < lines.size()) {
                break;  // past the end of the section
            }
            if (same_name(*name, section)) {
                section_line = i;
                insert_at = i + 1;
            }
        } else if (section_line < lines.size()) {
            if (is_key(lines[i], key)) {
                if (value) {
                    lines[i] = entry;
                } else {
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
                }
                insert_at = lines.size() + 1;  // done
                break;
            }
            if (!trim(lines[i]).empty()) {
                insert_at = i + 1;
            }
        }
    }
    if (insert_at <= lines.size() && value) {
        if (section_line == lines.size()) {
            lines.push_back("[" + section + "]");
            insert_at = lines.size();
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), entry);
    }

    std::ostringstream text;
    for (const std::string& line : lines) {
        text << line << NEWLINE;
    }
    std::ofstream(path, std::ios::binary) << text.str();
}
