//gamepads.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "gamepads.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "notch.hpp"

// The longest a read waits for a controller to change before sending its state again, so the
// game keeps getting frames at least as often as from a stock adapter (125 Hz).
static constexpr int FRAME_MS = 8;

uint8_t gamepad_stick_byte(int axis) {
    const long value = std::lround(axis * static_cast<double>(GATE_STRAIGHT) / SDL_JOYSTICK_AXIS_MAX);
    return static_cast<uint8_t>(128 + std::clamp<long>(value, -GATE_STRAIGHT, GATE_STRAIGHT));
}

uint8_t gamepad_trigger_byte(int trigger) {
    return static_cast<uint8_t>(std::clamp<long>(std::lround(trigger * 255.0 / SDL_JOYSTICK_AXIS_MAX), 0, 255));
}

static GCPortState state_of(SDL_Gamepad* pad) {
    auto button = [pad](SDL_GamepadButton b, int bit) {
        return SDL_GetGamepadButton(pad, b) ? 1 << bit : 0;
    };
    auto axis = [pad](SDL_GamepadAxis a) {
        return static_cast<int>(SDL_GetGamepadAxis(pad, a));
    };
    const int l = axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER), r = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    GCPortState s;
    s.connected = true;
    s.buttons1 = static_cast<uint8_t>(button(SDL_GAMEPAD_BUTTON_SOUTH, 0) | button(SDL_GAMEPAD_BUTTON_EAST, 1) |
                                      button(SDL_GAMEPAD_BUTTON_WEST, 2) | button(SDL_GAMEPAD_BUTTON_NORTH, 3) |
                                      button(SDL_GAMEPAD_BUTTON_DPAD_LEFT, 4) | button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 5) |
                                      button(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 6) | button(SDL_GAMEPAD_BUTTON_DPAD_UP, 7));
    s.buttons2 = static_cast<uint8_t>(button(SDL_GAMEPAD_BUTTON_START, 0) | button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 1) |
                                      (r >= TRIGGER_CLICK) << 2 | (l >= TRIGGER_CLICK) << 3);
    s.stick[0] = gamepad_stick_byte(axis(SDL_GAMEPAD_AXIS_LEFTX));
    s.stick[1] = gamepad_stick_byte(-axis(SDL_GAMEPAD_AXIS_LEFTY));
    s.cstick[0] = gamepad_stick_byte(axis(SDL_GAMEPAD_AXIS_RIGHTX));
    s.cstick[1] = gamepad_stick_byte(-axis(SDL_GAMEPAD_AXIS_RIGHTY));
    s.trigger[0] = gamepad_trigger_byte(l);
    s.trigger[1] = gamepad_trigger_byte(r);
    return s;
}

Gamepads::~Gamepads() {
    close();
}

bool Gamepads::open() {
    // The game's window has focus, not ours, so read controllers in the background too.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // There is no SDL window to receive device and raw input messages on Windows, so let SDL
    // handle them on its own thread.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    // The adapter is the other input source, read with libusb. Keep SDL away from it.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "0");
    // SDL would turn SIGINT and SIGTERM into quit events, which nothing here reads, so on Linux
    // Ctrl+C, kill and logging out would no longer end the program.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        error_ = std::string("Controller support failed to start: ") + SDL_GetError();
        return false;
    }
    // Controllers already plugged in arrive as SDL_EVENT_GAMEPAD_ADDED on the first read.
    open_ = true;
    error_.clear();
    return true;
}

void Gamepads::close() {
    if (!open_) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        for (const Pad& pad : pads_) {
            SDL_CloseGamepad(pad.handle);
        }
        pads_.clear();
    }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    open_ = false;
}

Gamepads::Pad* Gamepads::in_port(int port) {
    for (Pad& pad : pads_) {
        if (pad.port == port) {
            return &pad;
        }
    }
    return nullptr;
}

// A free port for the pad: one that keeps it, or else the first that takes any controller.
void Gamepads::place(Pad& pad) {
    auto keeps = [&](const PortController& chosen) {
        return !chosen.model.empty() && chosen.model == pad.model && (chosen.serial.empty() || chosen.serial == pad.serial);
    };
    pad.port = -1;
    for (const bool any : {false, true}) {
        for (int port = 0; port < PORT_COUNT; ++port) {
            if (!in_port(port) && (any ? chosen_[port].model.empty() : keeps(chosen_[port]))) {
                pad.port = port;
                ports_changed_ = true;
                return;
            }
        }
    }
}

void Gamepads::fill_free_ports() {
    for (Pad& pad : pads_) {
        if (pad.port < 0) {
            place(pad);
        }
    }
}

void Gamepads::add(uint32_t id) {
    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (!handle) {
        return;
    }
    const char* name_text = SDL_GetGamepadName(handle);
    const char* serial = SDL_GetGamepadSerial(handle);
    const std::string name = name_text ? name_text : "Controller";
    // SDL's GUID names the model. Should a device have none (all zeros), use its name instead, so
    // it doesn't match every other such device.
    const SDL_GUID guid = SDL_GetJoystickGUID(SDL_GetGamepadJoystick(handle));
    const bool no_guid = std::all_of(std::begin(guid.data), std::end(guid.data), [](Uint8 byte) {
        return byte == 0;
    });
    char model[33];
    SDL_GUIDToString(guid, model, sizeof(model));
    std::lock_guard lock(mutex_);
    pads_.push_back({handle, id, name, no_guid ? "name:" + name : std::string(model), serial ? serial : "", -1});
    place(pads_.back());
}

void Gamepads::remove(uint32_t id) {
    std::lock_guard lock(mutex_);
    const auto pad = std::find_if(pads_.begin(), pads_.end(), [id](const Pad& p) {
        return p.id == id;
    });
    if (pad != pads_.end()) {
        SDL_CloseGamepad(pad->handle);
        pads_.erase(pad);
        fill_free_ports();
    }
}

Gamepads::ReadResult Gamepads::read(AdapterFrame& frame) {
    SDL_WaitEventTimeout(nullptr, FRAME_MS);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
            add(event.gdevice.which);
        } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
            remove(event.gdevice.which);
        }
    }
    std::lock_guard lock(mutex_);
    if (std::exchange(ports_changed_, false)) {
        for (const Pad& pad : pads_) {
            SDL_SetGamepadPlayerIndex(pad.handle, pad.port);  // lights up the port, where the controller has lights
        }
    }
    for (int port = 0; port < PORT_COUNT; ++port) {
        const Pad* pad = in_port(port);
        frame[port] = pad ? state_of(pad->handle) : GCPortState{};
        frame_ids_[port] = pad ? pad->id : 0;
    }
    return ReadResult::Ok;
}

std::string Gamepads::status() const {
    std::lock_guard lock(mutex_);
    return pads_.empty() ? "No controllers found. Plug one in." : "Controllers connected";
}

std::vector<GamepadInfo> Gamepads::connected() const {
    std::lock_guard lock(mutex_);
    std::vector<GamepadInfo> list;
    for (const Pad& pad : pads_) {
        list.push_back({pad.id, pad.name, pad.port});
    }
    return list;
}

PortController Gamepads::port_controller(int port) const {
    std::lock_guard lock(mutex_);
    return chosen_[port];
}

void Gamepads::set_port_controller(int port, const PortController& controller) {
    std::lock_guard lock(mutex_);
    chosen_[port] = controller;
}

std::optional<PortController> Gamepads::controller(uint32_t id) const {
    std::lock_guard lock(mutex_);
    for (const Pad& pad : pads_) {
        if (pad.id == id) {
            return PortController{pad.model, pad.serial, pad.name};
        }
    }
    return std::nullopt;
}

bool Gamepads::pick(int port, uint32_t id) {
    std::lock_guard lock(mutex_);
    const auto pad = std::find_if(pads_.begin(), pads_.end(), [id](const Pad& p) {
        return p.id == id;
    });
    if (pad == pads_.end()) {
        return false;  // unplugged while the menu was open: keep what the port had
    }
    const int from = pad->port;
    if (from != port) {
        if (from >= 0) {
            chosen_[from] = chosen_[port];
        }
        if (Pad* occupant = in_port(port)) {
            occupant->port = from;  // to the picked controller's old port, or none
        }
        pad->port = port;
        ports_changed_ = true;
    }
    chosen_[port] = {pad->model, pad->serial, pad->name};
    fill_free_ports();
    return true;
}

void Gamepads::pick_any(int port) {
    std::lock_guard lock(mutex_);
    chosen_[port] = {};
    fill_free_ports();
}
