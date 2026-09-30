#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace kv {

enum class Durability { Sync, Buffered };

struct Options {
    std::size_t memtable_bytes = 4 * 1024 * 1024;
    std::size_t max_tables = 4;
    Durability durability = Durability::Sync;
};

// One process per directory; one Store may be shared by multiple threads.
// Operations throw std::exception on invalid input, corruption, or I/O errors.
class Store {
public:
    explicit Store(const std::filesystem::path& directory, Options options = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void put(std::string_view key, std::string_view value);
    void erase(std::string_view key);
    std::optional<std::string> get(std::string_view key) const;
    void sync();     // Persist the WAL, including previous buffered writes.
    void flush();    // Checkpoint the memtable to a sorted table.
    void compact();  // Flush and merge all tables, dropping obsolete versions.

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kv
