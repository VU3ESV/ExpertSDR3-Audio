#pragma once

// SpectralNR.cpp is the only vendored file that includes LogManager.h, and it
// uses one logging category from it. This stands in for upstream's header, which
// drags in the rest of AetherSDR's logging subsystem. See AetherShim.cpp.
#include <QLoggingCategory>

namespace AetherSDR {
Q_DECLARE_LOGGING_CATEGORY(lcDsp)
}
