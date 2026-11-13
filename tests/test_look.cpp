// The approved look, held still. Each page is drawn as `--settings <copy of tests/look/<page>.ini>
// --page <page> --size WxH --screenshot` draws it (showForScreenshot, the code that option runs)
// and compared with the golden drawn in the same environment, tests/golden/<key>/<page>.png. A
// later AetherSDR sync, or a change of ours, that alters a page fails here until someone has
// looked at it and drawn the goldens again (docs/RELEASING.md, Look goldens).
//
//   ESA_LOOK_OUT=<folder>    write every render to <folder>/<key>/<page>.png, a set ready to
//                            commit, and a diff to <folder>/diff/<page>.png for each page that
//                            fails. Without it, only a failure writes those, to a new temporary
//                            folder it names.
//   ESA_UPDATE_GOLDENS=1     draw this environment's set again, into the source tree, comparing
//                            nothing: each page reports the file it wrote, and ctest lists the
//                            test as skipped.

#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "audio/AudioSink.h"
#include "gui/MainWindow.h"
#include "gui/Screenshot.h"
#include "gui/pages/OutputPage.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QLibraryInfo>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QVersionNumber>
#include <QtTest>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>

using namespace esa;

namespace {

// What the test returns when it compared nothing; CMakeLists.txt gives it to ctest as
// SKIP_RETURN_CODE. Clear of QtTest's own codes, which count failures.
constexpr int kNothingCompared = 77;

// A pixel matches when every channel is within this of the golden's, and a page passes only
// when every pixel matches. The environment that drew a set draws it again pixel for pixel, in
// a release or a debug build, so any pixel further out is a change someone should look at.
// Small ones are real: a new colour for the window's title text moves 0.3 % of the pixels, and
// a comma for each decimal point on AetherNR 48. The theme's surfaces are closer still: the
// ground, a raised card and a nested one differ by only 4 to 12 per channel, so a tolerance as
// wide as that would let a card lose its surface unseen. This one leaves room only for blending
// that rounds a step or two apart.
constexpr int kTolerance = 3;

// The environment that drew a set. The same window rasterises its text differently from one
// operating system release, processor or Qt minor version to the next, so each has its own
// goldens: "macos-15-arm64-qt6.10", "debian-12-x86_64-qt6.4", "windows-11-x86_64-qt6.10".
// A Mac's point releases share its major version's set.
QString goldenKey()
{
    const QString os = QSysInfo::productType();
    QString release = QSysInfo::productVersion();
    if (os == QLatin1String("macos") || os == QLatin1String("osx")) release = release.section(QLatin1Char('.'), 0, 0);
    const QVersionNumber qt = QLibraryInfo::version();
    QString key = QStringLiteral("%1-%2-%3-qt%4.%5")
                      .arg(os, release, QSysInfo::buildCpuArchitecture())
                      .arg(qt.majorVersion())
                      .arg(qt.minorVersion())
                      .toLower();
    // Only what a folder name can hold on every system.
    key.replace(QRegularExpression(QStringLiteral("[^a-z0-9.-]")), QStringLiteral("_"));
    return key;
}

// The largest difference in any one channel between two pixels, out of 255.
int channelDifference(QRgb a, QRgb b)
{
    return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
}

struct Difference {
    qint64 pixels{0};
    qint64 beyond{0};  // with a channel past the tolerance
    int worst{0};
    QPoint worstAt;

    bool passes() const { return beyond == 0; }
    // The share that match, rounded down, so a page with one pixel out never reads as 100.00 %.
    QString within() const
    {
        const qint64 hundredths = pixels > 0 ? (pixels - beyond) * 10000 / pixels : 0;
        return QStringLiteral("%1.%2 %").arg(hundredths / 100).arg(hundredths % 100, 2, 10, QLatin1Char('0'));
    }
};

// Two images of the same size, both Format_RGB32.
Difference measure(const QImage& golden, const QImage& render)
{
    Difference d;
    for (int y = 0; y < golden.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(golden.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(render.constScanLine(y));
        for (int x = 0; x < golden.width(); ++x) {
            const int diff = channelDifference(a[x], b[x]);
            ++d.pixels;
            if (diff > kTolerance) ++d.beyond;
            if (diff > d.worst) {
                d.worst = diff;
                d.worstAt = QPoint(x, y);
            }
        }
    }
    return d;
}

// The golden, the render and where they differ, side by side. In the third, a pixel past the
// tolerance is red, and the rest is the golden dimmed, to show where on the page it is.
QImage diffImage(const QImage& golden, const QImage& render)
{
    const int width = std::max(golden.width(), render.width());
    const int height = std::max(golden.height(), render.height());
    QImage out(3 * width, height, QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter painter(&out);
    painter.drawImage(0, 0, golden);
    painter.drawImage(width, 0, render);
    painter.end();
    if (golden.size() != render.size()) return out;
    for (int y = 0; y < golden.height(); ++y) {
        const auto* a = reinterpret_cast<const QRgb*>(golden.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(render.constScanLine(y));
        auto* map = reinterpret_cast<QRgb*>(out.scanLine(y)) + 2 * width;
        for (int x = 0; x < golden.width(); ++x) {
            map[x] = channelDifference(a[x], b[x]) > kTolerance
                         ? qRgb(255, 40, 40)
                         : qRgb(qRed(a[x]) / 4, qGreen(a[x]) / 4, qBlue(a[x]) / 4);
        }
    }
    return out;
}

} // namespace

class TestLook : public QObject {
    Q_OBJECT

public:
    bool comparedNothing() const { return m_compared == 0; }

private:
    QString m_key;
    QDir m_goldens;      // tests/golden/<key>
    QString m_out;       // ESA_LOOK_OUT, made absolute; empty when not set
    bool m_update{false};
    std::unique_ptr<QTemporaryDir> m_failures;  // where failures go without ESA_LOOK_OUT
    int m_compared{0};

    QString noSetMessage() const
    {
        return QStringLiteral(
                   "No look goldens for this environment, %1 (tests/golden/%1/), so nothing was compared. "
                   "To make them, run ESA_UPDATE_GOLDENS=1 ctest --test-dir build -R test_look -V, look at every "
                   "picture it writes and commit the folder; or commit the folder a CI run uploads as "
                   "look-<job>. See docs/RELEASING.md, Look goldens.")
            .arg(m_key);
    }

    // ESA_LOOK_OUT; without it, a new temporary folder, kept, made at the first failure.
    QString failureFolder()
    {
        if (!m_out.isEmpty()) return m_out;
        if (!m_failures) {
            m_failures = std::make_unique<QTemporaryDir>(QDir::temp().filePath(QStringLiteral("test_look-XXXXXX")));
            m_failures->setAutoRemove(false);
        }
        return m_failures->isValid() ? m_failures->path() : QString();
    }

    // Writes `image` to <folder>/<sub>/<page>.png and returns the path, or why it could not.
    static QString write(const QImage& image, const QString& folder, const QString& sub, const QString& page)
    {
        if (folder.isEmpty()) return QStringLiteral("(no folder to write to)");
        const QDir dir(QDir(folder).filePath(sub));
        if (!dir.mkpath(QStringLiteral("."))) return QStringLiteral("(could not make %1)").arg(dir.path());
        const QString path = dir.filePath(page + QStringLiteral(".png"));
        return image.save(path, "PNG") ? QDir::toNativeSeparators(path) : QStringLiteral("(could not write %1)").arg(path);
    }

    // The window as main() opens it for --settings <copy of the fixture>, and its picture as
    // --page, --size and --screenshot take it. Only what the engine plays into and the three
    // pins below differ from the app.
    void render(const QString& page, const QSize& size, QImage* shot)
    {
        // A copy, as the fixtures ask: the window saves its settings.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fixture = QStringLiteral(ESA_SOURCE_DIR "/tests/look/%1.ini").arg(page);
        QFile source(fixture);
        QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(fixture));
        const QString ini = dir.filePath(page + QStringLiteral(".ini"));
        QFile copy(ini);
        QVERIFY(copy.open(QIODevice::WriteOnly));
        QVERIFY(copy.write(source.readAll()) == source.size());
        copy.close();

        Settings settings(ini);
        // Never the Documents folder, which no picture shows.
        settings.setRecordingFolder(dir.filePath(QStringLiteral("Recordings")));
        const AppConfig config = settings.load();
        // The fixtures play into nothing. Even one that named a device would get no sound: the
        // engine below has no other output to give it.
        QCOMPARE(config.deviceKey, QString(kNullDeviceKey));
        ProfileLibrary profiles(ProfileLibrary::defaultPath(ini));
        Engine engine(ini, [](const AppConfig&, QString*) { return std::unique_ptr<AudioSink>(std::make_unique<NullSink>()); });
        // After the window, which may still post to it, and before it goes.
        const auto stop = qScopeGuard([&engine] { engine.shutdown(); });

        const std::unique_ptr<MainWindow> opened = openMainWindow(&engine, &settings, &profiles, config);
        MainWindow& window = *opened;
        window.setQuitHandler([] {});
        // This computer's audio devices are not part of the look, and Final Output says when
        // there are none, as on most CI runners. Every render gets the same one instead.
        auto* output = window.findChild<OutputPage*>();
        QVERIFY(output);
        QObject::disconnect(&engine, &Engine::devicesChanged, output, nullptr);
        AudioDeviceInfo speakers;
        speakers.key = QStringLiteral("Test|Speakers");
        speakers.name = QStringLiteral("Speakers");
        speakers.hostApi = QStringLiteral("Test");
        speakers.maxOutputChannels = 2;
        speakers.defaultSampleRate = 48000.0;
        speakers.isDefault = true;
        output->showDevices({speakers});
        // AetherNR greys the tab of a method this build lacks, and DFNR's on a virtual Mac, such
        // as a CI runner. Every render shows them available, as a release on a real computer.
        for (const char* id : {"nr4", "dfnr", "nnr"}) {
            auto* tab = window.findChild<QPushButton*>(QStringLiteral("nrTab_") + QLatin1String(id));
            QVERIFY2(tab, id);
            tab->setEnabled(true);
        }

        std::optional<QImage> taken;
        showForScreenshot(window, page, size, [&taken](const QImage& image) { taken = image; });
        QTRY_VERIFY_WITH_TIMEOUT(taken.has_value(), kScreenshotDelayMs + 10000);
        *shot = taken->convertToFormat(QImage::Format_RGB32);
    }

private slots:
    void initTestCase()
    {
        m_key = goldenKey();
        qInfo("Look goldens for this environment: %s", qPrintable(m_key));
        m_goldens = QDir(QStringLiteral(ESA_SOURCE_DIR "/tests/golden/") + m_key);
        m_update = qEnvironmentVariable("ESA_UPDATE_GOLDENS") == QLatin1String("1");
        if (!qEnvironmentVariableIsEmpty("ESA_LOOK_OUT")) m_out = QDir(qEnvironmentVariable("ESA_LOOK_OUT")).absolutePath();
        // Goldens are approved by a person who has looked at them, never drawn by a build.
        if (m_update && !qEnvironmentVariableIsEmpty("CI")) QFAIL("ESA_UPDATE_GOLDENS is never set in CI");
        // Every set is drawn offscreen, at one pixel per point; a real screen draws otherwise.
        if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
            QSKIP("The goldens are drawn offscreen: run with QT_QPA_PLATFORM=offscreen, as ctest does.");
        }

        // Nothing to compare with, and no renders asked for: nothing to draw.
        if (!m_goldens.exists() && !m_update && m_out.isEmpty()) QSKIP(qPrintable(noSetMessage()));

        // As every window test: the pages' painted check boxes are cached in the test location.
        QStandardPaths::setTestModeEnabled(true);
        // The check boxes, radio buttons and arrows are painted once into that cache and kept
        // until their colours change, so an old drawing would outlive a new one and pass here.
        // Every run paints its own. The folder is named for this test, never one the app or
        // AetherSDR reads.
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const QString name = QCoreApplication::applicationName();
        QVERIFY2(!name.isEmpty() && cache.contains(name),
                 qPrintable(QStringLiteral("Not this test's own cache, so not cleared: \"%1\"").arg(cache)));
        QVERIFY2(QDir(cache).removeRecursively(), qPrintable(QStringLiteral("Could not clear %1").arg(cache)));
        // The locale writes AetherNR's readings, "1.00" or "1,00": one for every render.
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    }

    // The rule itself: any number of pixels just inside the tolerance pass, and one pixel just
    // past it fails the page.
    void tolerance()
    {
        QImage golden(40, 30, QImage::Format_RGB32);
        golden.fill(qRgb(30, 40, 50));
        QImage render(golden.size(), QImage::Format_RGB32);
        render.fill(qRgb(30 + kTolerance, 40, 50 - kTolerance));
        QVERIFY(measure(golden, render).passes());

        render.setPixel(7, 9, qRgb(30, 40 + kTolerance + 1, 50));
        const Difference d = measure(golden, render);
        QVERIFY(!d.passes());
        QCOMPARE(d.beyond, qint64(1));
        QCOMPARE(d.worst, kTolerance + 1);
        QCOMPARE(d.worstAt, QPoint(7, 9));
        // 1199 of 1200, rounded down rather than up to 100.00 %.
        QCOMPARE(d.within(), QStringLiteral("99.91 %"));
    }

    void pages_data()
    {
        QTest::addColumn<QSize>("size");
        // The windows they were matched against: AetherRX's six screenshots hold a 720x750
        // window, and AetherNR, which has none, is the spike's 720x480 render of upstream's page.
        for (const char* page : {"gate", "eq", "comp", "tube", "exciter", "output"}) {
            QTest::newRow(page) << QSize(720, 750);
        }
        QTest::newRow("nr") << QSize(720, 480);
    }

    void pages()
    {
        QFETCH(QSize, size);
        const QString page = QString::fromLatin1(QTest::currentDataTag());
        // Without a set, drawn only for ESA_LOOK_OUT or ESA_UPDATE_GOLDENS (initTestCase).
        const bool haveSet = m_goldens.exists();

        QImage shot;
        render(page, size, &shot);
        if (QTest::currentTestFailed()) return;
        if (!m_out.isEmpty()) qInfo("%s: rendered to %s", qPrintable(page), qPrintable(write(shot, m_out, m_key, page)));

        if (m_update) {
            QVERIFY2(m_goldens.mkpath(QStringLiteral(".")), qPrintable(m_goldens.path()));
            const QString golden = m_goldens.filePath(page + QStringLiteral(".png"));
            QVERIFY2(shot.save(golden, "PNG"), qPrintable(golden));
            QSKIP(qPrintable(QStringLiteral("Wrote %1; nothing compared. Look at it before committing it.")
                                 .arg(QDir::toNativeSeparators(golden))));
        }
        if (!haveSet) QSKIP(qPrintable(noSetMessage()));

        const QString goldenPath = m_goldens.filePath(page + QStringLiteral(".png"));
        QVERIFY2(QFileInfo::exists(goldenPath),
                 qPrintable(QStringLiteral("The goldens for %1 have no %2.png. Draw the set again with "
                                           "ESA_UPDATE_GOLDENS=1 (docs/RELEASING.md).")
                                .arg(m_key, page)));
        const QImage golden = QImage(goldenPath).convertToFormat(QImage::Format_RGB32);
        QVERIFY2(!golden.isNull(), qPrintable(goldenPath));
        ++m_compared;

        QString problem;
        if (golden.size() != shot.size()) {
            problem = QStringLiteral("%1 is drawn %2x%3, its golden is %4x%5.")
                          .arg(page)
                          .arg(shot.width())
                          .arg(shot.height())
                          .arg(golden.width())
                          .arg(golden.height());
        } else {
            const Difference d = measure(golden, shot);
            qInfo("%s: %s of pixels within %d/255 of the golden", qPrintable(page), qPrintable(d.within()), kTolerance);
            if (!d.passes()) {
                problem = QStringLiteral("%1 differs from its golden: %2 of pixels within %3/255 on every channel, "
                                         "100 % needed. %4 of %5 pixels differ by more; the most, %6/255, at (%7, %8).")
                              .arg(page, d.within())
                              .arg(kTolerance)
                              .arg(d.beyond)
                              .arg(d.pixels)
                              .arg(d.worst)
                              .arg(d.worstAt.x())
                              .arg(d.worstAt.y());
            }
        }
        if (problem.isEmpty()) return;

        const QString folder = failureFolder();
        const QString rendered = m_out.isEmpty() ? write(shot, folder, m_key, page)
                                                 : QDir::toNativeSeparators(QDir(m_out).filePath(m_key + QLatin1Char('/') + page + QStringLiteral(".png")));
        const QString diff = write(diffImage(golden, shot), folder, QStringLiteral("diff"), page);
        QFAIL(qPrintable(problem
                         + QStringLiteral(" In %1: the render, %2, and the golden, the render and the pixels "
                                          "past the tolerance in red, side by side, %3. If the change is meant, "
                                          "draw the goldens again with ESA_UPDATE_GOLDENS=1 (docs/RELEASING.md).")
                               .arg(QDir::toNativeSeparators(folder), rendered, diff)));
    }
};

// QTEST_MAIN without the 96 dpi it sets for widget tests: the window is drawn as the
// application draws it.
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    TestLook test;
    QTEST_SET_MAIN_SOURCE_PATH
    const int failures = QTest::qExec(&test, argc, argv);
    // A run that compared nothing has not passed.
    return failures == 0 && test.comparedNothing() ? kNothingCompared : failures;
}

#include "test_look.moc"
