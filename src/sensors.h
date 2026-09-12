// Accelerometer in s3e units (milli-g, landscape game orientation).
#pragma once

#include <cstdint>

namespace sensors {
bool available();
bool start();
void stop();
void poll();
int32_t x();
int32_t y();
int32_t z();
}  // namespace sensors
