// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Named stop points, so that a test decides the interleaving instead of hoping for it.
// A thread that reaches an armed gate blocks until the test releases it. A released gate
// never blocks again.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

namespace diag {

class Gates {
public:
    void Arm(const std::string& name) {
        std::lock_guard lock{m_};
        armed_.insert(name);
    }
    void Pass(const char* name) {
        std::unique_lock lock{m_};
        if (armed_.count(name) == 0 || released_.count(name) != 0) {
            return;
        }
        reached_.insert(name);
        cv_.notify_all();
        if (!cv_.wait_for(lock, std::chrono::seconds{20},
                          [&] { return released_.count(name) != 0; })) {
            std::fprintf(stderr, "gate %s was never released\n", name);
            std::abort();
        }
    }
    void WaitReached(const std::string& name) {
        std::unique_lock lock{m_};
        if (!cv_.wait_for(lock, std::chrono::seconds{20},
                          [&] { return reached_.count(name) != 0; })) {
            std::fprintf(stderr, "gate %s was never reached\n", name.c_str());
            std::abort();
        }
    }
    void Release(const std::string& name) {
        std::lock_guard lock{m_};
        released_.insert(name);
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::set<std::string> armed_;
    std::set<std::string> reached_;
    std::set<std::string> released_;
};

inline void Pass(Gates* gates, const char* name) {
    if (gates != nullptr) {
        gates->Pass(name);
    }
}

} // namespace diag
