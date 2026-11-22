#pragma once

namespace esa::audio {

// Whether this app may open an audio input. macOS treats every input as a microphone, a
// radio's USB codec included, and asks the operator once.
enum class InputAccess {
    Granted,       // or nothing to ask: another platform, or a run from outside the app bundle
    Undetermined,  // not asked yet
    Denied,        // refused, or restricted by policy
};

InputAccess inputAccess();
// Asks the operator, on macOS, if they have not been asked; the answer is read with
// inputAccess() afterwards. Returns at once.
void requestInputAccess();

} // namespace esa::audio
