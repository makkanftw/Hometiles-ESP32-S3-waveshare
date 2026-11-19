#pragma once

#include "src/devices/hardware_io_profile.h"

namespace DeviceWaveshareS3TouchLCD7 {

// The board's RS485, CAN and UART headers are not part of the HomeTiles I/O
// contract, and every GPIO is used by the display, touch or SD interface.
// Expose no configurable onboard outputs.
inline constexpr Device::HardwareIoProfile kHardwareIoProfile{};

}  // namespace DeviceWaveshareS3TouchLCD7
