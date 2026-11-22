#include "audio/MicrophonePermission.h"

#include <QtGlobal>

// macOS has its own (mac/MicrophonePermission.mm). Elsewhere an input opens or fails by itself.
#ifndef Q_OS_MACOS
namespace esa::audio {

InputAccess inputAccess()
{
    return InputAccess::Granted;
}

void requestInputAccess() {}

} // namespace esa::audio
#endif
