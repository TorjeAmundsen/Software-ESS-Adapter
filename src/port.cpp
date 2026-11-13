//port.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "port.hpp"

#include <algorithm>

#include "ess.hpp"

GCPortState PortCentering::center(const GCPortState& raw, const NotchCalibration& notches, GCPortState* origin) {
    if (raw.connected && !has_origin_) {
        origin_ = raw;
    }
    has_origin_ = raw.connected;
    GCPortState used = origin_;
    if (notches.valid) {
        used.stick[0] = notches.neutral[0];
        used.stick[1] = notches.neutral[1];
    }
    if (origin) {
        *origin = used;
    }

    GCPortState out = raw;
    uint8_t stick_origin[2] = {used.stick[0], used.stick[1]};
    normalize_origin(out.stick, stick_origin);
    uint8_t cstick_origin[2] = {used.cstick[0], used.cstick[1]};
    normalize_origin(out.cstick, cstick_origin);
    for (int i = 0; i < 2; ++i) {
        out.trigger[i] = raw.trigger[i] > used.trigger[i] ? raw.trigger[i] - used.trigger[i] : 0;
    }
    if (in_neutral_range(notches, raw.stick)) {
        out.stick[0] = 128;
        out.stick[1] = 128;
    }
    return out;
}

CalibrationResult calibration_press(CalibrationProgress& progress, const uint8_t raw_stick[2], const uint8_t origin_stick[2],
                                    bool a_pressed, bool b_pressed) {
    CalibrationResult result;
    if (b_pressed) {
        result.event = CalibrationEvent::Cancelled;
        return result;
    }
    if (!a_pressed) {
        return result;
    }
    const int step = progress.step;
    progress.samples[step][0] = raw_stick[0];
    progress.samples[step][1] = raw_stick[1];
    const int x = raw_stick[0] - origin_stick[0];
    const int y = raw_stick[1] - origin_stick[1];
    if (!is_neutral_step(step) && !notch_in_section(notch_of_step(step), x, y)) {
        result.event = CalibrationEvent::WrongNotch;
        result.notch = notch_of_step(step);
    } else if (step + 1 < CALIBRATION_STEPS) {
        progress.step = step + 1;
        result.event = CalibrationEvent::NextStep;
    } else if (const NotchCalibration calibration = make_calibration(progress.samples); misplaced_notch(calibration) >= 0) {
        result.event = CalibrationEvent::Misplaced;
        result.notch = misplaced_notch(calibration);
        progress.step = 2 * result.notch;
    } else {
        result.event = CalibrationEvent::Finished;
        result.calibration = calibration;
    }
    return result;
}

std::string calibration_section(const PortController& controller) {
    std::string section = "Controller " + controller.model;
    if (!controller.serial.empty()) {
        section += " " + controller.serial;
    }
    // ] would end the section name early.
    section.erase(std::remove_if(section.begin(), section.end(),
                                 [](char c) {
                                     return c == ']' || static_cast<unsigned char>(c) < ' ';
                                 }),
                  section.end());
    return section;
}
