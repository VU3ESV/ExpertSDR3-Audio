#include "MockExpert.h"

#include "app/Engine.h"
#include "app/Profiles.h"
#include "app/Settings.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/AboutDialog.h"
#include "gui/MainWindow.h"
#include "gui/ParamWidgets.h"
#include "gui/RadioDiscoveryDialog.h"
#include "gui/aether/Appearance.h"

#include "ClientCompKnob.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QSpinBox>
#include <QSlider>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

using namespace esa;

// Builds the real window against a real, unconnected engine, so it never reaches a
// radio. Set ESA_GUI_SHOT_DIR to also save a PNG of each tab, for eyeballing layout.
class TestGui : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<ProfileLibrary> m_profiles;
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<MainWindow> m_window;

    void make(const AppConfig& config)
    {
        const QString path = m_dir.filePath(QStringLiteral("gui.ini"));
        m_window.reset();
        m_engine = std::make_unique<Engine>(path);
        m_settings = std::make_unique<Settings>(path);
        // Whatever a test presses, REC never reaches the real Documents folder.
        m_settings->setRecordingFolder(m_dir.filePath(QStringLiteral("Recordings")));
        m_profiles = std::make_unique<ProfileLibrary>(ProfileLibrary::defaultPath(path));
        m_window = std::make_unique<MainWindow>(m_engine.get(), m_engine->taps(), m_settings.get(), m_profiles.get(),
                                                config);
        // Nor does anything open a file manager or a folder dialog.
        m_window->setRevealHandler([](const QString&) {});
        m_window->setFolderChooser([](const QString&) { return QString(); });
        m_window->show();
        QCoreApplication::processEvents();
    }

    ParamControl* control(const QString& key)
    {
        const auto controls = m_window->findChildren<ParamControl*>();
        for (ParamControl* c : controls) {
            if (c->spec().key == key) return c;
        }
        return nullptr;
    }

private slots:
    void initTestCase()
    {
        // Caches the pages write (AetherSDR's painted check boxes) go to the test location.
        QStandardPaths::setTestModeEnabled(true);
        Appearance::install();  // the look main() sets up, so tests see it too
    }

    void cleanup()
    {
        m_window.reset();
        if (m_engine) m_engine->shutdown();
        m_engine.reset();
        m_settings.reset();
        m_profiles.reset();
        QDir dir(m_dir.path());
        const QStringList files = dir.entryList(QDir::Files);
        for (const QString& file : files) dir.remove(file);
    }

    void everyTabRendersAndCanBeCaptured()
    {
        make(AppConfig{});
        const QStringList pages = m_window->pageIds();
        // Radio, AetherNR, Gate, EQ, Compressor, Tube, Exciter and Final Output.
        QCOMPARE(pages.size(), 8);
        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        for (const QString& page : pages) {
            m_window->showPage(page);
            QCoreApplication::processEvents();
            QCOMPARE(m_window->currentPage(), page);
            const QPixmap shot = m_window->grab();
            QVERIFY(!shot.isNull());
            if (!shots.isEmpty()) shot.save(QDir(shots).filePath(QStringLiteral("page-%1.png").arg(page)));
        }
    }

    // A parameter in the table with no way to reach it from the window is a bug.
    void everyParameterHasAControl()
    {
        make(AppConfig{});
        for (const ParamSpec& spec : RxChain::stageParams()) {
            QVERIFY2(control(spec.key), qPrintable(spec.key));
        }
        for (const NrMethod& method : nrMethods()) {
            for (const ParamSpec& spec : method.params) QVERIFY2(control(spec.key), qPrintable(spec.key));
        }
    }

    void theEqHasControlsForEveryBand()
    {
        make(AppConfig{});
        for (int band = 0; band < AetherSDR::ClientEq::kDefaultBandCount; ++band) {
            for (const char* leaf : {"on", "freq", "gain", "q", "type", "slope"}) {
                const QString key = QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf));
                QVERIFY2(control(key), qPrintable(key));
            }
        }
    }

    void aControlShowsTheStoredValue()
    {
        AppConfig config;
        config.params.insert(QStringLiteral("comp.threshold"), -33.5);
        make(config);
        QCOMPARE(control(QStringLiteral("comp.threshold"))->value(), -33.5);
        QCOMPARE(control(QStringLiteral("comp.ratio"))->value(), 3.0);  // untouched: the default
    }

    // Upstream's mode is a preset that overwrites ratio and floor, so the sliders must
    // follow it or they would display values the audio is not using.
    void gateModeUpdatesTheSlidersItPresets()
    {
        make(AppConfig{});
        control(QStringLiteral("gate.mode"))->setValue(1);
        QCOMPARE(control(QStringLiteral("gate.ratio"))->value(), 10.0);
        QCOMPARE(control(QStringLiteral("gate.floor"))->value(), -40.0);
        control(QStringLiteral("gate.mode"))->setValue(0);
        QCOMPARE(control(QStringLiteral("gate.ratio"))->value(), 2.0);
        QCOMPARE(control(QStringLiteral("gate.floor"))->value(), -15.0);
    }

    void aLogarithmicSliderCoversItsWholeRange()
    {
        make(AppConfig{});
        ParamControl* freq = nullptr;
        // A wide range that uses a slider rather than the EQ table's spin box.
        for (ParamControl* c : m_window->findChildren<ParamControl*>()) {
            if (c->spec().min > 0 && c->spec().max / c->spec().min >= 100.0 && c->spec().choices.isEmpty()) {
                freq = c;
                break;
            }
        }
        if (!freq) QSKIP("no logarithmic control in this build");
        for (double v : {freq->spec().min, freq->spec().max}) {
            freq->setValue(v);
            QCOMPARE(freq->value(), v);
        }
    }

    void valuesAreClampedToTheirRange()
    {
        make(AppConfig{});
        ParamControl* c = control(QStringLiteral("comp.threshold"));
        c->setValue(999.0);
        QCOMPARE(c->value(), c->spec().max);
        c->setValue(-999.0);
        QCOMPARE(c->value(), c->spec().min);
    }

    QComboBox* profileList()
    {
        // The only combo box whose first entry is the factory settings.
        const auto combos = m_window->findChildren<QComboBox*>();
        for (QComboBox* combo : combos) {
            if (combo->count() > 0 && combo->itemText(0) == QLatin1String("Factory settings")) return combo;
        }
        return nullptr;
    }

    // The chain stages as the rail shows them, top to bottom.
    QStringList railOrder()
    {
        QCoreApplication::processEvents();  // the rail re-lays its rows out on the next pass
        QList<QPair<int, QString>> rows;
        for (const QString& stage : RxChain::stageIds()) {
            const QString name = QStringLiteral("aetherRxTab") + RxChain::stageLabel(stage).remove(QLatin1Char(' '));
            auto* tab = m_window->findChild<QPushButton*>(name);
            if (!tab) return {};
            rows.append({tab->mapTo(m_window.get(), QPoint()).y(), stage});
        }
        std::sort(rows.begin(), rows.end());
        QStringList order;
        for (const auto& row : rows) order << row.second;
        return order;
    }

    void savingAProfileListsItAndClearsModified()
    {
        make(AppConfig{});
        QVERIFY(!m_window->chainModified());
        control(QStringLiteral("comp.threshold"))->setValue(-30);
        QVERIFY(m_window->chainModified());

        QVERIFY(m_window->saveProfileAs(QStringLiteral("Weak SSB")));
        QVERIFY(!m_window->chainModified());
        QCOMPARE(m_window->currentProfile(), QStringLiteral("Weak SSB"));
        QCOMPARE(profileList()->currentText(), QStringLiteral("Weak SSB"));
    }

    // Everything the operator can see has to follow, or the screen would lie about the sound.
    void loadingAProfileUpdatesEveryControl()
    {
        make(AppConfig{});
        control(QStringLiteral("comp.threshold"))->setValue(-30);
        control(QStringLiteral("gate.mode"))->setValue(1);
        control(QStringLiteral("gate.ratio"))->setValue(3.5);  // not the preset the mode implies
        // RN2, chosen on the AetherNR page as the operator would. Once that page has its method
        // tabs, this presses nrTab_rn2 instead of setting the interim combo.
        m_window->showPage(QStringLiteral("nr"));
        auto* method = m_window->findChild<QComboBox*>(QStringLiteral("nrMethod"));
        QVERIFY(method);
        QVERIFY(method->findData(QStringLiteral("rn2")) >= 0);
        method->setCurrentIndex(method->findData(QStringLiteral("rn2")));
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Mine")));

        m_window->loadProfile(QString());
        QCOMPARE(control(QStringLiteral("comp.threshold"))->value(), -18.0);
        QCOMPARE(control(QStringLiteral("gate.mode"))->value(), 0.0);
        QCOMPARE(method->currentData().toString(), QString());
        QCOMPARE(m_window->currentProfile(), QString());
        QVERIFY(!m_window->chainModified());

        m_window->loadProfile(QStringLiteral("Mine"));
        QCOMPARE(control(QStringLiteral("comp.threshold"))->value(), -30.0);
        QCOMPARE(control(QStringLiteral("gate.mode"))->value(), 1.0);
        QCOMPARE(method->currentData().toString(), QStringLiteral("rn2"));
        // Upstream's mode is a preset overwriting ratio; the saved value has to win over it.
        QCOMPARE(control(QStringLiteral("gate.ratio"))->value(), 3.5);
        QVERIFY(!m_window->chainModified());
        QCOMPARE(profileList()->currentText(), QStringLiteral("Mine"));
    }

    void loadingAProfileUpdatesTheEqTableAndStageOrder()
    {
        make(AppConfig{});
        ChainSettings chain;
        chain.stageOrder = {QStringLiteral("pudu"), QStringLiteral("tube"), QStringLiteral("comp"), QStringLiteral("eq"), QStringLiteral("gate")};
        chain.params.insert(QStringLiteral("eq.b4.freq"), 850.0);
        chain.params.insert(QStringLiteral("eq.b4.on"), 1.0);
        QVERIFY(m_profiles->save(QStringLiteral("EQ"), chain));

        m_window->loadProfile(QStringLiteral("EQ"));
        QCOMPARE(m_window->stageOrder(), chain.stageOrder);  // what the rail's host answers
        QCOMPARE(railOrder(), chain.stageOrder);             // and what the rail shows
        QCOMPARE(control(QStringLiteral("eq.b4.freq"))->value(), 850.0);
        QCOMPARE(control(QStringLiteral("eq.b4.on"))->value(), 1.0);
        // And what the AetherRX pages draw from.
        QCOMPARE(m_engine->taps()->view.eq.band(4).freqHz, 850.0f);
        QVERIFY(m_engine->taps()->view.eq.band(4).enabled);
    }

    void aProfileLeavesTheStationSettingsAlone()
    {
        AppConfig config;
        config.outputGainDb = -12;
        config.host = QStringLiteral("10.0.0.9");
        make(config);
        ChainSettings chain;
        chain.nrMethod = QStringLiteral("rn2");
        QVERIFY(m_profiles->save(QStringLiteral("X"), chain));
        m_window->loadProfile(QStringLiteral("X"));
        auto* host = m_window->findChild<QLineEdit*>(QStringLiteral("host"));
        QVERIFY(host);
        QCOMPARE(host->text(), QStringLiteral("10.0.0.9"));
        // Once Final Output is AetherRX's own, this reads its Volume knob's value() instead.
        auto* volume = m_window->findChild<QSlider*>(QStringLiteral("volume"));
        QVERIFY(volume);
        QCOMPARE(volume->minimum(), -60);
        QCOMPARE(volume->value(), -12);
    }

    // An AetherRX knob, found as the operator finds it: by what it is and what it says.
    AetherSDR::ClientCompKnob* knob(const QString& label)
    {
        const auto knobs = m_window->findChildren<AetherSDR::ClientCompKnob*>();
        for (AetherSDR::ClientCompKnob* k : knobs) {
            if (k->accessibleName() == label) return k;
        }
        return nullptr;
    }

    // Typed into the knob's value field once it has the keyboard, then Return.
    static void turn(AetherSDR::ClientCompKnob* k, double value)
    {
        auto* field = k->findChild<QLineEdit*>();
        QVERIFY(field);
        k->window()->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(k->window()));
        field->setFocus(Qt::MouseFocusReason);
        QTRY_VERIFY(field->hasFocus());
        field->selectAll();
        QTest::keyClicks(field, QString::number(value));
        QTest::keyClick(field, Qt::Key_Return);
    }

    void changingAnythingShowsAsModified()
    {
        make(AppConfig{});
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Base")));

        m_window->showPage(QStringLiteral("gate"));
        AetherSDR::ClientCompKnob* thresh = knob(QStringLiteral("Thresh"));
        QVERIFY(thresh);
        turn(thresh, -30);
        QCOMPARE(control(QStringLiteral("gate.threshold"))->value(), -30.0);
        QVERIFY(m_window->chainModified());
        turn(thresh, -40);
        QVERIFY2(!m_window->chainModified(), "back to the saved value is not a change");

        ParamControl* q = control(QStringLiteral("eq.b9.q"));
        QVERIFY(q);
        q->setValue(q->value() + 1);
        QVERIFY2(m_window->chainModified(), "the EQ table counts too");
    }

    void aProfileFromTheLastSessionIsStillCurrent()
    {
        AppConfig config;
        make(config);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Kept")));
        config.profileName = QStringLiteral("Kept");
        make(config);
        QCOMPARE(m_window->currentProfile(), QStringLiteral("Kept"));
        QCOMPARE(profileList()->currentText(), QStringLiteral("Kept"));
        QVERIFY(!m_window->chainModified());
    }

    void aProfileThatNoLongerExistsIsForgotten()
    {
        AppConfig config;
        config.profileName = QStringLiteral("Deleted elsewhere");
        make(config);
        QCOMPARE(m_window->currentProfile(), QString());
        QCOMPARE(profileList()->currentIndex(), 0);
    }

    void deletingAProfileDoesNotChangeTheSound()
    {
        make(AppConfig{});
        control(QStringLiteral("comp.threshold"))->setValue(-30);
        QVERIFY(m_window->saveProfileAs(QStringLiteral("Doomed")));
        QVERIFY(m_window->deleteProfile(QStringLiteral("Doomed")));
        QCOMPARE(m_window->currentProfile(), QString());
        QCOMPARE(control(QStringLiteral("comp.threshold"))->value(), -30.0);
        QVERIFY2(m_window->chainModified(), "it now differs from the factory settings");
        QCOMPARE(profileList()->count(), 1);
    }

    void importedProfilesAppearButAreNotAppliedUnasked()
    {
        make(AppConfig{});
        ChainSettings chain;
        chain.nrMethod = QStringLiteral("rn2");
        ProfileLibrary elsewhere(m_dir.filePath(QStringLiteral("elsewhere.json")));
        QVERIFY(elsewhere.save(QStringLiteral("Shared"), chain));
        const QString file = m_dir.filePath(QStringLiteral("shared.json"));
        QVERIFY(elsewhere.exportFile(file, {QStringLiteral("Shared")}));

        const ImportResult result = m_window->importProfiles(file);
        QVERIFY(result.error.isEmpty());
        QVERIFY(profileList()->findText(QStringLiteral("Shared")) >= 0);
        QCOMPARE(m_window->currentProfile(), QString());  // the sound must not change by surprise
        QVERIFY(!m_window->chainModified());
    }

    void aProfileWithAMissingMethodPlaysWithNrOff()
    {
        make(AppConfig{});
        const QString file = m_dir.filePath(QStringLiteral("foreign.json"));
        QFile out(file);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(R"({"format":"ExpertSDR3Audio.profiles","version":1,"profiles":[{"name":"Foreign","nr":{"method":"nope"}}]})");
        out.close();
        QVERIFY(m_window->importProfiles(file).error.isEmpty());
        m_window->loadProfile(QStringLiteral("Foreign"));
        QCOMPARE(m_window->currentProfile(), QStringLiteral("Foreign"));
        QVERIFY(!m_window->chainModified());
    }

    void aboutBoxNamesTheDeveloper()
    {
        QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7"));
        AboutDialog about;
        const auto* heading = about.findChild<QLabel*>(QStringLiteral("aboutHeading"));
        QVERIFY(heading);
        QVERIFY(heading->text().contains(QStringLiteral("VU3ESV/LB9KJ")));
        QVERIFY(heading->text().contains(QStringLiteral("9.8.7")));
        const auto* icon = about.findChild<QLabel*>(QStringLiteral("aboutIcon"));
        QVERIFY(icon);
        QVERIFY2(!icon->pixmap().isNull(), "the icon resource did not load");
        about.show();
        QCoreApplication::processEvents();
        QVERIFY(!about.grab().isNull());
        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        if (!shots.isEmpty()) about.grab().save(QDir(shots).filePath(QStringLiteral("about.png")));
    }

    void theMenusOfferEverything()
    {
        make(AppConfig{});
        QStringList titles;
        const auto menus = m_window->menuBar()->actions();
        for (QAction* menu : menus) titles << menu->text();
        QCOMPARE(titles, (QStringList{QStringLiteral("Radio"), QStringLiteral("Profiles"), QStringLiteral("Help")}));
    }

    void theDiscoveryDialogFindsARadioAndHandsItBack()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        TciDiscovery::Options options;
        options.hosts = {QStringLiteral("127.0.0.1")};
        options.ports = {radio.port()};

        RadioDiscoveryDialog dialog(options);
        dialog.show();
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QListWidget*>()->count() == 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.isScanning(), 6000);
        const auto selected = dialog.selectedRadio();
        QVERIFY(selected.has_value());
        QCOMPARE(selected->port, radio.port());
        QCOMPARE(selected->device, QStringLiteral("ColibriDDC"));
        QVERIFY(dialog.findChild<QListWidget*>()->item(0)->text().contains(QStringLiteral("ColibriDDC")));
        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        if (!shots.isEmpty()) dialog.grab().save(QDir(shots).filePath(QStringLiteral("find-radios.png")));
        QVERIFY(radio.commands.isEmpty());
    }

    // Refused at once on a Mac or Linux, but only after retries on Windows, and an address that
    // never answers is slower still. TEST-NET is reserved: nothing is there on any real network.
    void theDiscoveryDialogExplainsWhenThereIsNothing_data()
    {
        QTest::addColumn<QString>("host");
        QTest::newRow("connection refused") << QStringLiteral("127.0.0.1");
        QTest::newRow("no answer") << QStringLiteral("192.0.2.1");
    }
    void theDiscoveryDialogExplainsWhenThereIsNothing()
    {
        QFETCH(QString, host);
        const int closedPort = closedLocalPort();  // nothing listens there, and no radio could
        QVERIFY(closedPort > 0);
        TciDiscovery::Options options;
        options.hosts = {host};
        options.ports = {closedPort};
        options.connectTimeoutMs = 400;

        RadioDiscoveryDialog dialog(options);
        dialog.show();
        // Waits on what the operator reads, because isScanning() is also false before a scan begins.
        const auto explained = [&dialog] {
            const auto labels = dialog.findChildren<QLabel*>();
            return std::any_of(labels.begin(), labels.end(), [](const QLabel* label) {
                return label->text().contains(QStringLiteral("TCI enabled"));
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(explained(), 6000);
        QVERIFY(!dialog.isScanning());
        QCOMPARE(dialog.findChild<QListWidget*>()->count(), 0);
        QVERIFY(!dialog.selectedRadio().has_value());
    }

    void pickingARadioConnectsToIt()
    {
        MockExpert radio;
        QVERIFY(radio.listen());
        AppConfig config;
        config.deviceKey = QStringLiteral("null");  // never the real speakers
        config.host = QStringLiteral("192.0.2.1");
        make(config);

        DiscoveredRadio picked;
        picked.host = QStringLiteral("127.0.0.1");
        picked.port = radio.port();
        m_window->connectToRadio(picked);
        QTRY_VERIFY_WITH_TIMEOUT(radio.audioStartCount() == 1, 6000);
        auto* host = m_window->findChild<QLineEdit*>(QStringLiteral("host"));
        QVERIFY(host);
        QCOMPARE(host->text(), QStringLiteral("127.0.0.1"));
        const auto spins = m_window->findChildren<QSpinBox*>();
        bool sawPort = false;
        for (QSpinBox* spin : spins) sawPort = sawPort || spin->value() == radio.port();
        QVERIFY(sawPort);
    }

    void pickingAnotherRadioLeavesTheFirstOneProperly()
    {
        MockExpert first;
        MockExpert second;
        QVERIFY(first.listen());
        QVERIFY(second.listen());
        AppConfig config;
        config.deviceKey = QStringLiteral("null");
        make(config);

        DiscoveredRadio a;
        a.host = QStringLiteral("127.0.0.1");
        a.port = first.port();
        m_window->connectToRadio(a);
        QTRY_VERIFY_WITH_TIMEOUT(first.muted, 6000);  // playing, so ExpertSDR3 went quiet

        DiscoveredRadio b;
        b.host = QStringLiteral("127.0.0.1");
        b.port = second.port();
        m_window->connectToRadio(b);
        QTRY_VERIFY_WITH_TIMEOUT(second.audioStartCount() == 1, 6000);
        QTRY_VERIFY_WITH_TIMEOUT(!first.muted, 6000);  // given back before moving on
        QTRY_VERIFY_WITH_TIMEOUT(second.muted, 6000);
    }

    void theWindowCanShrinkForASmallScreen()
    {
        make(AppConfig{});
        const QSize minimum = m_window->minimumSizeHint();
        qInfo() << "minimum window size" << minimum;
        // A 7-inch Raspberry Pi display is 800x480, so this needs to fit one, near enough
        // to be usable with the panel hidden.
        QVERIFY2(minimum.height() <= 600, qPrintable(QString::number(minimum.height())));
#ifndef Q_OS_WIN
        // Only where a small screen is plausible: font metrics make Windows report 952 here.
        QVERIFY2(minimum.width() <= 800, qPrintable(QString::number(minimum.width())));
#endif
    }
};

QTEST_MAIN(TestGui)
#include "test_gui.moc"
