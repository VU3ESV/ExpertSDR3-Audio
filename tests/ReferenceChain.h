#pragma once

// The receive chain as v0.1.1 built it, before StageSet and the read table: five stage
// members, the old parameter table written out against the vendored setters, and the
// old process loop. Tests run the same signal through this and through RxChain, so a
// refactor that changes a single sample fails on every platform, without a stored
// reference that would only match one compiler's floating point.
//
// Only the keys v0.1.1 had are known here. That is the point: a v0.1.1 profile holds
// exactly these, and must sound the same in a build that has more.

#include "dsp/NrProcessor.h"

#include "ClientComp.h"
#include "ClientEq.h"
#include "ClientGate.h"
#include "ClientPudu.h"
#include "ClientTube.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <memory>

namespace reference {

class OldChain {
public:
    explicit OldChain(int rate)
    {
        m_gate.prepare(rate);
        m_eq.prepare(rate);
        m_comp.prepare(rate);
        m_tube.prepare(rate);
        m_pudu.prepare(rate);
        m_eq.setActiveBandCount(AetherSDR::ClientEq::kDefaultBandCount);
        for (int band = 0; band < AetherSDR::ClientEq::kDefaultBandCount; ++band) {
            m_eq.setBand(band, AetherSDR::ClientEq::defaultBand(band));
        }
    }

    void setOrder(const QStringList& order) { m_order = order; }
    void setNr(std::shared_ptr<esa::NrProcessor> nr) { m_nr = std::move(nr); }

    // v0.1.1's table, entry by entry. False for a key it did not have.
    bool set(const QString& key, double v)
    {
        using namespace AetherSDR;
        const auto is = [&](const char* name) { return key == QLatin1String(name); };
        if (is("gate.enabled")) m_gate.setEnabled(v != 0.0);
        else if (is("gate.mode")) m_gate.setMode(static_cast<ClientGate::Mode>(int(v)));
        else if (is("gate.threshold")) m_gate.setThresholdDb(float(v));
        else if (is("gate.ratio")) m_gate.setRatio(float(v));
        else if (is("gate.attack")) m_gate.setAttackMs(float(v));
        else if (is("gate.release")) m_gate.setReleaseMs(float(v));
        else if (is("gate.hold")) m_gate.setHoldMs(float(v));
        else if (is("gate.floor")) m_gate.setFloorDb(float(v));
        else if (is("gate.return")) m_gate.setReturnDb(float(v));
        else if (is("gate.lookahead")) m_gate.setLookaheadMs(float(v));
        else if (is("eq.enabled")) m_eq.setEnabled(v != 0.0);
        else if (is("eq.master")) m_eq.setMasterGain(float(std::pow(10.0, v / 20.0)));
        else if (is("eq.family")) m_eq.setFilterFamily(static_cast<ClientEq::FilterFamily>(int(v)));
        else if (is("comp.enabled")) m_comp.setEnabled(v != 0.0);
        else if (is("comp.threshold")) m_comp.setThresholdDb(float(v));
        else if (is("comp.ratio")) m_comp.setRatio(float(v));
        else if (is("comp.attack")) m_comp.setAttackMs(float(v));
        else if (is("comp.release")) m_comp.setReleaseMs(float(v));
        else if (is("comp.knee")) m_comp.setKneeDb(float(v));
        else if (is("comp.makeup")) m_comp.setMakeupDb(float(v));
        else if (is("comp.drive")) m_comp.setDriveDb(float(v));
        else if (is("comp.rotator")) m_comp.setPhaseRotatorStages(int(v));
        else if (is("comp.limiter")) m_comp.setLimiterEnabled(v != 0.0);
        else if (is("comp.ceiling")) m_comp.setLimiterCeilingDb(float(v));
        else if (is("tube.enabled")) m_tube.setEnabled(v != 0.0);
        else if (is("tube.model")) m_tube.setModel(static_cast<ClientTube::Model>(int(v)));
        else if (is("tube.drive")) m_tube.setDriveDb(float(v));
        else if (is("tube.bias")) m_tube.setBiasAmount(float(v));
        else if (is("tube.tone")) m_tube.setTone(float(v));
        else if (is("tube.output")) m_tube.setOutputGainDb(float(v));
        else if (is("tube.mix")) m_tube.setDryWet(float(v));
        else if (is("tube.envelope")) m_tube.setEnvelopeAmount(float(v));
        else if (is("tube.attack")) m_tube.setAttackMs(float(v));
        else if (is("tube.release")) m_tube.setReleaseMs(float(v));
        else if (is("pudu.enabled")) m_pudu.setEnabled(v != 0.0);
        else if (is("pudu.mode")) m_pudu.setMode(static_cast<ClientPudu::Mode>(int(v)));
        else if (is("pudu.pooDrive")) m_pudu.setPooDriveDb(float(v));
        else if (is("pudu.pooTune")) m_pudu.setPooTuneHz(float(v));
        else if (is("pudu.pooMix")) m_pudu.setPooMix(float(v));
        else if (is("pudu.dooTune")) m_pudu.setDooTuneHz(float(v));
        else if (is("pudu.dooHarmonics")) m_pudu.setDooHarmonicsDb(float(v));
        else if (is("pudu.dooMix")) m_pudu.setDooMix(float(v));
        else return setBand(key, v);
        return true;
    }

    // v0.1.1's RxChain::process, transmit bypass aside.
    QByteArray process(const QByteArray& in)
    {
        QByteArray out;
        if (m_nr) {
            m_nr->process(in, out);
        } else {
            out = in;
        }
        const int frames = int(out.size() / (2 * sizeof(float)));
        if (frames <= 0) return out;
        auto* samples = reinterpret_cast<float*>(out.data());
        for (const QString& stage : std::as_const(m_order)) {
            if (stage == QLatin1String("gate")) {
                if (m_gate.isEnabled()) m_gate.process(samples, frames, 2);
            } else if (stage == QLatin1String("eq")) {
                if (m_eq.isEnabled()) m_eq.process(samples, frames, 2);
            } else if (stage == QLatin1String("comp")) {
                if (m_comp.isEnabled()) m_comp.process(samples, frames, 2);
            } else if (stage == QLatin1String("tube")) {
                if (m_tube.isEnabled()) m_tube.process(samples, frames, 2);
            } else if (stage == QLatin1String("pudu")) {
                if (m_pudu.isEnabled()) m_pudu.process(samples, frames, 2);
            }
        }
        return out;
    }

    // Every stage key v0.1.1's table had, in its order.
    static QStringList keys()
    {
        QStringList list = {
            QStringLiteral("gate.enabled"), QStringLiteral("gate.mode"), QStringLiteral("gate.threshold"),
            QStringLiteral("gate.ratio"), QStringLiteral("gate.attack"), QStringLiteral("gate.release"),
            QStringLiteral("gate.hold"), QStringLiteral("gate.floor"), QStringLiteral("gate.return"),
            QStringLiteral("gate.lookahead"), QStringLiteral("eq.enabled"), QStringLiteral("eq.master"),
            QStringLiteral("eq.family"),
        };
        for (int band = 0; band < AetherSDR::ClientEq::kDefaultBandCount; ++band) {
            for (const char* leaf : {"on", "freq", "gain", "q"}) {
                list.append(QStringLiteral("eq.b%1.%2").arg(band).arg(QLatin1String(leaf)));
            }
        }
        list += {
            QStringLiteral("comp.enabled"), QStringLiteral("comp.threshold"), QStringLiteral("comp.ratio"),
            QStringLiteral("comp.attack"), QStringLiteral("comp.release"), QStringLiteral("comp.knee"),
            QStringLiteral("comp.makeup"), QStringLiteral("comp.drive"), QStringLiteral("comp.rotator"),
            QStringLiteral("comp.limiter"), QStringLiteral("comp.ceiling"), QStringLiteral("tube.enabled"),
            QStringLiteral("tube.model"), QStringLiteral("tube.drive"), QStringLiteral("tube.bias"),
            QStringLiteral("tube.tone"), QStringLiteral("tube.output"), QStringLiteral("tube.mix"),
            QStringLiteral("tube.envelope"), QStringLiteral("tube.attack"), QStringLiteral("tube.release"),
            QStringLiteral("pudu.enabled"), QStringLiteral("pudu.mode"), QStringLiteral("pudu.pooDrive"),
            QStringLiteral("pudu.pooTune"), QStringLiteral("pudu.pooMix"), QStringLiteral("pudu.dooTune"),
            QStringLiteral("pudu.dooHarmonics"), QStringLiteral("pudu.dooMix"),
        };
        return list;
    }

    // A profile as a v0.1.1 user might have saved it: every stage on, most values off
    // their defaults, including ones whose precision this build raised (2.55, 30.2).
    static QHash<QString, double> v011Settings()
    {
        return {
            {QStringLiteral("gate.enabled"), 1}, {QStringLiteral("gate.mode"), 1},
            {QStringLiteral("gate.threshold"), -45.5}, {QStringLiteral("gate.ratio"), 3.7},
            {QStringLiteral("gate.attack"), 2.5}, {QStringLiteral("gate.release"), 30.2},
            {QStringLiteral("gate.hold"), 20.5}, {QStringLiteral("gate.floor"), -20.8},
            {QStringLiteral("gate.return"), 2.35}, {QStringLiteral("gate.lookahead"), 1.5},
            {QStringLiteral("eq.enabled"), 1}, {QStringLiteral("eq.master"), -3.5},
            {QStringLiteral("eq.family"), 1},
            {QStringLiteral("eq.b0.on"), 1}, {QStringLiteral("eq.b0.freq"), 120}, {QStringLiteral("eq.b0.q"), 0.9},
            {QStringLiteral("eq.b1.on"), 1}, {QStringLiteral("eq.b1.gain"), 4.5},
            {QStringLiteral("eq.b4.on"), 1}, {QStringLiteral("eq.b4.freq"), 850}, {QStringLiteral("eq.b4.gain"), 6},
            {QStringLiteral("eq.b4.q"), 1.4}, {QStringLiteral("eq.b8.on"), 1}, {QStringLiteral("eq.b8.gain"), -3},
            {QStringLiteral("eq.b9.on"), 1}, {QStringLiteral("eq.b9.freq"), 6000},
            {QStringLiteral("comp.enabled"), 1}, {QStringLiteral("comp.threshold"), -35.6},
            {QStringLiteral("comp.ratio"), 2.55}, {QStringLiteral("comp.attack"), 12.5},
            {QStringLiteral("comp.release"), 124}, {QStringLiteral("comp.knee"), 4.5},
            {QStringLiteral("comp.makeup"), 2}, {QStringLiteral("comp.drive"), 3},
            {QStringLiteral("comp.rotator"), 2}, {QStringLiteral("comp.limiter"), 1},
            {QStringLiteral("comp.ceiling"), -2},
            {QStringLiteral("tube.enabled"), 1}, {QStringLiteral("tube.model"), 1},
            {QStringLiteral("tube.drive"), 7.92}, {QStringLiteral("tube.bias"), 0.11},
            {QStringLiteral("tube.tone"), 0.2}, {QStringLiteral("tube.output"), 3.25},
            {QStringLiteral("tube.mix"), 0.8}, {QStringLiteral("tube.envelope"), 0.3},
            {QStringLiteral("tube.attack"), 3}, {QStringLiteral("tube.release"), 35.5},
            {QStringLiteral("pudu.enabled"), 1}, {QStringLiteral("pudu.mode"), 1},
            {QStringLiteral("pudu.pooDrive"), 4}, {QStringLiteral("pudu.pooTune"), 90},
            {QStringLiteral("pudu.pooMix"), 0.4}, {QStringLiteral("pudu.dooTune"), 4500},
            {QStringLiteral("pudu.dooHarmonics"), 8}, {QStringLiteral("pudu.dooMix"), 0.6},
        };
    }

private:
    bool setBand(const QString& key, double v)
    {
        if (!key.startsWith(QLatin1String("eq.b"))) return false;
        bool ok = false;
        const int band = key.section(QLatin1Char('.'), 1, 1).mid(1).toInt(&ok);
        if (!ok || band < 0 || band >= AetherSDR::ClientEq::kDefaultBandCount) return false;
        const QString leaf = key.section(QLatin1Char('.'), 2);
        AetherSDR::ClientEq::BandParams p = m_eq.band(band);
        if (leaf == QLatin1String("on")) p.enabled = v != 0.0;
        else if (leaf == QLatin1String("freq")) p.freqHz = float(v);
        else if (leaf == QLatin1String("gain")) p.gainDb = float(v);
        else if (leaf == QLatin1String("q")) p.q = float(v);
        else return false;
        m_eq.setBand(band, p);
        return true;
    }

    AetherSDR::ClientGate m_gate;
    AetherSDR::ClientEq m_eq;
    AetherSDR::ClientComp m_comp;
    AetherSDR::ClientTube m_tube;
    AetherSDR::ClientPudu m_pudu;
    std::shared_ptr<esa::NrProcessor> m_nr;
    QStringList m_order = {QStringLiteral("gate"), QStringLiteral("eq"), QStringLiteral("comp"),
                           QStringLiteral("tube"), QStringLiteral("pudu")};
};

} // namespace reference
