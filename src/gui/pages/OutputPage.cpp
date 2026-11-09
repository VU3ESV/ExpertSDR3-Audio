#include "gui/pages/OutputPage.h"

#include "gui/aether/Guards.h"
#include "gui/binding/OutputFeed.h"
#include "gui/pages/StationBand.h"

#include "ClientCompKnob.h"
#include "StagePage.h"
#include "StripRxOutputPanel.h"
#include "StripWaveformPanel.h"
#include "WaveformWidget.h"
#include "core/AudioEngine.h"
#include "core/ThemeManager.h"

#include <QEvent>
#include <QLabel>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QtGlobal>

#include <algorithm>

namespace esa {

namespace {

using AetherSDR::ClientCompKnob;

constexpr float kMinVolumeDb = float(kMinOutputGainDb);
constexpr float kMaxVolumeDb = float(kMaxOutputGainDb);

} // namespace

float OutputPage::volumeFromNorm(float norm)
{
    norm = std::clamp(norm, 0.0f, 1.0f);
    // Each half from its own end, so the middle is exactly 0 dB, never -0.
    if (norm <= 0.5f) return kMinVolumeDb + 2.0f * norm * -kMinVolumeDb;
    return 2.0f * (norm - 0.5f) * kMaxVolumeDb;
}

float OutputPage::normFromVolume(float db)
{
    db = std::clamp(db, kMinVolumeDb, kMaxVolumeDb);
    if (db <= 0.0f) return 0.5f * (db - kMinVolumeDb) / -kMinVolumeDb;
    return 0.5f + 0.5f * db / kMaxVolumeDb;
}

OutputPage::OutputPage(PageContext& ctx)
    : Page(ctx)
{
    // As AetherRxDialog builds its last tab: the output panel at the top, the waveform below
    // taking what room is left (its own top stretch keeps the scope at the bottom), and
    // between them, where AetherRX leaves room, this computer's output.
    m_output = new AetherSDR::StripRxOutputPanel(ctx.aether);
    m_waveform = new AetherSDR::StripWaveformPanel(ctx.aether);
    m_scope = m_waveform->findChild<AetherSDR::WaveformWidget*>();
    makeVolume();
    m_band = new StationBand(ctx);

    auto* content = new QWidget;
    auto* column = new QVBoxLayout(content);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(8);
    column->addWidget(m_output, 0, Qt::AlignTop);
    column->addWidget(m_band);
    column->addWidget(m_waveform, 1);
    // Exactly the page wherever it fits. On a screen too short for it (a Raspberry Pi's
    // 480 lines) it scrolls, rather than cutting the scope off at the bottom.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    QWidget* scroller = scrollable(content);
    // A slim canon bar, as the other pages scroll with; only when it scrolls at all. Across
    // too: a window at its narrowest cuts MUTE and BOOST off at the right, and the bar is the
    // way to them, so it needs a handle to see and take.
    AetherSDR::ThemeManager::instance().applyStyleSheet(scroller, QStringLiteral(R"(
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical {
    background: {{color.canon.lineHi}};
    border-radius: 3px;
    min-height: 30px;
}
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal {
    background: {{color.canon.lineHi}};
    border-radius: 3px;
    min-width: 30px;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
    border: none;
    background: none;
}
QScrollBar::add-page, QScrollBar::sub-page { background: none; }
)"));
    layout->addWidget(scroller);

    // What goes to the speakers, shaped by the window's own Volume and BOOST, read before MUTE.
    m_feed = new OutputFeed(
        ctx.taps, ctx.aether,
        [this] {
            OutputFeed::Shape shape;
            shape.volumeDb = config().outputGainDb;
            shape.boost = config().boost;
            return shape;
        },
        this);
    connect(m_feed, &OutputFeed::silenceChanged, this, &OutputPage::showScopeNote);

    if (m_scope) {
        m_scopeNote = new QLabel(m_scope);
        m_scopeNote->setObjectName(QStringLiteral("outputScopeNote"));
        m_scopeNote->setAlignment(Qt::AlignCenter);
        // A click still reaches the scope beneath, which pauses it.
        m_scopeNote->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_scopeNote->hide();
        m_scope->installEventFilter(this);
    }
}

void OutputPage::makeVolume()
{
    // Upstream's Trim, the panel's only knob, becomes the one gain control there is.
    const auto knobs = m_output->findChildren<ClientCompKnob*>();
    Q_ASSERT(knobs.size() == 1);
    if (knobs.size() != 1) {
        // test_aether_gui fails on this too: the panel changed in a sync.
        qWarning("The Final Output panel has %d knobs where one was expected.", int(knobs.size()));
        if (knobs.isEmpty()) return;
    }
    m_volume = knobs.constFirst();
    m_volume->setLabel(QStringLiteral("Volume"));
    m_volume->setAccessibleName(QStringLiteral("Volume"));
    m_volume->setToolTip(QStringLiteral("How loud this app plays, from -60 to +30 dB; 0 dB at the top. "
                                        "Double-click for 0 dB. Kept with this computer's settings, not in profiles."));
    m_volume->setRange(kMinVolumeDb, kMaxVolumeDb);
    m_volume->setDefault(0.0f);
    m_volume->setValueFromNorm(&OutputPage::volumeFromNorm);
    m_volume->setNormFromValue(&OutputPage::normFromVolume);
    // Upstream's "+0.8 dB" reading stays.
    const QSignalBlocker block(m_volume);
    m_volume->setValue(float(config().outputGainDb));
}

void OutputPage::attach()
{
    // As AetherRxDialog, once every page is in the window: bound to RX, then the floating
    // editors' chrome taken off. Both title plates stay, because on this page they are what
    // tell the two panels apart.
    m_output->showForRx();
    m_waveform->showForRx();
    AetherSDR::tidyEmbeddedPanel(m_output, /*keepTitle=*/true);
    AetherSDR::tidyEmbeddedPanel(m_waveform, /*keepTitle=*/true);
    WheelAccumulator::install(m_output);
    ValueFieldGuard::install(m_output);

    if (!m_scope) return;
    // A click pauses the scope. A double-click would open upstream's WAVE settings, which
    // this app does not have.
    m_scope->setToolTip(QStringLiteral("Click to pause or resume the waveform."));
#if defined(Q_PROCESSOR_ARM) && defined(Q_OS_LINUX)
    // Upstream redraws it at 62 Hz, too much for a Raspberry Pi beside everything else. Only
    // Linux on ARM: an Apple Silicon Mac keeps upstream's rate.
    m_scope->setRefreshRateHz(30);
#endif
    // In the colour and size upstream writes its own message over the scope in.
    m_scope->ensurePolished();
    const double points = std::max(8.0, m_scope->font().pointSizeF() - 1.0);
    AetherSDR::ThemeManager::instance().applyStyleSheet(
        m_scopeNote, QStringLiteral("QLabel { background: transparent; color: {{color.text.secondary}};"
                                    " font-size: %1pt; padding: 0; }")
                         .arg(points));
    showScopeNote();
}

void OutputPage::showDevices(const QList<AudioDeviceInfo>& devices)
{
    m_band->showDevices(devices);
}

void OutputPage::showStatus(const EngineStatus& s)
{
    m_band->showStatus(s);
    m_radioConnected = s.connection == TciClient::State::Ready;
    showScopeNote();
}

void OutputPage::showChain()
{
    // Volume, BOOST and MUTE are not part of a profile, so a load leaves them as they were.
    m_output->syncControlsFromEngine();
}

void OutputPage::showEvent(QShowEvent* event)
{
    Page::showEvent(event);
    m_feed->start();
    showScopeNote();
}

void OutputPage::hideEvent(QHideEvent* event)
{
    m_feed->stop();
    Page::hideEvent(event);
}

bool OutputPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_scope && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        placeScopeNote();
    }
    return Page::eventFilter(watched, event);
}

void OutputPage::showScopeNote()
{
    if (!m_scopeNote) return;
    // Silence is fed only when nothing reaches the output: no radio, or a radio sending
    // nothing. A muted output still has audio reaching it, and MUTE says why it is quiet.
    const QString text = m_radioConnected ? QStringLiteral("No audio from radio") : QStringLiteral("Not connected");
    if (text != m_scopeNote->text()) {
        m_scopeNote->setText(text);
        placeScopeNote();
    }
    m_scopeNote->setVisible(m_feed->feedingSilence());
}

void OutputPage::placeScopeNote()
{
    if (!m_scopeNote || !m_scope) return;
    // Where upstream writes its "Enable PC Audio": the middle of the plot, which is the
    // scope less its readout line, its footer and its dB column (WaveformWidget::plotArea).
    const QRect plot = m_scope->rect().adjusted(5, 18, -34, -17);
    m_scopeNote->adjustSize();
    QRect note(QPoint(), m_scopeNote->size());
    note.moveCenter(plot.center());
    m_scopeNote->setGeometry(note);
}

} // namespace esa
