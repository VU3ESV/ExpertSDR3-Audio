#include "app/Profiles.h"
#include "dsp/NrProcessor.h"
#include "dsp/RxChain.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

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
};

QTEST_GUILESS_MAIN(TestProfiles)
#include "test_profiles.moc"
