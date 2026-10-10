//gc_adapter.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Reads the official Nintendo Wii U / Switch Gamecube adapter (WUP-028) over libusb.
// Same protocol Dolphin uses in Source/Core/InputCommon/GCAdapter.cpp.

#pragma once
#include <array>
#include <cstdint>
#include <string>

struct libusb_context;
struct libusb_device_handle;

constexpr int PORT_COUNT = 4;  // controller ports on the adapter

struct GCPortState {
    bool connected = false;
    uint8_t buttons1 = 0;  // A B X Y Left Right Down Up (bit 0..7)
    uint8_t buttons2 = 0;  // Start Z R L (bit 0..3)
    uint8_t stick[2] = {128, 128};
    uint8_t cstick[2] = {128, 128};
    uint8_t trigger[2] = {0, 0};  // L, R
};

using AdapterFrame = std::array<GCPortState, PORT_COUNT>;

class GCAdapter {
public:
    enum class ReadResult { Ok, Timeout, Error };

    GCAdapter();
    ~GCAdapter();

    bool open();  // On failure, last_error() says why.
    void close();
    bool is_open() const {
        return handle_ != nullptr;
    }
    ReadResult read(AdapterFrame& frame);
    const std::string& last_error() const {
        return error_;
    }

private:
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    uint8_t endpoint_in_ = 0;
    uint8_t endpoint_out_ = 0;
    std::string error_;
};
