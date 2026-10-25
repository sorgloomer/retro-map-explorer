// Read-only virtual file system backed by files the user dropped into the page
// (or by a dev server). The JavaScript side only provides: list, size and ranged read.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace vfs {

struct Entry {
    std::string path;   // original spelling, '/' separated
    std::string lower;  // lower-cased path used for case-insensitive lookup
    uint64_t size = 0;
};

// Re-pulls the file list from JavaScript.
void refresh();
const std::vector<Entry>& files();

// Case-insensitive lookup. Returns -1 if absent.
int find(const std::string& path);
// Looks for a file by name only (case-insensitive) inside directory `dir` (no trailing slash).
int findIn(const std::string& dir, const std::string& name);
// Lists the (case preserved) immediate sub-directory names of `dir` (path as prefix, '' for root).
std::vector<std::string> subdirs(const std::string& dir);
// All entries whose lower-cased path starts with `prefixLower`.
std::vector<int> withPrefix(const std::string& prefixLower);

// Reads a whole file / a byte range. Suspends (Asyncify) while the browser reads the data.
bool read(int idx, std::vector<uint8_t>& out);
bool readRange(int idx, uint64_t off, uint64_t len, std::vector<uint8_t>& out);
bool readText(const std::string& path, std::string& out);

std::string lower(const std::string& s);
std::string dirOf(const std::string& path);   // "a/b/c.txt" -> "a/b"
std::string baseOf(const std::string& path);  // "a/b/c.txt" -> "c.txt"
std::string stemOf(const std::string& path);  // "a/b/c.txt" -> "c"
std::string join(const std::string& a, const std::string& b);

}  // namespace vfs
