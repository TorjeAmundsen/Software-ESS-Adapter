//port.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// What the bridge loop does for each port before process_stick: centering the controller on its
// origin, and notch calibration one button press at a time.

#pragma once
#include <cstdint>
#include <string>

#include "gamepads.hpp"
#include "input_source.hpp"
#include "notch.hpp"

// Centers a port's controller the way the game would against its origin, since Dolphin's emulated
// pad always reports a centered one.
class PortCentering {
public:
    // Like Dolphin and the console, the first report after a controller connects is its origin.
    // With calibrated notches, the main stick is centered on the saved neutral instead, and
    // anywhere the stick was seen resting during calibration reads exactly neutral. `origin` gets
    // the origin used, for checking calibration presses.
    GCPortState center(const GCPortState& raw, const NotchCalibration& notches, GCPortState* origin = nullptr);
    // Takes the next report as the origin, for when another controller takes over the port without
    // it ever reading as unplugged.
    void reset() {
        has_origin_ = false;
    }

private:
    GCPortState origin_;
    bool has_origin_ = false;
};

// Notch calibration in progress on a port: the step it's on and the raw stick recorded per step.
struct CalibrationProgress {
    int step = 0;  // 0 to CALIBRATION_STEPS - 1, see notch.hpp
    uint8_t samples[CALIBRATION_STEPS][2] = {};
};

enum class CalibrationEvent {
    NoPress,     // no new press
    Cancelled,   // B
    WrongNotch,  // A outside the notch the step asks for: `notch` says which, the step stays
    NextStep,
    Misplaced,   // the finished calibration has `notch` out of place: the step goes back to it
    Finished,    // `calibration` is the new, valid calibration
};

struct CalibrationResult {
    CalibrationEvent event = CalibrationEvent::NoPress;
    int notch = -1;
    NotchCalibration calibration;
};

// One frame of calibrating: A records the current step at the raw stick, B cancels. Pass whether
// A and B were just pressed, and the origin the port is centered on. A notch is checked against
// that origin, since neutral isn't known yet; the finished calibration is checked again against
// the real neutral.
CalibrationResult calibration_press(CalibrationProgress& progress, const uint8_t raw_stick[2], const uint8_t origin_stick[2],
                                    bool a_pressed, bool b_pressed);

// The settings file section a controller's notch calibration and offsets live in, through other
// controllers: one per model and serial number, so identical controllers without a serial share
// one. Adapter calibrations live in the port sections instead, so the two never share a section.
std::string calibration_section(const PortController& controller);
