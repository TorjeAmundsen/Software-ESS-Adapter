//gamepads.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Reads any controller SDL knows (Xbox, PlayStation, Switch Pro, and most others) as a Gamecube
// controller. Buttons go by position: the bottom face button is A and the right one B, in the
// order OoT shows them and the Gamecube and N64 controllers have them. Left is X, top Y. The right
// shoulder is Z, and the triggers are L and R.
//
// Each port either keeps a chosen controller (PortController) or takes any controller without a
// port of its own, in the order they connect. A port whose chosen controller is unplugged waits
// for it.

#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "input_source.hpp"

struct SDL_Gamepad;

// An SDL stick axis (-32768 to 32767) as a Gamecube stick byte, with full tilt at
// GATE_STRAIGHT from center. SDL's y grows downward, so pass y negated.
uint8_t gamepad_stick_byte(int axis);
// An SDL trigger (0 to 32767) as a Gamecube trigger byte (0 to 255).
uint8_t gamepad_trigger_byte(int trigger);
// How far a trigger goes before it also counts as the Gamecube's digital L or R, which clicks at
// the bottom of the press.
constexpr int TRIGGER_CLICK = 32767 * 9 / 10;

// A connected controller, as the windows list it.
struct GamepadInfo {
    uint32_t id;  // SDL's id for it, while it stays connected
    std::string name;
    int port;  // -1 when it has none: every port is taken or waiting for its own controller
    // What the controller calls its bottom and right face buttons (A and B here), such as Cross
    // and Circle, or B and A on Nintendo's layout, for prompts.
    std::string a_label, b_label;
};

// A controller as the settings file remembers it: the one a port keeps (no model means any
// controller), or whose notch calibration a section holds.
struct PortController {
    std::string model;   // SDL's GUID for the model
    std::string serial;  // empty if the controller reports none: then any of the model will do
    std::string name;    // to show while it's unplugged
};

class Gamepads : public InputSource {
public:
    ~Gamepads() override;

    bool open() override;
    void close() override;
    bool is_open() const override {
        return open_;
    }
    ReadResult read(AdapterFrame& frame) override;
    // SDL's id for the controller behind each port in the last frame read, 0 for none, so the
    // caller can tell when another controller takes over a port. Bridge thread only.
    const std::array<uint32_t, PORT_COUNT>& frame_ids() const {
        return frame_ids_;
    }
    std::string status() const override;
    const std::string& last_error() const override {
        return error_;
    }

    // These are safe to call from any thread.
    std::vector<GamepadInfo> connected() const;
    PortController port_controller(int port) const;
    void set_port_controller(int port, const PortController& controller);  // for loading settings
    // Which connected controller `id` is, or nothing once it's unplugged.
    std::optional<PortController> controller(uint32_t id) const;
    // Makes the connected controller `id` the port's own. A controller picked from another port
    // swaps ports with this port's controller, and the two ports swap what they keep. False if
    // it's no longer connected, and then nothing changes.
    bool pick(int port, uint32_t id);
    // Lets the port take any controller.
    void pick_any(int port);

private:
    struct Pad {
        SDL_Gamepad* handle;
        uint32_t id;
        std::string name, model, serial;
        int port;
        std::string a_label, b_label;
    };
    Pad* in_port(int port);
    void place(Pad& pad);
    void fill_free_ports();
    void add(uint32_t id);
    void remove(uint32_t id);

    mutable std::mutex mutex_;  // guards the three below
    std::vector<Pad> pads_;
    PortController chosen_[PORT_COUNT];
    bool ports_changed_ = false;  // tells read() to update the controllers' player lights
    std::array<uint32_t, PORT_COUNT> frame_ids_{};
    bool open_ = false;
    std::string error_;
};
