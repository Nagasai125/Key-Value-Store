#include "kv/store.hpp"
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

struct Temp {
    fs::path path;
    Temp() {
        auto pattern = (fs::temp_directory_path() / "kv-test-XXXXXX").string();
        const auto result = ::mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~Temp() { std::error_code ec; fs::remove_all(path, ec); }
};
template<class F> void throws(F f) {
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    CHECK(caught);
}
kv::Options buffered(std::size_t bytes = 1024) {
    return {bytes, 3, kv::Durability::Buffered};
}
std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(file.good());
}
void basics() {
    Temp temp;
    const auto path = temp.path / "db";
    const std::string binary("a\0b", 3);
    {
        kv::Store db(path, buffered());
        CHECK(!db.get("missing"));
        db.put("", "");
        db.put(binary, binary);
        db.put("key", "old");
        db.flush();
        db.put("key", "new");
        CHECK(db.get("key") == "new");
        db.flush();
        db.erase("key");
        db.erase("absent");
        CHECK(!db.get("key"));
        db.compact();
        CHECK(db.get("") == "");
        CHECK(db.get(binary) == binary);
        throws([&] { kv::Store second(path); });
        throws([&] { db.put(std::string(1024 * 1024 + 1, 'x'), "x"); });
        throws([&] { db.put("x", std::string(16 * 1024 * 1024 + 1, 'x')); });
        CHECK(db.get(binary) == binary); // Invalid input doesn't poison the store.
        db.sync();
    }
    {
        kv::Store db(path);
        CHECK(!db.get("key"));
        CHECK(!db.get("absent"));
        CHECK(db.get(binary) == binary);
        db.put("key", "resurrected");
    }
    CHECK(kv::Store(path).get("key") == "resurrected");
    throws([&] { kv::Store db(temp.path / "invalid", {0, 4, kv::Durability::Sync}); });
}
void model() {
    Temp temp;
    std::map<std::string, std::string> expected;
    std::mt19937 rng(42);
    for (int epoch = 0; epoch < 8; ++epoch) {
        kv::Store db(temp.path / "db", buffered(2048));
        for (int i = 0; i < 700; ++i) {
            const auto key = std::to_string(rng() % 150);
            if (rng() % 4 == 0) { db.erase(key); expected.erase(key); }
            else {
                const auto value = std::to_string(rng());
                db.put(key, value);
                expected[key] = value;
            }
            if (i % 131 == 0) db.flush();
            if (i % 211 == 0) db.compact();
            const auto it = expected.find(key);
            CHECK(db.get(key) == (it == expected.end() ? std::nullopt :
                                  std::optional<std::string>(it->second)));
        }
        for (int k = 0; k < 150; ++k) {
            const auto key = std::to_string(k);
            const auto it = expected.find(key);
            CHECK(db.get(key) == (it == expected.end() ? std::nullopt :
                                  std::optional<std::string>(it->second)));
        }
    }
}
void wal_recovery() {
    Temp temp;
    const auto original = temp.path / "original";
    std::size_t prefix = 0;
    {
        kv::Store db(original, buffered(1024 * 1024));
        db.put("saved", "yes");
        prefix = fs::file_size(original / "WAL");
        db.put("tail", "a value with enough bytes to truncate");
    }
    const auto wal = read(original / "WAL");
    for (std::size_t end = prefix; end < wal.size(); ++end) {
        const auto path = temp.path / ("cut-" + std::to_string(end));
        fs::copy(original, path, fs::copy_options::recursive);
        write(path / "WAL", wal.substr(0, end));
        {
            kv::Store db(path, buffered());
            CHECK(db.get("saved") == "yes");
            CHECK(!db.get("tail"));
            CHECK(fs::file_size(path / "WAL") == prefix);
            db.put("after", "recovery");
        }
        CHECK(kv::Store(path).get("after") == "recovery");
    }
    for (std::size_t byte : {std::size_t(0), std::size_t(5), std::size_t(17), wal.size() - 1}) {
        const auto path = temp.path / ("corrupt-" + std::to_string(byte));
        fs::copy(original, path, fs::copy_options::recursive);
        auto bad = wal;
        bad[byte] ^= 1;
        write(path / "WAL", bad);
        throws([&] { kv::Store db(path); });
        CHECK(read(path / "WAL") == bad); // Corruption must not be silently repaired.
    }
}
void publication_recovery() {
    Temp temp;
    const auto path = temp.path / "db";
    std::string duplicate;
    {
        kv::Store db(path, buffered());
        db.put("old", "value");
        db.flush();
        db.erase("old");
        db.put("new", "value");
        duplicate = read(path / "WAL");
        db.flush();
    }
    // Simulate a crash after manifest publication, before WAL truncation.
    write(path / "WAL", duplicate);
    write(path / "sst-999.kv", "unfinished orphan");
    write(path / "MANIFEST.tmp", "unfinished manifest");
    {
        kv::Store db(path, buffered());
        CHECK(!db.get("old"));
        CHECK(db.get("new") == "value");
        db.compact();
        CHECK(!db.get("old"));
    }
    CHECK(kv::Store(path).get("new") == "value");
    CHECK(read(path / "sst-999.kv") == "unfinished orphan");
}
void corruption() {
    Temp temp;
    const auto original = temp.path / "original";
    {
        kv::Store db(original);
        db.put("a", "1"); db.put("b", "2"); db.flush();
    }
    for (const auto& name : {"MANIFEST", "sst-1.kv"}) {
        const auto path = temp.path / name;
        fs::copy(original, path, fs::copy_options::recursive);
        auto bytes = read(path / name);
        bytes.back() ^= 1;
        write(path / name, bytes);
        throws([&] { kv::Store db(path); });
        write(path / name, read(original / name).substr(0, 12));
        throws([&] { kv::Store db(path); });
    }
    fs::remove(original / "MANIFEST");
    throws([&] { kv::Store db(original); });
}
void io_failure() {
    Temp temp;
    const auto path = temp.path / "db";
    {
        kv::Store db(path, buffered());
        db.put("saved", "yes");
        fs::create_directory(path / "MANIFEST.tmp");
        throws([&] { db.flush(); });
        throws([&] { db.get("saved"); });
        throws([&] { db.put("unsafe", "no"); });
    }
    fs::remove(path / "MANIFEST.tmp");
    kv::Store db(path);
    CHECK(db.get("saved") == "yes");
    CHECK(!db.get("unsafe"));
    db.flush();
}
void overwrite_bound() {
    Temp temp;
    kv::Store db(temp.path / "db", buffered(512));
    for (int i = 0; i < 200; ++i) db.put("same", std::to_string(i));
    CHECK(db.get("same") == "199");
    CHECK(fs::file_size(temp.path / "db" / "WAL") < 512);
}
void concurrency() {
    Temp temp;
    kv::Store db(temp.path / "db", buffered(4096));
    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
        try {
            for (int i = 0; i < 250; ++i) {
                const auto key = std::to_string(t) + ":" + std::to_string(i);
                db.put(key, key);
                if (db.get(key) != key) ok = false;
                if (i % 3 == 0) db.erase(key);
                if (i % 79 == 0) db.compact();
            }
        } catch (...) { ok = false; }
    });
    for (auto& thread : threads) thread.join();
    CHECK(ok);
    for (int t = 0; t < 4; ++t) for (int i = 0; i < 250; ++i) {
        const auto key = std::to_string(t) + ":" + std::to_string(i);
        CHECK(db.get(key) == (i % 3 == 0 ? std::nullopt : std::optional<std::string>(key)));
    }
}
void process_crash() {
    Temp temp;
    for (int run = 0; run < 6; ++run) {
        const auto path = temp.path / std::to_string(run);
        int ack[2]; CHECK(::pipe(ack) == 0);
        const auto pid = ::fork(); CHECK(pid >= 0);
        if (pid == 0) {
            ::close(ack[0]);
            try {
                auto options = buffered(512);
                if (run % 2 == 0) options.durability = kv::Durability::Sync;
                kv::Store db(path, options);
                for (int i = 0; ; ++i) {
                    db.put(std::to_string(i), "acknowledged");
                    if (::write(ack[1], "x", 1) != 1) ::_exit(3);
                }
            } catch (...) { ::_exit(2); }
        }
        ::close(ack[1]);
        int acknowledged = 0;
        char byte;
        while (acknowledged < 40 + run * 7 && ::read(ack[0], &byte, 1) == 1) ++acknowledged;
        ::kill(pid, SIGKILL);
        int status = 0;
        CHECK(::waitpid(pid, &status, 0) == pid);
        ::close(ack[0]);
        CHECK(acknowledged == 40 + run * 7);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        kv::Store recovered(path);
        for (int i = 0; i < acknowledged; ++i)
            CHECK(recovered.get(std::to_string(i)) == "acknowledged");
    }
}
int main() {
    try {
        for (const auto& test : std::vector<std::pair<const char*, void(*)()>>{
            {"basic operations and validation", basics}, {"randomized reference model", model},
            {"WAL torn tails and checksums", wal_recovery}, {"publication recovery", publication_recovery},
            {"table and manifest corruption", corruption}, {"I/O failure and reopen", io_failure},
            {"bounded overwrite WAL", overwrite_bound}, {"concurrent operations", concurrency},
            {"SIGKILL recovery", process_crash}}) {
            test.second();
            std::cout << "PASS " << test.first << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
