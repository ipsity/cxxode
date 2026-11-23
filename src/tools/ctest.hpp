#pragma once

#include "core/process.hpp"

namespace lay {

// Observations are limited to the streams captured from this completed CTest process.
void observe_ctest(Verification& verification);

} // namespace lay
