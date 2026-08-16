// Linked into every registered test target (see the TMP/TEMP block in
// CMakeLists.txt) so QStandardPaths test mode is on before main() runs.
//
// QTest names the QCoreApplication after the test executable (test_editor,
// test_controller, ...), so any QStandardPaths::writableLocation() call in
// app code — ThemeManager::userDir() uses AppDataLocation — would create
// %APPDATA%\<testname> directories on Windows for every test run.
//
// NOTE: Qt test mode does NOT redirect AppDataLocation under the temp dir
// on Windows — qstandardpaths_win.cpp appends "/qttest" to the *same*
// %APPDATA% path. The flag is still set here because (a)
// ThemeManager::userDir() branches on QStandardPaths::isTestModeEnabled()
// to route user themes to the build tree, and (b) it keeps QSettings and
// other Qt mechanisms from touching real user data during tests.
//
// A TU is used instead of a forced-include (/FI) header because ninja does
// NOT recompile sources when compile flags change — an existing build tree
// would silently keep stale test binaries without the flag. Adding a source
// file, by contrast, changes the link inputs and forces every test
// executable to relink, so a plain incremental `cmake --build` applies the
// change to any tree.
#include <QStandardPaths>

namespace {
struct RcxQTestTestMode {
    RcxQTestTestMode() { QStandardPaths::setTestModeEnabled(true); }
} rcxQTestTestMode;
}
