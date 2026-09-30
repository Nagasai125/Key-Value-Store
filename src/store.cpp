#include "kv/store.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace kv {
namespace {
constexpr std::size_t max_key = 1024 * 1024;
constexpr std::size_t max_value = 16 * 1024 * 1024;
constexpr std::size_t record_header = 21;
using Value = std::optional<std::string>;
using Memtable = std::map<std::string, Value, std::less<>>;

[[noreturn]] void io_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct File {
    int fd;
    explicit File(const std::filesystem::path& path, int flags)
        : fd(::open(path.c_str(), flags | O_CLOEXEC, 0600)) {
        if (fd < 0) io_error("open");
    }
    ~File() { ::close(fd); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
void persist(int fd) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR) io_error("fsync");
    }
}
void write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0) { if (errno == EINTR) continue; io_error("write"); }
        require(n != 0, "write made no progress");
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
}
std::uint32_t checksum(std::string_view bytes, std::uint32_t hash = 2166136261u) {
    for (unsigned char c : bytes) hash = (hash ^ c) * 16777619u;
    return hash;
}
void append_number(std::string& out, std::uint64_t n, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<char>((n >> (8 * i)) & 255));
}
std::uint64_t number(std::string_view bytes, std::size_t offset, unsigned width) {
    std::uint64_t n = 0;
    for (unsigned i = 0; i < width; ++i)
        n |= std::uint64_t(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return n;
}
void validate(std::string_view key, std::string_view value = {}) {
    if (key.size() > max_key || value.size() > max_value)
        throw std::invalid_argument("key exceeds 1 MiB or value exceeds 16 MiB");
}
std::string encode(std::string_view key, std::optional<std::string_view> value) {
    std::string out = "KVR1";
    out.reserve(record_header + key.size() + (value ? value->size() : 0));
    out.push_back(value ? 1 : 2);
    append_number(out, key.size(), 4);
    append_number(out, value ? value->size() : 0, 4);
    append_number(out, checksum(out), 4);
    append_number(out, checksum(value.value_or(""), checksum(key)), 4);
    out.append(key);
    if (value) out.append(*value);
    return out;
}
struct Record { std::string_view key; std::optional<std::string_view> value; };
// An incomplete final WAL record is recoverable; a complete corrupt record isn't.
bool decode(std::string_view bytes, std::size_t& pos, Record& record) {
    if (bytes.size() - pos < record_header) return false;
    const auto header = bytes.substr(pos, record_header);
    require(header.substr(0, 4) == "KVR1", "corrupt record magic");
    require(checksum(header.substr(0, 13)) == number(header, 13, 4),
            "record header checksum mismatch");
    const auto type = static_cast<unsigned char>(header[4]);
    const auto k = number(header, 5, 4), v = number(header, 9, 4);
    require((type == 1 || type == 2) && k <= max_key && v <= max_value &&
            (type == 1 || v == 0), "corrupt record lengths or type");
    if (bytes.size() - pos - record_header < k + v) return false;
    const auto body = bytes.substr(pos + record_header, static_cast<std::size_t>(k + v));
    require(checksum(body) == number(header, 17, 4),
            "record checksum mismatch");
    record = {body.substr(0, static_cast<std::size_t>(k)),
              type == 1 ? std::optional<std::string_view>(body.substr(static_cast<std::size_t>(k)))
                        : std::nullopt};
    pos += record_header + static_cast<std::size_t>(k + v);
    return true;
}
struct Mapping {
    void* data = MAP_FAILED;
    std::size_t size = 0;
    explicit Mapping(int fd) {
        struct stat st {};
        if (::fstat(fd, &st) != 0) io_error("fstat");
        require(st.st_size >= 0 && static_cast<std::uintmax_t>(st.st_size) <=
                    std::numeric_limits<std::size_t>::max(), "file too large");
        size = static_cast<std::size_t>(st.st_size);
        if (size) {
            data = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
            if (data == MAP_FAILED) io_error("mmap");
        }
    }
    ~Mapping() { if (data != MAP_FAILED) ::munmap(data, size); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    std::string_view bytes() const {
        return size ? std::string_view(static_cast<const char*>(data), size) : std::string_view{};
    }
};
std::string table_name(std::uint64_t id) { return "sst-" + std::to_string(id) + ".kv"; }
struct Table {
    std::uint64_t id;
    File file;
    Mapping mapping;
    std::vector<Record> index;
    Table(const std::filesystem::path& directory, std::uint64_t table_id)
        : id(table_id), file(directory / table_name(id), O_RDONLY), mapping(file.fd) {
        const auto bytes = mapping.bytes();
        require(bytes.size() >= 12 && bytes.substr(0, 4) == "KVS1", "corrupt table header");
        const auto count = number(bytes, 4, 8);
        require(count <= (bytes.size() - 12) / record_header, "truncated table");
        index.reserve(static_cast<std::size_t>(count));
        std::size_t pos = 12;
        for (std::uint64_t i = 0; i < count; ++i) {
            Record record;
            require(decode(bytes, pos, record), "truncated table record");
            require(index.empty() || index.back().key < record.key, "unsorted table");
            index.push_back(record);
        }
        require(pos == bytes.size(), "unexpected table trailing bytes");
    }
    const Record* find(std::string_view key) const {
        const auto it = std::lower_bound(index.begin(), index.end(), key,
            [](const Record& record, std::string_view k) { return record.key < k; });
        return it != index.end() && it->key == key ? &*it : nullptr;
    }
};
std::filesystem::path prepare(const std::filesystem::path& directory) {
    // Require an existing parent so its directory entry can be persisted explicitly.
    auto path = std::filesystem::absolute(directory).lexically_normal();
    if (std::filesystem::create_directory(path)) {
        File parent(path.parent_path(), O_RDONLY | O_DIRECTORY);
        persist(parent.fd);
    }
    return path;
}
} // namespace

struct Store::Impl {
    std::filesystem::path directory;
    Options options;
    File directory_fd;
    File lock;
    std::unique_ptr<File> wal;
    mutable std::shared_mutex mutex;
    Memtable memtable;
    std::size_t memtable_size = 0;
    std::size_t wal_bytes = 0;
    std::vector<std::unique_ptr<Table>> tables; // Oldest to newest.
    std::uint64_t next_id = 1;
    bool failed = false;

    Impl(const std::filesystem::path& path, Options opts)
        : directory(prepare(path)), options(opts), directory_fd(directory, O_RDONLY | O_DIRECTORY),
          lock(directory / "LOCK", O_RDWR | O_CREAT) {
        if (options.memtable_bytes == 0 || options.max_tables < 2)
            throw std::invalid_argument("memtable_bytes must be positive and max_tables >= 2");
        if (::flock(lock.fd, LOCK_EX | LOCK_NB) != 0) io_error("database already open (flock)");
        load_manifest();
        // Include orphan files left by interrupted publication when allocating IDs.
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const std::string name = entry.path().filename().string();
            if (name.size() <= 7 || name.substr(0, 4) != "sst-" ||
                name.substr(name.size() - 3) != ".kv") continue;
            std::uint64_t id = 0;
            const auto parsed = std::from_chars(name.data() + 4, name.data() + name.size() - 3, id);
            require(parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() - 3 &&
                    id < std::numeric_limits<std::uint64_t>::max(), "invalid table filename");
            next_id = std::max(next_id, id + 1);
        }
        wal = std::make_unique<File>(directory / "WAL", O_RDWR | O_CREAT | O_APPEND);
        persist(directory_fd.fd);
        {
            std::size_t pos = 0;
            std::size_t size = 0;
            {
                Mapping mapping(wal->fd);
                const auto bytes = mapping.bytes();
                size = bytes.size();
                while (pos < bytes.size()) {
                    Record record;
                    if (!decode(bytes, pos, record)) break;
                    apply(record.key, record.value);
                }
            }
            wal_bytes = pos;
            // Unmap before truncating the incomplete tail.
            if (pos != size) {
                if (::ftruncate(wal->fd, static_cast<off_t>(pos)) != 0) io_error("truncate WAL tail");
                persist(wal->fd);
            }
        }
        if (memtable_size >= options.memtable_bytes || wal_bytes >= options.memtable_bytes) flush_locked();
        if (tables.size() >= options.max_tables) compact_locked();
    }
    void healthy() const { require(!failed, "store encountered an I/O failure; close and reopen it"); }
    void apply(std::string_view key, std::optional<std::string_view> value) {
        auto [it, inserted] = memtable.try_emplace(std::string(key), std::nullopt);
        if (inserted) memtable_size += key.size() + record_header;
        if (it->second) memtable_size -= it->second->size();
        it->second = value ? Value(std::string(*value)) : std::nullopt;
        if (it->second) memtable_size += it->second->size();
    }
    void load_manifest() {
        const auto path = directory / "MANIFEST";
        if (!std::filesystem::exists(path)) {
            // A missing manifest with existing data is ambiguous: don't silently discard it.
            for (const auto& entry : std::filesystem::directory_iterator(directory))
                require(entry.path().filename() == "LOCK" || entry.path().filename() == "MANIFEST.tmp",
                        "missing manifest in nonempty database");
            publish({});
            return;
        }
        File file(path, O_RDONLY);
        Mapping mapping(file.fd);
        const auto bytes = mapping.bytes();
        require(bytes.size() >= 12 && bytes.substr(0, 4) == "KVM1", "corrupt manifest");
        const auto count = number(bytes, 4, 4);
        require(count <= 1000000 && bytes.size() == 12 + count * 8, "corrupt manifest length");
        require(checksum(bytes.substr(0, bytes.size() - 4)) == number(bytes, bytes.size() - 4, 4),
                "manifest checksum mismatch");
        std::set<std::uint64_t> seen;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto id = number(bytes, 8 + static_cast<std::size_t>(i) * 8, 8);
            require(id != 0 && seen.insert(id).second, "duplicate or invalid table ID");
            tables.push_back(std::make_unique<Table>(directory, id));
        }
    }
    void publish(const std::vector<std::uint64_t>& ids) {
        require(ids.size() <= 1000000, "too many tables");
        std::string bytes = "KVM1";
        append_number(bytes, ids.size(), 4);
        for (auto id : ids) append_number(bytes, id, 8);
        append_number(bytes, checksum(bytes), 4);
        File file(directory / "MANIFEST.tmp", O_WRONLY | O_CREAT | O_TRUNC);
        write_all(file.fd, bytes);
        persist(file.fd);
        std::filesystem::rename(directory / "MANIFEST.tmp", directory / "MANIFEST");
        persist(directory_fd.fd);
    }
    std::unique_ptr<Table> write_table(const Memtable& entries) {
        require(next_id < std::numeric_limits<std::uint64_t>::max(), "table IDs exhausted");
        const auto id = next_id++;
        File file(directory / table_name(id), O_WRONLY | O_CREAT | O_EXCL);
        std::string buffer = "KVS1";
        append_number(buffer, entries.size(), 8);
        for (const auto& [key, value] : entries) {
            buffer += encode(key, value ? std::optional<std::string_view>(*value) : std::nullopt);
            if (buffer.size() >= 1024 * 1024) { write_all(file.fd, buffer); buffer.clear(); }
        }
        write_all(file.fd, buffer);
        persist(file.fd);
        persist(directory_fd.fd);
        return std::make_unique<Table>(directory, id);
    }
    void flush_locked() {
        if (memtable.empty()) return;
        auto table = write_table(memtable);
        std::vector<std::uint64_t> ids;
        for (const auto& existing : tables) ids.push_back(existing->id);
        ids.push_back(table->id);
        publish(ids);
        tables.push_back(std::move(table));
        // Publication is durable before WAL reclamation; replaying a duplicate is safe.
        if (::ftruncate(wal->fd, 0) != 0) io_error("truncate WAL");
        persist(wal->fd);
        memtable.clear();
        memtable_size = 0;
        wal_bytes = 0;
    }
    void compact_locked() {
        if (tables.size() < 2) return;
        // ponytail: full merge uses O(live data) RAM and blocks operations; use a streaming
        // background leveled merge when databases outgrow memory or stalls matter.
        Memtable merged;
        for (const auto& table : tables)
            for (const auto& entry : table->index) {
                if (entry.value) merged[std::string(entry.key)] = std::string(*entry.value);
                else merged.erase(std::string(entry.key));
            }
        auto table = write_table(merged);
        publish({table->id});
        for (const auto& old : tables) std::filesystem::remove(directory / table_name(old->id));
        tables.clear();
        tables.push_back(std::move(table));
        persist(directory_fd.fd);
    }
    void mutate(std::string_view key, std::optional<std::string_view> value) {
        validate(key, value.value_or(""));
        std::unique_lock<std::shared_mutex> guard(mutex);
        healthy();
        try {
            const auto record = encode(key, value);
            write_all(wal->fd, record);
            wal_bytes += record.size();
            if (options.durability == Durability::Sync) persist(wal->fd);
            apply(key, value);
            if (memtable_size >= options.memtable_bytes || wal_bytes >= options.memtable_bytes) {
                flush_locked();
                if (tables.size() >= options.max_tables) compact_locked();
            }
        } catch (...) { failed = true; throw; }
    }
};

Store::Store(const std::filesystem::path& directory, Options options)
    : impl_(std::make_unique<Impl>(directory, options)) {}
Store::~Store() = default;
void Store::put(std::string_view key, std::string_view value) { impl_->mutate(key, value); }
void Store::erase(std::string_view key) { impl_->mutate(key, std::nullopt); }
std::optional<std::string> Store::get(std::string_view key) const {
    validate(key);
    std::shared_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    const auto it = impl_->memtable.find(key);
    if (it != impl_->memtable.end()) return it->second;
    for (auto table = impl_->tables.rbegin(); table != impl_->tables.rend(); ++table) {
        if (const auto* record = (*table)->find(key))
            return record->value ? Value(std::string(*record->value)) : std::nullopt;
    }
    return std::nullopt;
}
void Store::sync() {
    std::unique_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    try { persist(impl_->wal->fd); }
    catch (...) { impl_->failed = true; throw; }
}
void Store::flush() {
    std::unique_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    try {
        impl_->flush_locked();
        if (impl_->tables.size() >= impl_->options.max_tables) impl_->compact_locked();
    } catch (...) { impl_->failed = true; throw; }
}
void Store::compact() {
    std::unique_lock<std::shared_mutex> guard(impl_->mutex);
    impl_->healthy();
    try { impl_->flush_locked(); impl_->compact_locked(); }
    catch (...) { impl_->failed = true; throw; }
}
} // namespace kv
