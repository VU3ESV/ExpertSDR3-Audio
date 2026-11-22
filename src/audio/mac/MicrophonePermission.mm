#include "audio/MicrophonePermission.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

namespace esa::audio {

namespace {

// Only an app bundle carries the usage string macOS shows when it asks. Asking without one
// ends the process, so a binary run on its own leaves it to CoreAudio, which asks on behalf
// of the terminal that started it.
bool bundleCanAsk()
{
    return [[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSMicrophoneUsageDescription"] != nil;
}

} // namespace

InputAccess inputAccess()
{
    if (!bundleCanAsk()) return InputAccess::Granted;
    switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized:
        return InputAccess::Granted;
    case AVAuthorizationStatusNotDetermined:
        return InputAccess::Undetermined;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
        return InputAccess::Denied;
    }
    return InputAccess::Denied;
}

void requestInputAccess()
{
    if (!bundleCanAsk()) return;
    // The answer comes on a queue of macOS's choosing; it is only ever read back through
    // inputAccess(), so nothing here needs to reach Qt.
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
}

} // namespace esa::audio
