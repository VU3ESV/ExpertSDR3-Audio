#include "ReferenceChain.h"

#include "app/Profiles.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"

#include "SpectralNR.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <random>

using namespace esa;

namespace {

ChainSettings sampleChain()
{
    ChainSettings chain;
    chain.nrMethod = QStringLiteral("nr2");
    chain.nrAutoBypass = false;
    chain.stageOrder = {QStringLiteral("tube"), QStringLiteral("eq"), QStringLiteral("gate"), QStringLiteral("comp"), QStringLiteral("pudu")};
    chain.params.insert(QStringLiteral("comp.threshold"), -33.5);
    chain.params.insert(QStringLiteral("eq.b4.gain"), 7.0);
    chain.params.insert(QStringLiteral("nr2.smooth"), 0.5);
    return chain;
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(bytes);
}

QByteArray profileFile(const QJsonArray& profiles, int version = 1)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("format"), QStringLiteral("ExpertSDR3Audio.profiles")},
                                     {QStringLiteral("version"), version},
                                     {QStringLiteral("profiles"), profiles}}).toJson();
}

// The NR2 keys a v0.1.1 profile held, in its table's order: everything but the fill
// character, added later. The noise fill stays off, because its noise generator is
// seeded from the filter's address, as upstream's is: two NR2s never fill alike, not
// even two from the same build, so a comparison with it on could not be exact.
QList<QPair<QString, double>> v011Nr2Settings()
{
    return {
        {QStringLiteral("nr2.gainMax"), 1.2}, {QStringLiteral("nr2.gainFloor"), 0.1},
        {QStringLiteral("nr2.qspp"), 0.25}, {QStringLiteral("nr2.smooth"), 0.8},
        {QStringLiteral("nr2.gainMethod"), 2}, {QStringLiteral("nr2.npe"), 1},
        {QStringLiteral("nr2.ae"), 1}, {QStringLiteral("nr2.post2"), 0},
        {QStringLiteral("nr2.post2Level"), 0.3}, {QStringLiteral("nr2.post2Taper"), 4000},
    };
}

// A profile as v0.1.1 exported it: every key it had, and none it did not.
QJsonObject v011Profile(const QString& name)
{
    QJsonObject params;
    const QHash<QString, double> stages = reference::OldChain::v011Settings();
    for (const QString& key : reference::OldChain::keys()) {
        params.insert(key, stages.value(key, findParamSpec(key)->def));
    }
    for (const auto& [key, value] : v011Nr2Settings()) params.insert(key, value);
    return QJsonObject{{QStringLiteral("name"), name},
                       {QStringLiteral("createdBy"), QStringLiteral("ExpertSDR3 Audio 0.1.1")},
                       {QStringLiteral("nr"), QJsonObject{{QStringLiteral("method"), QStringLiteral("nr2")},
                                                          {QStringLiteral("autoBypass"), true}}},
                       {QStringLiteral("order"), QJsonArray{QStringLiteral("eq"), QStringLiteral("gate"),
                                                            QStringLiteral("comp"), QStringLiteral("tube"),
                                                            QStringLiteral("pudu")}},
                       {QStringLiteral("params"), params}};
}

// Steady noise with a little tone: enough for NR2 to find something to remove.
QByteArray testSignal(int rate, double seconds)
{
    const int frames = int(rate * seconds);
    QByteArray pcm(qsizetype(frames) * 2 * qsizetype(sizeof(float)), Qt::Uninitialized);
    auto* samples = reinterpret_cast<float*>(pcm.data());
    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.0, 0.02);
    for (int i = 0; i < frames; ++i) {
        const double tone = 0.1 * std::sin(2.0 * 3.14159265358979323846 * 700.0 * i / rate);
        samples[i * 2] = float(tone + noise(rng));
        samples[i * 2 + 1] = float(tone + noise(rng));
    }
    return pcm;
}

QByteArray inBlocks(const QByteArray& pcm, const std::function<QByteArray(const QByteArray&)>& process)
{
    QByteArray out;
    const int blockBytes = 512 * 2 * int(sizeof(float));
    for (qsizetype offset = 0; offset + blockBytes <= pcm.size(); offset += blockBytes) {
        out += process(pcm.mid(offset, blockBytes));
    }
    return out;
}

} // namespace

class TestProfiles : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString path(const char* name) const { return m_dir.filePath(QString::fromLatin1(name)); }

private slots:
    void init()
    {
        QDir dir(m_dir.path());
        for (const QString& file : dir.entryList(QDir::Files)) dir.remove(file);
    }

    void aSavedProfileComesBackAfterARestart()
    {
        {
            ProfileLibrary library(path("p.json"));
            QVERIFY(library.save(QStringLiteral("Rain Static"), sampleChain()));
        }
        ProfileLibrary library(path("p.json"));
        QCOMPARE(library.names(), QStringList{QStringLiteral("Rain Static")});
        const Profile* profile = library.find(QStringLiteral("Rain Static"));
        QVERIFY(profile);
        QVERIFY(sameChain(profile->chain, sampleChain()));
        QCOMPARE(profile->chain.nrMethod, QStringLiteral("nr2"));
        QVERIFY(!profile->chain.nrAutoBypass);
        QCOMPARE(profile->chain.stageOrder.first(), QStringLiteral("tube"));
        QCOMPARE(profile->chain.params.value(QStringLiteral("comp.threshold")), -33.5);
        QVERIFY(!profile->createdAt.isEmpty());
    }

    // So that changing a default in a later version cannot alter how a saved profile sounds.
    void aProfileStoresEveryParameterNotJustTheChangedOnes()
    {
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.save(QStringLiteral("All"), ChainSettings{}));
        const Profile* profile = library.find(QStringLiteral("All"));
        for (const ParamSpec& spec : allParamSpecs()) {
            QVERIFY2(profile->chain.params.contains(spec.key), qPrintable(spec.key));
            QCOMPARE(profile->chain.params.value(spec.key), spec.def);
        }
    }

    void namesAreNotCaseSensitive()
    {
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.save(QStringLiteral("DX"), sampleChain()));
        ChainSettings other = sampleChain();
        other.nrMethod.clear();
        QVERIFY(library.save(QStringLiteral("dx"), other));
        QCOMPARE(library.names(), QStringList{QStringLiteral("DX")});  // replaced, keeping its spelling
        QVERIFY(library.find(QStringLiteral("dX"))->chain.nrMethod.isEmpty());
    }

    void namesAreCleanedAndAnEmptyOneRefused()
    {
        QCOMPARE(ProfileLibrary::cleanName(QStringLiteral("  a\t  b\n")), QStringLiteral("a b"));
        QCOMPARE(ProfileLibrary::cleanName(QStringLiteral("x").repeated(200)).size(), ProfileLibrary::kMaxNameLength);
        QVERIFY(ProfileLibrary::cleanName(QStringLiteral("   ")).isEmpty());
        ProfileLibrary library(path("p.json"));
        QString error;
        QVERIFY(!library.save(QStringLiteral("  "), sampleChain(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(library.names().isEmpty());
    }

    void namesAreListedAlphabeticallyIgnoringCase()
    {
        ProfileLibrary library(path("p.json"));
        for (const char* name : {"beta", "Alpha", "gamma"}) QVERIFY(library.save(QString::fromLatin1(name), sampleChain()));
        QCOMPARE(library.names(), (QStringList{QStringLiteral("Alpha"), QStringLiteral("beta"), QStringLiteral("gamma")}));
    }

    void aProfileCanBeDeleted()
    {
        {
            ProfileLibrary library(path("p.json"));
            QVERIFY(library.save(QStringLiteral("Gone"), sampleChain()));
            QVERIFY(library.save(QStringLiteral("Kept"), sampleChain()));
            QVERIFY(library.remove(QStringLiteral("gone")));
        }
        QCOMPARE(ProfileLibrary(path("p.json")).names(), QStringList{QStringLiteral("Kept")});
    }

    void exportThenImportOnAnotherComputerIsIdentical()
    {
        ProfileLibrary here(path("here.json"));
        QVERIFY(here.save(QStringLiteral("Weak SSB"), sampleChain()));
        QVERIFY(here.exportFile(path("export.json"), {QStringLiteral("Weak SSB")}));

        ProfileLibrary there(path("there.json"));
        const ImportResult result = there.importFile(path("export.json"));
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.imported, QStringList{QStringLiteral("Weak SSB")});
        QVERIFY(sameChain(there.find(QStringLiteral("Weak SSB"))->chain, here.find(QStringLiteral("Weak SSB"))->chain));
        // And it persisted, rather than only being in memory.
        QVERIFY(ProfileLibrary(path("there.json")).find(QStringLiteral("Weak SSB")));
    }

    void anExportHoldsOnlyWhatWasAskedFor()
    {
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.save(QStringLiteral("One"), sampleChain()));
        QVERIFY(library.save(QStringLiteral("Two"), sampleChain()));
        QVERIFY(library.exportFile(path("one.json"), {QStringLiteral("One")}));
        ProfileLibrary other(path("o.json"));
        QCOMPARE(other.importFile(path("one.json")).imported, QStringList{QStringLiteral("One")});
    }

    void exportingSeveralAtOnceWorks()
    {
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.save(QStringLiteral("One"), sampleChain()));
        QVERIFY(library.save(QStringLiteral("Two"), sampleChain()));
        QVERIFY(library.exportFile(path("all.json"), library.names()));
        ProfileLibrary other(path("o.json"));
        QCOMPARE(other.importFile(path("all.json")).imported.size(), 2);
    }

    void exportingNothingIsAnError()
    {
        ProfileLibrary library(path("p.json"));
        QString error;
        QVERIFY(!library.exportFile(path("none.json"), {QStringLiteral("Missing")}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(path("none.json")));
    }

    // Importing must never quietly replace something the operator has already tuned.
    void importNeverOverwritesAnExistingProfile()
    {
        ProfileLibrary source(path("s.json"));
        QVERIFY(source.save(QStringLiteral("DX"), sampleChain()));
        QVERIFY(source.exportFile(path("e.json"), {QStringLiteral("DX")}));

        ProfileLibrary library(path("p.json"));
        QVERIFY(library.save(QStringLiteral("dx"), ChainSettings{}));
        const ImportResult result = library.importFile(path("e.json"));
        QCOMPARE(result.imported, QStringList{QStringLiteral("DX (2)")});
        QCOMPARE(result.renamed.size(), 1);
        QVERIFY(library.find(QStringLiteral("dx"))->chain.nrMethod.isEmpty());  // untouched
        QCOMPARE(library.find(QStringLiteral("DX (2)"))->chain.nrMethod, QStringLiteral("nr2"));

        QCOMPARE(library.importFile(path("e.json")).imported, QStringList{QStringLiteral("DX (3)")});
    }

    void importRejectsWhatIsNotAProfileFile_data()
    {
        QTest::addColumn<QByteArray>("content");
        QTest::newRow("not JSON") << QByteArray("this is not json");
        QTest::newRow("empty") << QByteArray("");
        QTest::newRow("a JSON array") << QByteArray("[1,2,3]");
        QTest::newRow("some other JSON") << QByteArray(R"({"name":"package","version":"1.0.0"})");
        QTest::newRow("AetherSDR's own format") << QByteArray(R"({"version":1,"profiles":{"DX":{"nr":{"method":"NR4"}}}})");
    }

    void importRejectsWhatIsNotAProfileFile()
    {
        QFETCH(QByteArray, content);
        writeFile(path("bad.json"), content);
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.save(QStringLiteral("Existing"), sampleChain()));
        const ImportResult result = library.importFile(path("bad.json"));
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.imported.isEmpty());
        QCOMPARE(library.names(), QStringList{QStringLiteral("Existing")});
    }

    void importRefusesAFileFromANewerVersion()
    {
        writeFile(path("new.json"), profileFile({QJsonObject{{QStringLiteral("name"), QStringLiteral("X")}}}, 99));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("new.json")).error.contains(QStringLiteral("newer")));
    }

    void importReportsAMissingFile()
    {
        ProfileLibrary library(path("p.json"));
        QVERIFY(!library.importFile(path("does-not-exist.json")).error.isEmpty());
    }

    void importRefusesAnAbsurdlyLargeFile()
    {
        writeFile(path("huge.json"), QByteArray(5 * 1024 * 1024, ' '));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("huge.json")).error.contains(QStringLiteral("too large")));
    }

    void importWithNoUsableProfilesIsAnError()
    {
        writeFile(path("empty.json"), profileFile({}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(!library.importFile(path("empty.json")).error.isEmpty());
    }

    // A file is untrusted input. Out-of-range values must not reach the DSP.
    void importClampsValuesToTheirRanges()
    {
        const QJsonObject params{{QStringLiteral("comp.threshold"), 9999},
                                 {QStringLiteral("comp.ratio"), -5},
                                 {QStringLiteral("eq.b4.gain"), QStringLiteral("loud")}};
        writeFile(path("wild.json"), profileFile({QJsonObject{{QStringLiteral("name"), QStringLiteral("Wild")},
                                                              {QStringLiteral("params"), params}}}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("wild.json")).error.isEmpty());

        AppConfig config;
        applyChain(config, library.find(QStringLiteral("Wild"))->chain);
        QCOMPARE(paramValue(config, *findParamSpec(QStringLiteral("comp.threshold"))), 0.0);
        QCOMPARE(paramValue(config, *findParamSpec(QStringLiteral("comp.ratio"))), 1.0);
        QCOMPARE(paramValue(config, *findParamSpec(QStringLiteral("eq.b4.gain"))), 0.0);  // a non-number: the default
    }

    // A value the file leaves out means today's default, and must stay that if a default changes.
    void anImportedProfileStoresEveryParameterToo()
    {
        writeFile(path("few.json"), profileFile({QJsonObject{{QStringLiteral("name"), QStringLiteral("Few")},
            {QStringLiteral("params"), QJsonObject{{QStringLiteral("comp.threshold"), -30}}}}}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("few.json")).error.isEmpty());
        const Profile* profile = library.find(QStringLiteral("Few"));
        QCOMPARE(profile->chain.params.value(QStringLiteral("comp.threshold")), -30.0);
        for (const ParamSpec& spec : allParamSpecs()) QVERIFY2(profile->chain.params.contains(spec.key), qPrintable(spec.key));
    }

    void importSkipsUnusableEntriesAndKeepsTheRest()
    {
        const QJsonArray list{QStringLiteral("not an object"), QJsonObject{},
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("   ")}},
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("Good")}}};
        writeFile(path("mixed.json"), profileFile(list));
        ProfileLibrary library(path("p.json"));
        QCOMPARE(library.importFile(path("mixed.json")).imported, QStringList{QStringLiteral("Good")});
    }

    void importFixesABadStageOrder()
    {
        const QJsonArray order{QStringLiteral("tube"), QStringLiteral("nonsense"), QStringLiteral("tube")};
        writeFile(path("o.json"), profileFile({QJsonObject{{QStringLiteral("name"), QStringLiteral("O")},
                                                           {QStringLiteral("order"), order}}}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("o.json")).error.isEmpty());
        const QStringList stages = library.find(QStringLiteral("O"))->chain.stageOrder;
        QCOMPARE(stages.first(), QStringLiteral("tube"));
        QCOMPARE(stages.size(), RxChain::stageIds().size());
    }

    // A Mac profile on Linux: MNR is not there. It must play as no NR, not crash or pick another,
    // and must survive being passed along, so the Mac can have it back intact.
    void aMethodThisBuildLacksPlaysAsOffButSurvivesARoundTrip()
    {
        const QJsonObject nr{{QStringLiteral("method"), QStringLiteral("from-another-platform")}};
        const QJsonObject params{{QStringLiteral("from-another-platform.amount"), 0.7}};
        writeFile(path("x.json"), profileFile({QJsonObject{{QStringLiteral("name"), QStringLiteral("Foreign")},
                                                           {QStringLiteral("nr"), nr},
                                                           {QStringLiteral("params"), params}}}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("x.json")).error.isEmpty());
        const Profile* profile = library.find(QStringLiteral("Foreign"));

        AppConfig config;
        config.nrMethod = QStringLiteral("nr2");
        applyChain(config, profile->chain);
        QVERIFY(config.nrMethod.isEmpty());
        QVERIFY(!config.params.contains(QStringLiteral("from-another-platform.amount")));

        QVERIFY(library.exportFile(path("back.json"), {QStringLiteral("Foreign")}));
        ProfileLibrary other(path("o.json"));
        QVERIFY(other.importFile(path("back.json")).error.isEmpty());
        const Profile* returned = other.find(QStringLiteral("Foreign"));
        QCOMPARE(returned->chain.nrMethod, QStringLiteral("from-another-platform"));
        QCOMPARE(returned->chain.params.value(QStringLiteral("from-another-platform.amount")), 0.7);
    }

    void applyingAChainReplacesEverythingInIt()
    {
        AppConfig config;
        config.params.insert(QStringLiteral("tube.drive"), 12.0);  // not in the profile: must go back
        config.deviceKey = QStringLiteral("My Speakers");
        config.outputGainDb = -6;
        applyChain(config, sampleChain());
        QCOMPARE(config.nrMethod, QStringLiteral("nr2"));
        QVERIFY(!config.nrAutoBypass);
        QCOMPARE(config.stageOrder.first(), QStringLiteral("tube"));
        QCOMPARE(paramValue(config, *findParamSpec(QStringLiteral("comp.threshold"))), -33.5);
        QCOMPARE(paramValue(config, *findParamSpec(QStringLiteral("tube.drive"))), 0.0);
        // A profile is about the sound. This station's hardware stays as it was.
        QCOMPARE(config.deviceKey, QStringLiteral("My Speakers"));
        QCOMPARE(config.outputGainDb, -6.0);
    }

    void chainOfThenApplyRoundTrips()
    {
        AppConfig first;
        applyChain(first, sampleChain());
        AppConfig second;
        applyChain(second, chainOf(first));
        QVERIFY(sameChain(chainOf(first), chainOf(second)));
    }

    void sameChainNoticesEveryKindOfDifference()
    {
        const ChainSettings base = sampleChain();
        QVERIFY(sameChain(base, base));

        ChainSettings method = base;
        method.nrMethod = QStringLiteral("rn2");
        QVERIFY(!sameChain(base, method));

        ChainSettings bypass = base;
        bypass.nrAutoBypass = true;
        QVERIFY(!sameChain(base, bypass));

        ChainSettings order = base;
        order.stageOrder = RxChain::stageIds();
        QVERIFY(!sameChain(base, order));

        ChainSettings value = base;
        value.params.insert(QStringLiteral("comp.threshold"), -33.0);
        QVERIFY(!sameChain(base, value));
    }

    void anUnsetValueMeansItsDefault()
    {
        ChainSettings implicit;
        ChainSettings explicitDefault;
        explicitDefault.params.insert(QStringLiteral("comp.threshold"), findParamSpec(QStringLiteral("comp.threshold"))->def);
        QVERIFY(sameChain(implicit, explicitDefault));
        QVERIFY(sameChain(defaultChain(), ChainSettings{}));
    }

    void foreignContentDoesNotCountAsAChange()
    {
        ChainSettings foreign;
        foreign.nrMethod = QStringLiteral("from-another-platform");
        foreign.params.insert(QStringLiteral("from-another-platform.amount"), 0.7);
        QVERIFY(sameChain(foreign, defaultChain()));
    }

    // Never destroy what could not be read: it may be the operator's only copy.
    void anUnreadableLibraryIsSetAsideNotOverwritten()
    {
        writeFile(path("p.json"), "{ corrupted");
        ProfileLibrary library(path("p.json"));
        QVERIFY(!library.loadWarning().isEmpty());
        QVERIFY(library.names().isEmpty());
        QVERIFY(library.save(QStringLiteral("New"), sampleChain()));

        QStringList backups = QDir(m_dir.path()).entryList({QStringLiteral("p.json.unreadable-*")});
        QCOMPARE(backups.size(), 1);
        QFile backup(QDir(m_dir.path()).filePath(backups.first()));
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), QByteArray("{ corrupted"));
    }

    void aLibraryFromANewerVersionIsSetAsideToo()
    {
        writeFile(path("p.json"), profileFile({}, 99));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.loadWarning().contains(QStringLiteral("newer")));
        QCOMPARE(QDir(m_dir.path()).entryList({QStringLiteral("p.json.unreadable-*")}).size(), 1);
    }

    void aMissingLibraryIsSimplyEmpty()
    {
        ProfileLibrary library(path("never-created.json"));
        QVERIFY(library.names().isEmpty());
        QVERIFY(library.loadWarning().isEmpty());
        QVERIFY(!QFile::exists(path("never-created.json")));
    }

    void savingCreatesTheFoldersItNeeds()
    {
        const QString nested = m_dir.filePath(QStringLiteral("a/b/c/p.json"));
        ProfileLibrary library(nested);
        QVERIFY(library.save(QStringLiteral("N"), sampleChain()));
        QVERIFY(QFile::exists(nested));
    }

    void aFailedSaveLeavesTheLibraryAsItWas()
    {
        // Its parent is a file, so no folder can be made there.
        writeFile(path("blocker"), "x");
        ProfileLibrary library(m_dir.filePath(QStringLiteral("blocker/p.json")));
        QString error;
        QVERIFY(!library.save(QStringLiteral("N"), sampleChain(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(library.names().isEmpty());
    }

    void uniqueNamesStayWithinTheLengthLimit()
    {
        ProfileLibrary library(path("p.json"));
        const QString longName = QStringLiteral("x").repeated(ProfileLibrary::kMaxNameLength);
        QVERIFY(library.save(longName, sampleChain()));
        const QString next = library.uniqueName(longName);
        QVERIFY(next != longName);
        QVERIFY(next.size() <= ProfileLibrary::kMaxNameLength);
    }

    void testRunsKeepAwayFromRealProfiles()
    {
        QCOMPARE(ProfileLibrary::defaultPath(QStringLiteral("/tmp/run.ini")), QStringLiteral("/tmp/run.ini.profiles.json"));
    }

    // ── Profiles saved by an older version ───────────────────────────────────

    // A v0.1.1 profile holds no band types, slopes or fill character. Loaded here, as
    // the engine loads one, it must sound exactly as it did, sample for sample.
    void aProfileFromTheLastReleaseSoundsExactlyAsItDid()
    {
        writeFile(path("old.json"), profileFile({v011Profile(QStringLiteral("From 0.1.1"))}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("old.json")).error.isEmpty());
        AppConfig config;
        applyChain(config, library.find(QStringLiteral("From 0.1.1"))->chain);
        QCOMPARE(config.nrMethod, QStringLiteral("nr2"));

        // What Engine::rebuildChain and startNrBuild do with that configuration.
        constexpr int kRate = 48000;
        RxChain chain(kRate);
        chain.setStageOrder(config.stageOrder);
        for (const ParamSpec& spec : RxChain::stageParams()) chain.setStageParam(spec.key, paramValue(config, spec));
        const NrMethod* nr2 = findNrMethod(QStringLiteral("nr2"));
        std::shared_ptr<NrProcessor> nr(nr2->create(kRate).release());
        for (const ParamSpec& spec : nr2->params) nr->setParam(spec.key, paramValue(config, spec));
        chain.setNrProcessor(nr);

        // v0.1.1's chain with exactly the keys it knew, and an NR2 told only those.
        reference::OldChain old(kRate);
        old.setOrder(config.stageOrder);
        const QHash<QString, double> stages = reference::OldChain::v011Settings();
        for (const QString& key : reference::OldChain::keys()) old.set(key, stages.value(key, findParamSpec(key)->def));
        std::shared_ptr<NrProcessor> oldNr(nr2->create(kRate).release());
        for (const auto& [key, value] : v011Nr2Settings()) oldNr->setParam(key, value);
        old.setNr(oldNr);

        const QByteArray in = testSignal(kRate, 3.0);
        const QByteArray out = inBlocks(in, [&](const QByteArray& block) { return chain.process(block); });
        const QByteArray expected = inBlocks(in, [&](const QByteArray& block) { return old.process(block); });
        QVERIFY(!out.isEmpty());
        QCOMPARE(out.size(), expected.size());
        QVERIFY2(std::memcmp(out.constData(), expected.constData(), size_t(out.size())) == 0,
                 "a v0.1.1 profile no longer sounds the same");
    }

    // The fill character is the one new key that reaches a filter which had a value for
    // it all along. Its default must be that value, or adding the key changes the sound
    // of every profile that has the noise fill on.
    void theFillCharactersDefaultIsTheFiltersOwn()
    {
        const AetherSDR::SpectralNR filter(1024, 24000, 4);
        QCOMPARE(float(findParamSpec(QStringLiteral("nr2.post2Factor"))->def), filter.post2Factor());
        QCOMPARE(float(findParamSpec(QStringLiteral("nr2.post2Taper"))->def), filter.post2TaperHz());

        // And the band's new range is what the filter always accepted.
        AetherSDR::SpectralNR clamped(1024, 24000, 4);
        clamped.setPost2TaperHz(8000.0f);
        QCOMPARE(float(findParamSpec(QStringLiteral("nr2.post2Taper"))->max), clamped.post2TaperHz());
        clamped.setPost2TaperHz(100.0f);
        QCOMPARE(float(findParamSpec(QStringLiteral("nr2.post2Taper"))->min), clamped.post2TaperHz());
    }

    void aProfileWithoutBandTypesKeepsTheLayoutItHad()
    {
        writeFile(path("old.json"), profileFile({v011Profile(QStringLiteral("Old"))}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("old.json")).error.isEmpty());
        AppConfig config;
        applyChain(config, library.find(QStringLiteral("Old"))->chain);
        StageSet stages;
        stages.prepare(48000);
        stages.initialiseBands();
        for (const ParamSpec& spec : RxChain::stageParams()) RxChain::applyStageParam(stages, spec.key, paramValue(config, spec));
        for (int band = 0; band < AetherSDR::ClientEq::kDefaultBandCount; ++band) {
            QVERIFY(stages.eq.band(band).type == AetherSDR::ClientEq::defaultBand(band).type);
            QCOMPARE(stages.eq.band(band).slopeDbPerOct, 12);
        }
    }

    // An older version keeps keys it does not know through an import and an export
    // (fromJson and completed() above), so a profile with band types can pass through
    // it and come back whole. The same code does that here, for keys from the future.
    void keysThisVersionDoesNotKnowSurviveARoundTrip()
    {
        QJsonObject profile = v011Profile(QStringLiteral("Future"));
        QJsonObject params = profile.value(QStringLiteral("params")).toObject();
        params.insert(QStringLiteral("eq.b4.futureShape"), 3.0);
        params.insert(QStringLiteral("gate.futureKnob"), 0.25);
        profile.insert(QStringLiteral("params"), params);
        writeFile(path("future.json"), profileFile({profile}));

        ProfileLibrary here(path("here.json"));
        QVERIFY(here.importFile(path("future.json")).error.isEmpty());
        QVERIFY(here.exportFile(path("back.json"), {QStringLiteral("Future")}));
        ProfileLibrary there(path("there.json"));
        QVERIFY(there.importFile(path("back.json")).error.isEmpty());
        const Profile* returned = there.find(QStringLiteral("Future"));
        QCOMPARE(returned->chain.params.value(QStringLiteral("eq.b4.futureShape")), 3.0);
        QCOMPARE(returned->chain.params.value(QStringLiteral("gate.futureKnob")), 0.25);
    }

    // What an older version reads from a new export: the new keys as plain numbers.
    void anExportCarriesTheNewKeysAsPlainNumbers()
    {
        ChainSettings chain = sampleChain();
        chain.params.insert(QStringLiteral("eq.b3.type"), 1.0);
        chain.params.insert(QStringLiteral("eq.b0.slope"), 2.0);
        chain.params.insert(QStringLiteral("nr2.post2Factor"), 0.6);
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.save(QStringLiteral("New"), chain));
        const QJsonObject params = QJsonDocument::fromJson(library.exportDocument({QStringLiteral("New")}))
                                       .object().value(QStringLiteral("profiles")).toArray().first().toObject()
                                       .value(QStringLiteral("params")).toObject();
        QCOMPARE(params.value(QStringLiteral("eq.b3.type")).toDouble(-1), 1.0);
        QCOMPARE(params.value(QStringLiteral("eq.b0.slope")).toDouble(-1), 2.0);
        QCOMPARE(params.value(QStringLiteral("nr2.post2Factor")).toDouble(-1), 0.6);
        QVERIFY(params.value(QStringLiteral("eq.b9.type")).isDouble());  // every band's, as every key is stored
    }

    // Both ranges shrank to what the DSP had always clamped to, so a stored value beyond
    // them loads clamped and plays exactly as before.
    void valuesPastWhatTheDspAcceptedClampToItsLimit_data()
    {
        QTest::addColumn<QString>("key");
        QTest::addColumn<double>("stored");
        QTest::addColumn<double>("loaded");
        QTest::newRow("tube output") << QStringLiteral("tube.output") << 20.0 << 12.0;
        QTest::newRow("noise fill band") << QStringLiteral("nr2.post2Taper") << 7000.0 << 6000.0;
    }

    void valuesPastWhatTheDspAcceptedClampToItsLimit()
    {
        QFETCH(QString, key);
        QFETCH(double, stored);
        QFETCH(double, loaded);

        writeFile(path("wide.json"), profileFile({QJsonObject{{QStringLiteral("name"), QStringLiteral("Wide")},
                                                              {QStringLiteral("params"), QJsonObject{{key, stored}}}}}));
        ProfileLibrary library(path("p.json"));
        QVERIFY(library.importFile(path("wide.json")).error.isEmpty());
        AppConfig config;
        applyChain(config, library.find(QStringLiteral("Wide"))->chain);
        QCOMPARE(paramValue(config, *findParamSpec(key)), loaded);

        // The settings store too, where the last session's values live.
        const QString ini = path("s.ini");
        {
            QSettings raw(ini, QSettings::IniFormat);
            raw.setValue(QStringLiteral("params/") + key, stored);
        }
        QCOMPARE(Settings(ini).load().params.value(key), loaded);
    }
};

QTEST_GUILESS_MAIN(TestProfiles)
#include "test_profiles.moc"
