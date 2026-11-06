#include "tci/TciProtocol.h"

#include <QtEndian>
#include <QtTest>

#include <cmath>
#include <limits>

using namespace esa::tci;

namespace {

QByteArray makeFrame(quint32 format, quint32 channels, quint32 length, const QByteArray& payload,
                     quint32 type = 1, quint32 codec = 0)
{
    QByteArray frame(kHeaderBytes, '\0');
    const auto put = [&](int index, quint32 value) {
        qToLittleEndian(value, frame.data() + index * 4);
    };
    put(0, 0);
    put(1, 48000);
    put(2, format);
    put(3, codec);
    put(5, length);
    put(6, type);
    put(7, channels);
    put(8, 7);
    return frame + payload;
}

template <class T>
QByteArray bytesOf(std::initializer_list<T> values)
{
    QByteArray out;
    for (T value : values) {
        T le = qToLittleEndian(value);
        out.append(reinterpret_cast<const char*>(&le), sizeof(T));
    }
    return out;
}

QByteArray floats(std::initializer_list<float> values)
{
    QByteArray out;
    for (float value : values) out.append(reinterpret_cast<const char*>(&value), sizeof(float));
    return out;
}

int decode(const QByteArray& frame, std::vector<float>& out)
{
    const auto header = parseHeader(frame);
    if (!header) return -2;
    return decodeToStereo(*header, frame.constData() + kHeaderBytes, frame.size() - kHeaderBytes, out);
}

} // namespace

class TestTciProtocol : public QObject {
    Q_OBJECT

private slots:
    void parsesACommandWithArguments()
    {
        const auto commands = parseCommands(QStringLiteral("vfo:0,1,7024000;"));
        QCOMPARE(commands.size(), 1);
        QCOMPARE(commands[0].name, QStringLiteral("vfo"));
        QCOMPARE(commands[0].args, (QStringList{"0", "1", "7024000"}));
    }

    void parsesSeveralCommandsInOneFrame()
    {
        const auto commands = parseCommands(QStringLiteral("mute:true;ready;\nrx_mute:0,false;"));
        QCOMPARE(commands.size(), 3);
        QCOMPARE(commands[1].name, QStringLiteral("ready"));
        QVERIFY(commands[1].args.isEmpty());
        QCOMPARE(commands[2].args.at(1), QStringLiteral("false"));
    }

    void namesAreCaseInsensitive()
    {
        QCOMPARE(parseCommands(QStringLiteral("MUTE:TRUE;"))[0].name, QStringLiteral("mute"));
    }

    void ignoresEmptyFragments()
    {
        QVERIFY(parseCommands(QStringLiteral(";;  ;")).isEmpty());
        QVERIFY(parseCommands(QString()).isEmpty());
    }

    void boolsFollowTheSpec()
    {
        QVERIFY(parseBool(QStringLiteral("true")));
        QVERIFY(parseBool(QStringLiteral("TRUE")));
        QVERIFY(parseBool(QStringLiteral("1")));
        QVERIFY(!parseBool(QStringLiteral("false")));
        QVERIFY(!parseBool(QStringLiteral("0")));
        QVERIFY(!parseBool(QStringLiteral("yes")));
    }

    void shortFrameHasNoHeader()
    {
        QVERIFY(!parseHeader(QByteArray(63, '\0')));
        QVERIFY(parseHeader(QByteArray(64, '\0')));
    }

    void headerFieldsSitWhereTheSpecSays()
    {
        const auto header = parseHeader(makeFrame(3, 2, 4096, {}));
        QVERIFY(header);
        QCOMPARE(header->sampleRate, 48000u);
        QCOMPARE(header->format, 3u);
        QCOMPARE(header->length, 4096u);
        QCOMPARE(header->type, 1u);
        QCOMPARE(header->channels, 2u);
        QCOMPARE(header->count, 7u);
    }

    // The block ExpertSDR3 2.1 really sends: length counts real samples, not frames.
    void lengthCountsRealSamplesNotFrames()
    {
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(3, 2, 4, floats({0.1f, 0.2f, 0.3f, 0.4f})), out), 2);
        QCOMPARE(out.size(), size_t(4));
        QCOMPARE(out[0], 0.1f);
        QCOMPARE(out[3], 0.4f);
    }

    void monoIsDuplicatedToBothChannels()
    {
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(3, 1, 2, floats({0.25f, -0.5f})), out), 2);
        QCOMPARE(out, (std::vector<float>{0.25f, 0.25f, -0.5f, -0.5f}));
    }

    void decodesInt16()
    {
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(0, 2, 2, bytesOf<qint16>({16384, -32768})), out), 1);
        QCOMPARE(out[0], 0.5f);
        QCOMPARE(out[1], -1.0f);
    }

    void decodesInt24WithSignExtension()
    {
        // 0x400000 = +0.5, 0xC00000 = -0.5, 0xFFFFFF = -1 LSB, 0x800000 = -1.0
        QByteArray samples;
        samples.append(QByteArray::fromHex("000040"));
        samples.append(QByteArray::fromHex("0000C0"));
        samples.append(QByteArray::fromHex("FFFFFF"));
        samples.append(QByteArray::fromHex("000080"));
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(1, 2, 4, samples), out), 2);
        QCOMPARE(out[0], 0.5f);
        QCOMPARE(out[1], -0.5f);
        QVERIFY(out[2] < 0.0f && out[2] > -1e-6f);
        QCOMPARE(out[3], -1.0f);
    }

    void decodesInt32()
    {
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(2, 2, 2, bytesOf<qint32>({1073741824, std::numeric_limits<qint32>::min()})), out), 1);
        QCOMPARE(out[0], 0.5f);
        QCOMPARE(out[1], -1.0f);
    }

    void rejectsWhatItCannotDecodeSafely()
    {
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(9, 2, 2, floats({0, 0})), out), -1);                 // unknown format
        QCOMPARE(decode(makeFrame(3, 3, 3, floats({0, 0, 0})), out), -1);              // 3 channels
        QCOMPARE(decode(makeFrame(3, 0, 2, floats({0, 0})), out), -1);                 // 0 channels
        QCOMPARE(decode(makeFrame(3, 2, 2, floats({0, 0}), 1, 1), out), -1);           // compressed
        QCOMPARE(decode(makeFrame(3, 2, 4096, floats({0, 0})), out), -1);              // truncated
    }

    // A corrupt length must not wrap a 32-bit size check and read out of bounds.
    void hugeLengthCannotOverflowTheBoundsCheck()
    {
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(3, 2, 0x40000000u, floats({0, 0})), out), -1);
        QCOMPARE(decode(makeFrame(3, 2, 0xFFFFFFFFu, floats({0, 0})), out), -1);
    }

    void emptyBlockIsZeroFramesNotAnError()
    {
        std::vector<float> out;
        QCOMPARE(decode(makeFrame(3, 2, 0, {}), out), 0);
    }
};

QTEST_APPLESS_MAIN(TestTciProtocol)
#include "test_tci_protocol.moc"
