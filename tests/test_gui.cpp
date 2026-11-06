#include "app/Engine.h"
#include "app/Settings.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"
#include "gui/MainWindow.h"
#include "gui/ParamWidgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtTest>

using namespace esa;

// Builds the real window against a real, unconnected engine, so it never reaches a
// radio. Set ESA_GUI_SHOT_DIR to also save a PNG of each tab, for eyeballing layout.
class TestGui : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    std::unique_ptr<Settings> m_settings;
    std::unique_ptr<Engine> m_engine;
    std::unique_ptr<MainWindow> m_window;

    void make(const AppConfig& config)
    {
        const QString path = m_dir.filePath(QStringLiteral("gui.ini"));
        m_window.reset();
        m_engine = std::make_unique<Engine>(path);
        m_settings = std::make_unique<Settings>(path);
        m_window = std::make_unique<MainWindow>(m_engine.get(), m_settings.get(), config);
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
    void cleanup()
    {
        m_window.reset();
        if (m_engine) m_engine->shutdown();
        m_engine.reset();
        m_settings.reset();
    }

    void everyTabRendersAndCanBeCaptured()
    {
        make(AppConfig{});
        auto* tabs = m_window->findChild<QTabWidget*>();
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 7);
        const QString shots = qEnvironmentVariable("ESA_GUI_SHOT_DIR");
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QCoreApplication::processEvents();
            const QPixmap shot = m_window->grab();
            QVERIFY(!shot.isNull());
            if (!shots.isEmpty()) {
                shot.save(QDir(shots).filePath(QStringLiteral("tab-%1.png").arg(tabs->tabText(i))));
            }
        }
    }

    // A parameter in the table with no way to reach it from the window is a bug.
    void everyParameterHasAControl()
    {
        make(AppConfig{});
        for (const ParamSpec& spec : RxChain::stageParams()) {
            if (spec.key.startsWith(QLatin1String("eq.b"))) continue;  // the EQ's own table
            QVERIFY2(control(spec.key), qPrintable(spec.key));
        }
        for (const NrMethod& method : nrMethods()) {
            for (const ParamSpec& spec : method.params) QVERIFY2(control(spec.key), qPrintable(spec.key));
        }
    }

    void theEqTableHasARowForEveryBand()
    {
        make(AppConfig{});
        auto* tabs = m_window->findChild<QTabWidget*>();
        QWidget* eq = tabs->widget(2);
        const auto spins = eq->findChildren<QDoubleSpinBox*>();
        // Frequency, gain and Q per band, plus the output level's own spin box.
        QCOMPARE(int(spins.size()), AetherSDR::ClientEq::kDefaultBandCount * 3 + 1);
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
