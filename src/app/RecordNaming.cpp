#include "app/RecordNaming.h"

#include <QChar>

namespace esa::RecordNaming {

namespace {

// Built from numbers rather than QDateTime::toString, so no locale can change a digit.
QString twoDigits(int v)
{
    return QStringLiteral("%1").arg(v, 2, 10, QLatin1Char('0'));
}

} // namespace

QString frequencyToken(qint64 frequencyHz)
{
    const qint64 kHz = frequencyHz > 0 ? (frequencyHz + 500) / 1000 : 0;
    return QStringLiteral("%1.%2MHz").arg(kHz / 1000).arg(kHz % 1000, 3, 10, QLatin1Char('0'));
}

QString modeToken(const QString& mode)
{
    QString out;
    for (const QChar c : mode.toUpper()) {
        if ((c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')))
            out.append(c);
    }
    return out;
}

QString baseName(const QDateTime& start, qint64 frequencyHz, const QString& mode, int receiver)
{
    const QDateTime utc = start.toUTC();
    const QDate d = utc.date();
    const QTime t = utc.time();
    QString name = QStringLiteral("%1-%2-%3_%4%5%6Z_")
                       .arg(d.year(), 4, 10, QLatin1Char('0'))
                       .arg(twoDigits(d.month()), twoDigits(d.day()), twoDigits(t.hour()),
                            twoDigits(t.minute()), twoDigits(t.second()));
    name += frequencyToken(frequencyHz);
    const QString m = modeToken(mode);
    if (!m.isEmpty()) name += QLatin1Char('_') + m;
    if (receiver != 0 || m.isEmpty()) name += QStringLiteral("_RX%1").arg(receiver);
    return name;
}

QString fileName(const QString& base, int part, int collision)
{
    QString name = base;
    if (part > 1) name += QStringLiteral("_part%1").arg(part);
    if (collision > 0) name += QStringLiteral("_%1").arg(collision);
    return name + QStringLiteral(".wav");
}

} // namespace esa::RecordNaming
