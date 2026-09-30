#include "kv/store.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
struct TemporaryDatabase {
    std::filesystem::path directory;
    TemporaryDatabase() {
        auto pattern = (std::filesystem::temp_directory_path() / "kv-bench-XXXXXX").string();
        const auto result = ::mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        directory = result;
    }
    ~TemporaryDatabase() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
};
std::size_t parse(const char* arg) {
    const std::string_view text(arg);
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value == 0 || value > 10000000)
        throw std::invalid_argument("counts must be integers in [1, 10000000]");
    return value;
}
template<class Operation, class Finish>
void measure(const char* name, const char* mode, std::size_t count, Operation operation, Finish finish) {
    std::vector<std::int64_t> samples(count);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        const auto before = Clock::now();
        operation(i);
        samples[i] = std::chrono::duration_cast<Nanoseconds>(Clock::now() - before).count();
    }
    finish();
    const auto seconds = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double p) {
        return static_cast<double>(samples[static_cast<std::size_t>(std::ceil(p * count)) - 1]) / 1000.0;
    };
    std::cout << name << ',' << mode << ',' << count << ',' << std::fixed << std::setprecision(6)
              << seconds << ',' << count / seconds << ',' << percentile(0.50) << ','
              << percentile(0.95) << ',' << percentile(0.99) << ',' << samples.back() / 1000.0 << '\n';
}
int main(int argc, char** argv) {
    try {
        if (argc > 4) throw std::invalid_argument("usage: kv_bench [keys=100000] [operations=1000000] [buffered|sync]");
        const auto keys_count = argc > 1 ? parse(argv[1]) : 100000;
        const auto operations = argc > 2 ? parse(argv[2]) : 1000000;
        const std::string mode = argc > 3 ? argv[3] : "buffered";
        if (mode != "buffered" && mode != "sync") throw std::invalid_argument("mode must be buffered or sync");
        TemporaryDatabase temporary;
        kv::Options options;
        options.durability = mode == "sync" ? kv::Durability::Sync : kv::Durability::Buffered;
        kv::Store db(temporary.directory / "db", options);
        std::vector<std::string> keys, missing;
        keys.reserve(keys_count); missing.reserve(keys_count);
        for (std::size_t i = 0; i < keys_count; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "key-%012zu", 2 * i);
            keys.emplace_back(key);
            std::snprintf(key, sizeof(key), "key-%012zu", 2 * i + 1);
            missing.emplace_back(key);
        }
        const std::string value(128, 'v');
        std::mt19937 rng(42);
        std::uniform_int_distribution<std::size_t> uniform(0, keys_count - 1);
        std::vector<std::size_t> requests(operations);
        for (auto& request : requests) request = uniform(rng);
        std::cerr << "keys=" << keys_count << " operations=" << operations
                  << " key_bytes=16 value_bytes=128 threads=1 seed=42 durability=" << mode
                  << " memtable_bytes=" << options.memtable_bytes << " max_tables=" << options.max_tables << '\n';
        std::cout << "workload,durability,operations,seconds,ops_per_second,p50_us,p95_us,p99_us,max_us\n";
        measure("sequential_put", mode.c_str(), keys_count,
            [&](std::size_t i) { db.put(keys[i], value); }, [&] { db.sync(); });
        db.compact();
        for (const auto& key : keys) {
            if (db.get(key) != value) throw std::runtime_error("warmup verification failed");
        }
        measure("random_get_hit_warm_sstable", mode.c_str(), operations, [&](std::size_t i) {
            if (db.get(keys[requests[i]]) != value) throw std::runtime_error("read verification failed");
        }, [] {});
        measure("random_get_miss_warm_sstable", mode.c_str(), operations, [&](std::size_t i) {
            if (db.get(missing[requests[i]])) throw std::runtime_error("miss verification failed");
        }, [] {});
        measure("mixed_90read_10write", mode.c_str(), operations, [&](std::size_t i) {
            const auto& key = keys[requests[i]];
            if (i % 10 == 0) db.put(key, value);
            else if (db.get(key) != value) throw std::runtime_error("mixed verification failed");
        }, [&] { db.sync(); });
    } catch (const std::exception& error) {
        std::cerr << "kv_bench: " << error.what() << '\n';
        return 1;
    }
}
