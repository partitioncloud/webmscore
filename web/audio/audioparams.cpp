#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <type_traits>

#include "audio/engine/internal/engineplayback.h"
#include "context/iglobalcontext.h"
#include "notation/imasternotation.h"
#include "notation/inotationsolomutestate.h"
#include "playback/iplaybackcontroller.h"
#include "project/inotationproject.h"

#include "./audioparams.h"
#include "./audiosynth.h"

using namespace muse;


static QJsonObject toQJsonObject(const audio::AudioOutputParams& params) {
    QJsonObject obj;
    obj.insert(QStringLiteral("volume"), params.volume.raw());
    obj.insert(QStringLiteral("balance"), params.balance);
    obj.insert(QStringLiteral("solo"), params.solo);
    obj.insert(QStringLiteral("muted"), params.muted);
    obj.insert(QStringLiteral("forceMute"), params.forceMute);
    return obj;
}

static Ret patchOutputParamsFromJson(const QJsonObject& patch, audio::AudioOutputParams& params) {
    const auto applyDouble = [&patch](QStringView key, auto& target) {
        if (!patch.contains(key))
            return make_ok();

        const QJsonValue value = patch.value(key);
        if (!value.isDouble()) {
            return make_ret(Ret::Code::UnknownError, "Invalid type for '" + key.toString().toStdString() + "': number expected");
        }
        target = static_cast<float>(value.toDouble());
        return make_ok();
    };

    const auto applyBool = [&patch](QStringView key, bool& target) {
        if (!patch.contains(key))
            return make_ok();

        const QJsonValue value = patch.value(key);
        if (!value.isBool()) {
            return make_ret(Ret::Code::UnknownError, "Invalid type for '" + key.toString().toStdString() + "': boolean expected");
        }
        target = value.toBool();
        return make_ok();
    };

    for (const Ret& ret : {
        applyDouble(QStringLiteral("volume"), params.volume),
        applyDouble(QStringLiteral("balance"), params.balance),
        applyBool(QStringLiteral("solo"), params.solo),
        applyBool(QStringLiteral("muted"), params.muted),
        applyBool(QStringLiteral("forceMute"), params.forceMute),
    }) {
        if (!ret) {
            return ret;
        }
    }

    return make_ok();
}

namespace MainAudio {

// `score` is the current score, see `makeCurrent()` in main.cpp
Ret getOutputParams(MainScore score) {
    (void)score;

    const auto enginePlayback = modularity::ioc()->resolve<audio::engine::IEnginePlayback>("");

    const audio::TrackSequenceId sequenceId = Synth::getSequenceId();

    QJsonObject root;
    const auto masterParams = enginePlayback->masterOutputParams();
    if (!masterParams.ret) {
        return masterParams.ret;
    }
    root.insert(QStringLiteral("master"), toQJsonObject(masterParams.val));

    const auto trackIds = enginePlayback->trackIdList(sequenceId);
    if (!trackIds.ret) {
        return trackIds.ret;
    }
    
    QJsonArray tracks;
    for (const auto trackId : trackIds.val) {
        const auto trackName = QString::fromStdString(enginePlayback->trackName(sequenceId, trackId).val);
        const auto trackParams = enginePlayback->outputParams(sequenceId, trackId);
        if (!trackParams.ret) {
            return trackParams.ret;
        }

        QJsonObject trackObj = toQJsonObject(trackParams.val);
        trackObj.insert(QStringLiteral("trackId"), trackId);
        trackObj.insert(QStringLiteral("trackName"), trackName);
        tracks.append(trackObj);
    }
    root.insert(QStringLiteral("tracks"), tracks);

    Ret ret = make_ok();
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    ret.setData("wasm_payload", ByteArray(payload.constData(), payload.size()));
    return ret;
}

static mu::project::IProjectAudioSettingsPtr currentAudioSettings() {
    const auto globalContext = modularity::ioc()->resolve<mu::context::IGlobalContext>("");
    const auto project = globalContext ? globalContext->currentProject() : nullptr;
    return project ? project->audioSettings() : nullptr;
}

/**
 * Apply patched params to a track the way the playback controller expects, otherwise the
 * controller overrides them (`updateSoloMuteStates()`, possibly with rpc messages still queued):
 * - volume/balance are stored in the project's audio settings, which the controller reads (also
 *   to rebuild the sequence when the current score changes);
 * - solo/mute of instrument and aux tracks are solo/mute *states*, from which the controller
 *   computes `muted`/`forceMute` of every track (a solo force-mutes the others).
 *   Only the keys present in the patch change them: a read `muted` includes `forceMute`.
 * The metronome is not managed by the controller: it is only set in the engine.
 */
static void applyTrackOutputParams(audio::TrackSequenceId sequenceId, audio::TrackId trackId,
                                   const audio::AudioOutputParams& params, const QJsonObject& patch) {
    using SoloMuteState = mu::notation::INotationSoloMuteState::SoloMuteState;

    const auto enginePlayback = modularity::ioc()->resolve<audio::engine::IEnginePlayback>("");
    const auto playbackController = modularity::ioc()->resolve<mu::playback::IPlaybackController>("");
    const auto globalContext = modularity::ioc()->resolve<mu::context::IGlobalContext>("");
    const auto audioSettings = currentAudioSettings();
    const auto masterNotation = globalContext ? globalContext->currentMasterNotation() : nullptr;

    // applied right away; the controller pushes the same values later on
    enginePlayback->setOutputParams(sequenceId, trackId, params);
    if (!audioSettings || !playbackController || !masterNotation) {
        return;
    }

    const auto patchedState = [&patch](SoloMuteState state) {
        if (patch.contains(QStringLiteral("muted"))) {
            state.mute = patch.value(QStringLiteral("muted")).toBool();
        }
        if (patch.contains(QStringLiteral("solo"))) {
            state.solo = patch.value(QStringLiteral("solo")).toBool();
        }
        return state;
    };

    for (const auto& [instrumentTrackId, id] : playbackController->instrumentTrackIdMap()) {
        if (id != trackId) {
            continue;
        }
        if (instrumentTrackId == masterNotation->playback()->metronomeTrackId()) {
            return;
        }

        audio::AudioOutputParams stored = audioSettings->trackOutputParams(instrumentTrackId);
        stored.volume = params.volume;
        stored.balance = params.balance;
        audioSettings->setTrackOutputParams(instrumentTrackId, stored);

        // triggers `updateSoloMuteStates()`
        playbackController->setTrackSoloMuteState(instrumentTrackId,
                                                  patchedState(playbackController->trackSoloMuteState(instrumentTrackId)));
        return;
    }

    for (const auto& [auxIdx, id] : playbackController->auxTrackIdMap()) {
        if (id != trackId) {
            continue;
        }

        audio::AudioOutputParams stored = audioSettings->auxOutputParams(auxIdx);
        stored.volume = params.volume;
        stored.balance = params.balance;
        audioSettings->setAuxOutputParams(auxIdx, stored);

        // triggers `updateSoloMuteStates()`
        audioSettings->setAuxSoloMuteState(auxIdx, patchedState(audioSettings->auxSoloMuteState(auxIdx)));
        return;
    }
}

// `score` is the current score, see `makeCurrent()` in main.cpp
Ret setOutputParams(MainScore score, const char* json) {
    (void)score;

    const auto enginePlayback = modularity::ioc()->resolve<audio::engine::IEnginePlayback>("");

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return make_ret(Ret::Code::UnknownError, std::string("Invalid JSON payload"));
    }

    const QJsonObject payload = doc.object();

    const audio::TrackSequenceId sequenceId = Synth::getSequenceId();
    if (sequenceId == -1) {
        return make_ret(Ret::Code::UnknownError, std::string("No playback sequence found"));
    }

    if (payload.contains(QStringLiteral("master"))) {
        const QJsonValue masterValue = payload.value(QStringLiteral("master"));
        if (!masterValue.isObject()) {
            return make_ret(Ret::Code::UnknownError, std::string("Invalid type for 'master': object expected"));
        }

        const auto masterParamsRet = enginePlayback->masterOutputParams();
        if (!masterParamsRet.ret) {
            return masterParamsRet.ret;
        }

        auto masterParams = masterParamsRet.val;
        Ret ret = patchOutputParamsFromJson(masterValue.toObject(), masterParams);
        if (!ret) {
            return ret;
        }
        enginePlayback->setMasterOutputParams(masterParams);
        if (const auto audioSettings = currentAudioSettings()) {
            audioSettings->setMasterAudioOutputParams(masterParams);
        }
    }

    if (payload.contains(QStringLiteral("tracks"))) {
        const QJsonValue tracksValue = payload.value(QStringLiteral("tracks"));
        if (!tracksValue.isArray()) {
            return make_ret(Ret::Code::UnknownError, std::string("Invalid type for 'tracks': array expected"));
        }

        const QJsonArray tracksArray = tracksValue.toArray();
        for (const QJsonValue& trackValue : tracksArray) {
            if (!trackValue.isObject()) {
                return make_ret(Ret::Code::UnknownError, std::string("Each track patch must be an object"));
            }

            const QJsonObject trackPatch = trackValue.toObject();
            const QJsonValue trackIdValue = trackPatch.value(QStringLiteral("trackId"));
            if (!trackIdValue.isDouble()) {
                return make_ret(Ret::Code::UnknownError, std::string("Track patch requires numeric 'trackId'"));
            }

            const audio::TrackId trackId = static_cast<audio::TrackId>(trackIdValue.toInt(-1));
            const auto trackParamsRet = enginePlayback->outputParams(sequenceId, trackId);
            if (!trackParamsRet.ret) {
                return trackParamsRet.ret;
            }

            auto params = trackParamsRet.val;
            Ret ret = patchOutputParamsFromJson(trackPatch, params);
            if (!ret) {
                return ret;
            }

            applyTrackOutputParams(sequenceId, trackId, params, trackPatch);
        }

        // let the controller's updates reach the engine, so that the params read back are the new ones
        Synth::pump(16);
    }

    return make_ok();
}

} // namespace MainAudio