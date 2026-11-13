#pragma once

// Stands in for AetherSDR's core/LogManager.h, which brings its whole logging system.
// The vendored pages and ThemeManager log through one category from it. The DSP has a
// separate shim of the same name (src/dsp/shim/), private to aether_dsp.
#include <QLoggingCategory>

namespace AetherSDR {
Q_DECLARE_LOGGING_CATEGORY(lcGui)
}
