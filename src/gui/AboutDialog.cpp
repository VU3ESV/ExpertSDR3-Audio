#include "gui/AboutDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace esa {

namespace {
const QString kRepository = QStringLiteral("https://github.com/VU3ESV/ExpertSDR3-Audio");
}

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("About ExpertSDR3 Audio"));

    auto* icon = new QLabel;
    icon->setObjectName(QStringLiteral("aboutIcon"));
    icon->setPixmap(QPixmap(QStringLiteral(":/icons/ExpertSDR3Audio-256.png"))
                        .scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    icon->setAlignment(Qt::AlignTop);

    auto* heading = new QLabel(QStringLiteral(
        "<div style='font-size:18pt; font-weight:600'>ExpertSDR3 Audio</div>"
        "<div>Version %1</div>"
        "<p>ExpertSDR3 receive audio through the AetherRX noise-reduction chain.</p>"
        "<p><b>Developer:</b> VU3ESV/LB9KJ<br>"
        "<a href='%2'>%2</a></p>")
        .arg(QCoreApplication::applicationVersion(), kRepository));
    heading->setObjectName(QStringLiteral("aboutHeading"));
    heading->setTextFormat(Qt::RichText);
    heading->setWordWrap(true);  // the dialog's width is fixed, and the description is long
    heading->setOpenExternalLinks(true);
    heading->setTextInteractionFlags(Qt::TextBrowserInteraction);

    auto* credits = new QLabel(QStringLiteral(
        "<p><b>Built on</b><br>"
        "AetherSDR's AetherRX chain and GUI widgets (GPL-3.0), the Inter typeface (SIL Open Font License 1.1), "
        "WDSP by NR0V (GPL-2.0+), RNNoise (BSD), "
        "DeepFilterNet (MIT, Apache-2.0), libspecbleach (LGPL-2.1), r8brain (MIT), FFTW (GPL-2.0+), "
        "PortAudio (MIT) and Qt %1 (LGPL-3.0).</p>"
        "<p>Free software under the GNU General Public License, version 3, "
        "with no warranty of any kind. "
        "<a href='%2/blob/main/LICENSE'>Read the license</a>.</p>")
        .arg(QString::fromLatin1(qVersion()), kRepository));
    credits->setTextFormat(Qt::RichText);
    credits->setOpenExternalLinks(true);
    credits->setWordWrap(true);

    auto* text = new QVBoxLayout;
    text->addWidget(heading);
    text->addWidget(credits);

    auto* top = new QHBoxLayout;
    top->addWidget(icon);
    top->addSpacing(12);
    top->addLayout(text, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto* aboutQt = buttons->addButton(QStringLiteral("About Qt"), QDialogButtonBox::ActionRole);
    connect(aboutQt, &QPushButton::clicked, qApp, &QApplication::aboutQt);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(buttons);
    setFixedWidth(520);
}

} // namespace esa
