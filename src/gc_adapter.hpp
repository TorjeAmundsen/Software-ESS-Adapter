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
