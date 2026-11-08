#pragma once

#include <QtGlobal>

namespace esa {

// Set by the test programs only (tests/TestSandbox.cpp before main(), and ctest), never by a
// user. Under it nothing can play through this computer's speakers or reach a real ExpertSDR3:
// a real output, a connection or a discovery probe to one of ExpertSDR3's ports is refused,
// with a warning that starts "Test sandbox:" and, for the output, the status's error. A test
// that forgets its fake output or its mock radio then gets no sound and no radio. It is not
// made to fail by that alone; the warning is what shows it. The programs a test starts
// inherit the sandbox.
inline bool testSandbox()
{
    return !qEnvironmentVariableIsEmpty("ESA_TEST_SANDBOX");
}

// ExpertSDR3's own TCI ports. A real radio may be listening on any of them, on any host.
inline bool isExpertSdrPort(int port)
{
    return port == 40001 || port == 50001 || port == 60001;
}

} // namespace esa
