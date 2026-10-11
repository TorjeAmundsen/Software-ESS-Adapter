//delayed_sender.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Optional input lag. Ship of Harkinian answers a stick much sooner than Dolphin running Wii VC,
// so a runner who plays both can add lag on SoH to make them feel the same (as pyESS does). Each
// frame goes out a fixed time after it came in, from a thread of its own on a high-resolution
// timer, so the lag is exact instead of rounded to the adapter's report rate.

#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "game.hpp"
#include "gc_adapter.hpp"

class DelayedSender {
public:
    using Send = std::function<void(const AdapterFrame&, Game)>;

    explicit DelayedSender(Send send);
    ~DelayedSender();
    DelayedSender(const DelayedSender&) = delete;
    DelayedSender& operator=(const DelayedSender&) = delete;

    // Sends the frame `lag` from now, or right away (on the calling thread) with no lag. When the
    // lag changes, frames still waiting are dropped, so nothing goes out of order.
    void push(const AdapterFrame& frame, Game game, std::chrono::microseconds lag);

private:
    struct Pending {
        std::chrono::steady_clock::time_point due;
        AdapterFrame frame;
        Game game;
    };

    void run();

    Send send_;
    void* timer_ = nullptr;  // a Windows waitable timer, high resolution where supported
    std::mutex mutex_;       // guards everything below
    std::condition_variable wake_;
    std::deque<Pending> pending_;
    std::chrono::microseconds lag_{0};
    bool stopping_ = false;
    std::thread thread_;
};
