// Text helpers for the Carmageddon loader: line / quote / comment handling and the key-value descriptor files.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace carma {

std::vector<std::string> splitLines(const std::string& t);  // trimmed lines (CR, indentation and trailing blanks removed)
std::vector<std::string> quotedStrings(const std::string& line);
std::string stripComment(const std::string& l);  // drops "// ..." outside quotes
bool endsWith(const std::string& s, const char* suf);

// key -> remaining text of the first line that starts with the key (comments removed).
struct KeyFile {
    std::vector<std::pair<std::string, std::string>> items;
    void parse(const std::string& text);
    const std::string* get(const char* key) const;
    std::string str(const char* key) const;
    bool nums(const char* key, float* out, int n) const;
};

}  // namespace carma
