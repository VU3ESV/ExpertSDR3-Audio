#include "core/LogManager.h"

namespace AetherSDR {
// Warnings and up by default, as upstream: ThemeManager's debug lines are for its own
// developers. QT_LOGGING_RULES="esa.aethergui.debug=true" turns them on.
Q_LOGGING_CATEGORY(lcGui, "esa.aethergui", QtWarningMsg)
}
