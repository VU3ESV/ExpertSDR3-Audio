#pragma once

#include <QDateTime>
#include <QString>

namespace esa::RecordNaming {

// Recording file names, in UTC so they sort the same wherever they were made:
//
//   yyyy-MM-dd_HHmmssZ_<MHz to 3 dp>MHz_<MODE>[_RX<n>]       2026-11-07_141503Z_14.200MHz_USB
//
// The engine calls baseName() when REC is accepted, with the frequency and mode RadioState
// holds then, and passes the result as RecordRequest::baseName. `_RX<n>` is added when the
// receiver is not 0, and always when no mode is known, so the name never ends in "MHz_".
QString baseName(const QDateTime& start, qint64 frequencyHz, const QString& mode, int receiver);

// "14.200MHz": rounded to the nearest kHz. Unknown (0 or less) is "0.000MHz".
QString frequencyToken(qint64 frequencyHz);

// The mode uppercased, then reduced to A-Z and 0-9: "usb" is "USB", "cw-l" is "CWL". Empty
// when nothing is left.
QString modeToken(const QString& mode);

// A clash with an existing file adds _1 to _999. Beyond that the recording is refused.
inline constexpr int kMaxCollisionSuffix = 999;

// The file name for part `part` (1 for the first) of a recording, with collision suffix
// `collision` (0 for none): "<base>.wav", "<base>_3.wav", "<base>_part2.wav", "<base>_part2_1.wav".
QString fileName(const QString& base, int part, int collision);

} // namespace esa::RecordNaming
