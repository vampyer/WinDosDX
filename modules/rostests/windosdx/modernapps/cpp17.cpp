/*
 * PROJECT:     WinDosDX modern application tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     A C++17 program on the dynamic Visual C++ runtime
 *              (msvcp140 + vcruntime140, shipped next to the test).
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int failures;

#define CHECK(expr) \
    do { if (!(expr)) { std::cout << "  FAIL line " << __LINE__ << ": " #expr "\n"; failures++; } } while (0)

int main()
{
    std::cout << "cpp17\n";

    // Containers, strings, structured bindings, optional
    std::map<std::string, int> counts{ {"a", 1}, {"b", 2} };
    int sum = 0;
    for (const auto& [key, value] : counts)
        sum += value;
    CHECK(sum == 3);
    std::optional<int> maybe = counts.count("b") ? std::optional<int>(counts["b"]) : std::nullopt;
    CHECK(maybe && *maybe == 2);
    std::ostringstream os;
    os << "x=" << 42;
    CHECK(os.str() == "x=42");

    // Exceptions across the runtime
    bool caught = false;
    try
    {
        throw std::runtime_error("boom");
    }
    catch (const std::exception& e)
    {
        caught = std::string(e.what()) == "boom";
    }
    CHECK(caught);

    // Threads, mutex, atomics
    std::mutex m;
    std::atomic<int> atom{0};
    int guarded = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 1000; ++i)
            {
                std::lock_guard<std::mutex> lock(m);
                ++guarded;
                ++atom;
            }
        });
    for (auto& th : threads)
        th.join();
    CHECK(guarded == 4000 && atom == 4000);

    // std::filesystem
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec) / "wdx_cpp17";
    CHECK(!ec);
    std::filesystem::create_directories(dir, ec);
    CHECK(!ec);
    auto file = dir / "hello.txt";
    {
        std::ofstream out(file);
        out << "hello";
    }
    CHECK(std::filesystem::exists(file) && std::filesystem::file_size(file) == 5);
    std::filesystem::remove_all(dir, ec);
    CHECK(!std::filesystem::exists(dir));

    std::cout << (failures ? "FAIL" : "PASS") << " cpp17\n";
    return failures ? 1 : 0;
}
