//delayed_sender.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "delayed_sender.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>

// Newer Windows SDK value that older mingw-w64 headers don't have.
#if defined(_WIN32) && !defined(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

DelayedSender::DelayedSender(Send send) : send_(std::move(send)) {
#ifdef _WIN32
    // Without high-resolution support (before Windows 10 1803) a normal timer still works, just
    // less precisely.
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) {
        timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
#endif
    thread_ = std::thread(&DelayedSender::run, this);
}

DelayedSender::~DelayedSender() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    thread_.join();
#ifdef _WIN32
    CloseHandle(timer_);
#endif
}

void DelayedSender::push(const AdapterFrame& frame, Game game, std::chrono::microseconds lag) {
    {
        std::lock_guard lock(mutex_);
        if (lag != lag_) {
            pending_.clear();
            lag_ = lag;
        }
        if (lag.count() > 0) {
            pending_.push_back({std::chrono::steady_clock::now() + lag, frame, game});
        }
    }
    if (lag.count() > 0) {
        wake_.notify_one();
    } else {
        send_(frame, game);
    }
}

void DelayedSender::run() {
    // Waits at most this long at a time, so a frame that arrives while it waits never waits for
    // an older, dropped one.
    constexpr auto LONGEST_WAIT = std::chrono::milliseconds(5);
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (pending_.empty()) {
            wake_.wait(lock);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (pending_.front().due <= now) {
            const Pending next = pending_.front();
            pending_.pop_front();
            lock.unlock();
            send_(next.frame, next.game);
            lock.lock();
            continue;
        }
        const auto wait = std::min<std::chrono::steady_clock::duration>(pending_.front().due - now, LONGEST_WAIT);
        lock.unlock();
#ifdef _WIN32
        LARGE_INTEGER due;  // negative = relative, in 100 ns units
        due.QuadPart = -std::max<LONGLONG>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count() / 100);
        SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer_, INFINITE);
#else
        std::this_thread::sleep_for(wait);  // already precise on Linux
#endif
        lock.lock();
    }
}
