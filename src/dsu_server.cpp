//dsu_server.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "dsu_server.hpp"

#include <ws2tcpip.h>

#include <algorithm>
#include <cstring>
#include <random>

static constexpr uint16_t PROTOCOL_VERSION = 1001;
static constexpr uint32_t MSG_VERSION = 0x100000;
static constexpr uint32_t MSG_PORT_INFO = 0x100001;
static constexpr uint32_t MSG_PAD_DATA = 0x100002;
// Dolphin re-subscribes every second. Drop clients that stop asking.
static constexpr auto SUBSCRIPTION_TIMEOUT = std::chrono::seconds(5);

#pragma pack(push, 1)
struct Header {
    char magic[4];
    uint16_t protocol_version;
    uint16_t message_length;  // packet size minus this header
    uint32_t crc32;
    uint32_t id;
    uint32_t message_type;
};

struct PortShared {
    uint8_t pad_id;
    uint8_t state;       // 0 disconnected, 2 connected
    uint8_t model;       // 2 = DS4
    uint8_t connection;  // 1 = USB
    uint8_t mac[6];
    uint8_t battery;  // 5 = full
};

struct VersionResponse {
    Header header;
    uint16_t max_protocol_version;
    uint8_t padding[2];
};

struct PortInfo {
    Header header;
    PortShared port;
    uint8_t padding;
};

struct PadData {
    Header header;
    PortShared port;
    uint8_t active;
    uint32_t packet_counter;
    uint8_t buttons1;  // Share L3 R3 Options Up Right Down Left
    uint8_t buttons2;  // L2 R2 L1 R1 Triangle Circle Cross Square
    uint8_t ps;
    uint8_t touch;
    uint8_t left_x, left_y, right_x, right_y;  // Y: 255 = up, like the Gamecube
    uint8_t dpad_left, dpad_down, dpad_right, dpad_up;
    uint8_t square, cross, circle, triangle;
    uint8_t r1, l1, r2, l2;
    uint8_t touch_data[12];
    uint64_t motion_timestamp;
    float accel[3];
    float gyro[3];
};
#pragma pack(pop)

static_assert(sizeof(Header) == 20, "DSU header layout");
static_assert(sizeof(PortInfo) == 32, "DSU port info layout");
static_assert(sizeof(PadData) == 100, "DSU pad data layout");

static uint32_t crc32(const void* data, size_t size) {
    static uint32_t table[256] = {};
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

template <typename T>
static void finish(T& msg, uint32_t type, uint32_t server_id) {
    std::memcpy(msg.header.magic, "DSUS", 4);
    msg.header.protocol_version = PROTOCOL_VERSION;
    // The length counts the message type field, which lives in our Header struct.
    msg.header.message_length = sizeof(T) - 16;
    msg.header.id = server_id;
    msg.header.message_type = type;
    msg.header.crc32 = 0;
    msg.header.crc32 = crc32(&msg, sizeof(T));
}

static PortShared port_shared(int pad, bool connected) {
    PortShared p = {};
    p.pad_id = static_cast<uint8_t>(pad);
    if (connected) {
        p.state = 2;
        p.model = 2;
        p.connection = 1;
        p.mac[5] = static_cast<uint8_t>(pad + 1);
        p.battery = 5;
    }
    return p;
}

DsuServer::~DsuServer() {
    stop();
}

bool DsuServer::start(uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error_ = "WSAStartup failed";
        return false;
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (socket_ == INVALID_SOCKET || bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = "Cannot listen on UDP port " + std::to_string(port) + " (another DSU server running?)";
        stop();
        return false;
    }

    // Wake up regularly so stop() does not hang in recvfrom.
    DWORD timeout_ms = 200;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    server_id_ = std::random_device{}();
    running_ = true;
    thread_ = std::thread(&DsuServer::receive_loop, this);
    return true;
}

void DsuServer::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        WSACleanup();
    }
}

void DsuServer::receive_loop() {
    uint8_t buffer[512];
    while (running_) {
        sockaddr_in from = {};
        int from_len = sizeof(from);
        const int size =
            recvfrom(socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (size > 0) {
            handle_packet(buffer, size, from);
        }
    }
}

void DsuServer::handle_packet(const uint8_t* data, int size, const sockaddr_in& from) {
    if (size < static_cast<int>(sizeof(Header)) || std::memcmp(data, "DSUC", 4) != 0) {
        return;
    }
    Header header;
    std::memcpy(&header, data, sizeof(header));
    const uint8_t* body = data + sizeof(Header);
    const int body_size = size - static_cast<int>(sizeof(Header));

    if (header.message_type == MSG_VERSION) {
        VersionResponse msg = {};
        msg.max_protocol_version = PROTOCOL_VERSION;
        finish(msg, MSG_VERSION, server_id_);
        send_to(&msg, sizeof(msg), from);
    } else if (header.message_type == MSG_PORT_INFO && body_size >= 4) {
        int32_t count;
        std::memcpy(&count, body, 4);
        count = std::clamp(count, 0, std::min(4, body_size - 4));
        for (int i = 0; i < count; ++i) {
            const int pad = body[4 + i];
            if (pad >= 4) {
                continue;
            }
            PortInfo msg = {};
            {
                std::lock_guard lock(mutex_);
                msg.port = port_shared(pad, frame_[pad].connected);
            }
            finish(msg, MSG_PORT_INFO, server_id_);
            send_to(&msg, sizeof(msg), from);
        }
    } else if (header.message_type == MSG_PAD_DATA && body_size >= 8) {
        const uint8_t flags = body[0];
        const uint8_t pad_id = body[1];
        const uint8_t* mac = body + 2;

        const auto until = std::chrono::steady_clock::now() + SUBSCRIPTION_TIMEOUT;
        std::lock_guard lock(mutex_);
        auto it = std::find_if(clients_.begin(), clients_.end(), [&](const Client& c) {
            return c.addr.sin_addr.s_addr == from.sin_addr.s_addr && c.addr.sin_port == from.sin_port;
        });
        if (it == clients_.end()) {
            clients_.push_back({from, {}});
            it = clients_.end() - 1;
        }
        for (int pad = 0; pad < 4; ++pad) {
            const bool by_id = (flags & 1) && pad_id == pad;
            const bool by_mac = (flags & 2) && std::memcmp(mac, port_shared(pad, true).mac, 6) == 0;
            if (flags == 0 || by_id || by_mac) {
                it->subscribed_until[pad] = until;
            }
        }
    }
}

void DsuServer::publish(const AdapterFrame& frame) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);
    frame_ = frame;

    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [&](const Client& c) {
                                      return std::all_of(std::begin(c.subscribed_until), std::end(c.subscribed_until), [&](auto t) {
                                          return t < now;
                                      });
                                  }),
                   clients_.end());

    for (int pad = 0; pad < 4; ++pad) {
        const GCPortState& gc = frame[pad];
        if (!gc.connected) {
            continue;
        }

        auto bit = [](uint8_t value, int n) {
            return (value >> n) & 1;
        };
        auto analog = [](int pressed) {
            return static_cast<uint8_t>(pressed ? 255 : 0);
        };
        const int a = bit(gc.buttons1, 0), b = bit(gc.buttons1, 1), x = bit(gc.buttons1, 2), y = bit(gc.buttons1, 3),
                  left = bit(gc.buttons1, 4), right = bit(gc.buttons1, 5), down = bit(gc.buttons1, 6), up = bit(gc.buttons1, 7);
        const int start = bit(gc.buttons2, 0), z = bit(gc.buttons2, 1), r = bit(gc.buttons2, 2), l = bit(gc.buttons2, 3);

        // Gamecube to DS4 layout: A=Cross B=Circle X=Square Y=Triangle Z=R1 Start=Options,
        // L/R analog = L2/R2, L/R digital click = L3/R3.
        PadData msg = {};
        msg.port = port_shared(pad, true);
        msg.active = 1;
        msg.packet_counter = packet_counter_[pad]++;
        msg.buttons1 = static_cast<uint8_t>(l << 1 | r << 2 | start << 3 | up << 4 | right << 5 | down << 6 | left << 7);
        msg.buttons2 = static_cast<uint8_t>(z << 3 | y << 4 | b << 5 | a << 6 | x << 7);
        msg.left_x = gc.stick[0];
        msg.left_y = gc.stick[1];
        msg.right_x = gc.cstick[0];
        msg.right_y = gc.cstick[1];
        msg.dpad_left = analog(left);
        msg.dpad_down = analog(down);
        msg.dpad_right = analog(right);
        msg.dpad_up = analog(up);
        msg.square = analog(x);
        msg.cross = analog(a);
        msg.circle = analog(b);
        msg.triangle = analog(y);
        msg.r1 = analog(z);
        msg.l2 = gc.trigger[0];
        msg.r2 = gc.trigger[1];
        finish(msg, MSG_PAD_DATA, server_id_);

        for (const Client& c : clients_) {
            if (c.subscribed_until[pad] >= now) {
                send_to(&msg, sizeof(msg), c.addr);
                last_pad_data_ = now.time_since_epoch().count();
            }
        }
    }
}

int DsuServer::client_count() {
    std::lock_guard lock(mutex_);
    return static_cast<int>(clients_.size());
}

bool DsuServer::is_transmitting() const {
    const auto last = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(last_pad_data_.load()));
    return last_pad_data_ != 0 && std::chrono::steady_clock::now() - last < std::chrono::milliseconds(500);
}

void DsuServer::send_to(const void* data, int size, const sockaddr_in& to) {
    sendto(socket_, static_cast<const char*>(data), size, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}
