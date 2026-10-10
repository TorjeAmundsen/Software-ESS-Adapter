//gc_adapter.cpp

//The Following code is released under GNU GENERAL PUBLIC LICENSE Version 3.

#include "gc_adapter.hpp"

#include <libusb.h>

static constexpr uint16_t ADAPTER_VID = 0x057e;
static constexpr uint16_t ADAPTER_PID = 0x0337;
static constexpr int PAYLOAD_SIZE = 37;  // 1 header byte + 4 ports * 9 bytes
static constexpr unsigned READ_TIMEOUT_MS = 100;

GCAdapter::GCAdapter() {
    if (libusb_init(&ctx_) != LIBUSB_SUCCESS) {
        ctx_ = nullptr;
    }
}

GCAdapter::~GCAdapter() {
    close();
    if (ctx_) {
        libusb_exit(ctx_);
    }
}

bool GCAdapter::open() {
    if (!ctx_) {
        error_ = "libusb failed to initialize";
        return false;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx_, &list);
    libusb_device* device = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == LIBUSB_SUCCESS && desc.idVendor == ADAPTER_VID &&
            desc.idProduct == ADAPTER_PID) {
            device = list[i];
            break;
        }
    }

    if (!device) {
        libusb_free_device_list(list, 1);
        error_ = "Adapter not found";
        return false;
    }

    int ret = libusb_open(device, &handle_);
    if (ret == LIBUSB_SUCCESS) {
        // Same endpoint discovery as Dolphin: last IN and last OUT endpoint win.
        libusb_config_descriptor* config = nullptr;
        if (libusb_get_config_descriptor(device, 0, &config) == LIBUSB_SUCCESS) {
            for (uint8_t ic = 0; ic < config->bNumInterfaces; ++ic) {
                const libusb_interface& iface = config->interface[ic];
                for (int a = 0; a < iface.num_altsetting; ++a) {
                    const libusb_interface_descriptor& alt = iface.altsetting[a];
                    for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                        const uint8_t address = alt.endpoint[e].bEndpointAddress;
                        if (address & LIBUSB_ENDPOINT_IN) {
                            endpoint_in_ = address;
                        } else {
                            endpoint_out_ = address;
                        }
                    }
                }
            }
            libusb_free_config_descriptor(config);
        }
    }
    libusb_free_device_list(list, 1);

    if (ret == LIBUSB_ERROR_NOT_SUPPORTED) {
        error_ = "Adapter has no WinUSB driver (install it with Zadig, same as for Dolphin)";
        handle_ = nullptr;
        return false;
    }
    if (ret != LIBUSB_SUCCESS) {
        error_ = std::string("Cannot open adapter: ") + libusb_error_name(ret);
        handle_ = nullptr;
        return false;
    }

    ret = libusb_claim_interface(handle_, 0);
    if (ret != LIBUSB_SUCCESS) {
        error_ = std::string("Cannot claim adapter (is Dolphin using it?): ") + libusb_error_name(ret);
        libusb_close(handle_);
        handle_ = nullptr;
        return false;
    }

    // Tell the adapter to start sending input reports.
    uint8_t init = 0x13;
    int transferred = 0;
    libusb_interrupt_transfer(handle_, endpoint_out_, &init, 1, &transferred, READ_TIMEOUT_MS);

    error_.clear();
    return true;
}

void GCAdapter::close() {
    if (!handle_) {
        return;
    }
    libusb_release_interface(handle_, 0);
    libusb_close(handle_);
    handle_ = nullptr;
}

GCAdapter::ReadResult GCAdapter::read(AdapterFrame& frame) {
    uint8_t payload[PAYLOAD_SIZE];
    int size = 0;
    const int ret = libusb_interrupt_transfer(handle_, endpoint_in_, payload, PAYLOAD_SIZE, &size, READ_TIMEOUT_MS);
    if (ret == LIBUSB_ERROR_TIMEOUT) {
        return ReadResult::Timeout;
    }
    if (ret != LIBUSB_SUCCESS) {
        error_ = std::string("Adapter read failed: ") + libusb_error_name(ret);
        return ReadResult::Error;
    }
    // Short or non-HID reports happen for a few frames after init. Skip them.
    if (size != PAYLOAD_SIZE || payload[0] != LIBUSB_DT_HID) {
        return ReadResult::Timeout;
    }

    for (int port = 0; port < 4; ++port) {
        const uint8_t* d = &payload[1 + 9 * port];
        GCPortState& s = frame[port];
        // Bit 4 = wired controller, bit 5 = wavebird.
        s.connected = (d[0] & 0x30) != 0;
        s.buttons1 = d[1];
        s.buttons2 = d[2];
        s.stick[0] = d[3];
        s.stick[1] = d[4];
        s.cstick[0] = d[5];
        s.cstick[1] = d[6];
        s.trigger[0] = d[7];
        s.trigger[1] = d[8];
    }
    return ReadResult::Ok;
}
