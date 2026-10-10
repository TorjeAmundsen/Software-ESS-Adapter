//dsu_server.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Minimal cemuhook (DSU) UDP server. Dolphin connects to it through
// Controller Settings > Alternate Input Sources > DSU Client.
// Packet layout matches Dolphin's DualShockUDPProto.h.

#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>

#include "gc_adapter.hpp"

// The stick byte to send so Dolphin's emulated Gamecube pad ends up with `value`. Dolphin reads
// a DSU stick byte v below center as (v-128)/128 but maps it back with a scale of 127
// (GCPadEmu.cpp, MapFloat with neg_1_value 1), so bytes 0 to 64 would arrive one too high.
// Sending one less for those fixes 1 to 64; 0 cannot be reached and arrives as 1.
uint8_t to_dolphin_stick(uint8_t value);

class DsuServer {
public:
    static constexpr uint16_t DEFAULT_PORT = 26760;

    ~DsuServer();

    bool start(uint16_t port);  // On failure, last_error() says why.
    void stop();
    const std::string& last_error() const {
        return error_;
    }

    // Sends the frame to every client subscribed to each connected port.
    void publish(const AdapterFrame& frame);
    int client_count();
    // True if controller data went out to Dolphin within the last half second.
    bool is_transmitting() const;

private:
    struct Client {
        sockaddr_in addr;
        std::chrono::steady_clock::time_point subscribed_until[4];
    };

    void receive_loop();
    void handle_packet(const uint8_t* data, int size, const sockaddr_in& from);
    void send_to(const void* data, int size, const sockaddr_in& to);

    SOCKET socket_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool winsock_started_ = false;
    std::atomic<std::chrono::steady_clock::rep> last_pad_data_{0};  // steady_clock ticks of the last send
    uint32_t server_id_ = 0;
    std::string error_;

    std::mutex mutex_;  // guards everything below
    std::vector<Client> clients_;
    AdapterFrame frame_{};
    uint32_t packet_counter_[4] = {};
};
