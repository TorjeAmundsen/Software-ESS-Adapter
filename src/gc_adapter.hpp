//gc_adapter.hpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

// Reads the official Nintendo Wii U / Switch Gamecube adapter (WUP-028) over libusb.
// Same protocol Dolphin uses in Source/Core/InputCommon/GCAdapter.cpp.

#pragma once
#include <cstdint>
#include <string>

#include "input_source.hpp"

struct libusb_context;
struct libusb_device_handle;

// An adapter input report: a header byte (0x21, HID), then 9 bytes per port: status (bit 4 a wired
// controller, bit 5 a WaveBird), buttons A B X Y Left Right Down Up, buttons Start Z R L, main
// stick x and y, C-stick x and y, triggers L and R.
constexpr int ADAPTER_REPORT_SIZE = 37;
// Fills the frame from a report. False, leaving the frame alone, for the short or non-HID reports
// the adapter sends for a few frames after it starts.
bool parse_adapter_report(const uint8_t* report, int size, AdapterFrame& frame);

class GCAdapter : public InputSource {
public:
    GCAdapter();
    ~GCAdapter() override;

    bool open() override;
    void close() override;
    bool is_open() const override {
        return handle_ != nullptr;
    }
    ReadResult read(AdapterFrame& frame) override;
    std::string status() const override {
        return "Adapter connected";
    }
    const std::string& last_error() const override {
        return error_;
    }

private:
    libusb_context* ctx_ = nullptr;
    libusb_device_handle* handle_ = nullptr;
    uint8_t endpoint_in_ = 0;
    uint8_t endpoint_out_ = 0;
    std::string error_;
};
