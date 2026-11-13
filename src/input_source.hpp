//input_source.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Where controller input comes from: the Gamecube adapter (gc_adapter.hpp) or any other
// controller through SDL (gamepads.hpp). Both report Gamecube controllers, port by port.

#pragma once
#include <array>
#include <cstdint>
#include <string>

constexpr int PORT_COUNT = 4;  // controller ports, on the adapter or for other controllers

struct GCPortState {
    bool connected = false;
    uint8_t buttons1 = 0;  // A B X Y Left Right Down Up (bit 0..7)
    uint8_t buttons2 = 0;  // Start Z R L (bit 0..3)
    uint8_t stick[2] = {128, 128};
    uint8_t cstick[2] = {128, 128};
    uint8_t trigger[2] = {0, 0};  // L, R
};

using AdapterFrame = std::array<GCPortState, PORT_COUNT>;

class InputSource {
public:
    enum class ReadResult { Ok, Timeout, Error };

    virtual ~InputSource() = default;
    virtual bool open() = 0;  // On failure, last_error() says why.
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    // Waits for the next frame. On Error, last_error() says why.
    virtual ReadResult read(AdapterFrame& frame) = 0;
    // The main window's status line while reading works.
    virtual std::string status() const = 0;
    virtual const std::string& last_error() const = 0;
};
