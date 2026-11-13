// Built into every test program. Turns on the test sandbox (src/app/TestSandbox.h) before
// main(), and the offscreen platform unless another is asked for, so a test run straight from
// a build folder is as safe as one under ctest for the speakers, the radio and the screen, and
// so are the programs a test starts, which inherit both. The home folder is ctest's to set
// (HOME and CFFIXED_USER_HOME, for the window's tests): run directly, a test uses the real one.
#include <QByteArray>
#include <QtGlobal>

namespace {
[[maybe_unused]] const bool sandboxOn = [] {
    qputenv("ESA_TEST_SANDBOX", QByteArray("1"));  // QByteArray: the one overload Qt 6.4 also has
    // Before any QGuiApplication exists, which is when the platform is chosen.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
    return true;
}();
} // namespace
