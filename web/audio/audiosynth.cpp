#include "global/log.h"
#include "global/containers.h"
#include "async/processevents.h"

#include "modularity/ioc.h"

#include "audio/common/rpc/irpcchannel.h"
#include "audio/engine/internal/audioengine.h"
#include "audio/engine/internal/engineplayback.h"

#include "notation/inotation.h"

#include "playback/iplaybackcontroller.h"

#include "./audiosynth.h"

using namespace muse;

namespace MainAudio {

std::vector<std::function<Synth::SynthRes*(bool)>> Synth::synthIterators;

/**
 * De-interleave audio channels
 * @param dest: [ channelA #len frames, channelB #len frames ]
 * @param src:  [ channelA frame0, channelB frame0, channelA frame1, channelB frame1, ... ]
 */
void deInterleave(float* dest, const float* src, size_t framesLen) {
    for (size_t i = 0, j = 0; i < framesLen; i++, j+=2) {
        dest[i] = src[j];
        dest[framesLen + i] = src[j+1];
    }
}

const char* Synth::processBatch(int batchSize, bool cancel) {
    auto resArr = (SynthRes**)calloc(batchSize, sizeof(SynthRes*)); // array of pointers to SynthRes data 
    for (int i = 0; i < batchSize; i++) {
        resArr[i] = (*synthFn)(cancel);
    }
    return reinterpret_cast<const char*>(resArr);
}

audio::TrackSequenceId Synth::getSequenceId() {
    auto playbackController = modularity::ioc()->resolve<playback::IPlaybackController>("");
    auto rpcChannel = modularity::ioc()->resolve<audio::rpc::IRpcChannel>("");
    auto enginePlayback = modularity::ioc()->resolve<audio::engine::IEnginePlayback>("");

    // NOTE The engine runs in-process, but it is still driven by the rpc queue and by
    // promises, so both need to be pumped until the engine caught up
    auto pump = [rpcChannel]() {
        async::processMessages();
        rpcChannel->process();
    };

    static const int MAX_PUMPS = 64;

    // The sequence of the current notation is created asynchronously (`Playback::addSequence()`
    // is a `Promise`), and so are its tracks: the controller allows playing once all are loaded
    audio::TrackSequenceId sequenceId = -1;
    for (int i = 0; i < MAX_PUMPS * 8; i++) {
        sequenceId = playbackController->currentTrackSequenceId();
        if (sequenceId != -1 && playbackController->isPlayAllowed()
            && muse::contains(enginePlayback->sequenceIdList(), sequenceId)) {
            break;
        }

        pump();
    }

    if (sequenceId != -1 && !muse::contains(enginePlayback->sequenceIdList(), sequenceId)) {
        sequenceId = -1;
    }

    if (sequenceId == -1) {
        LOGE() << "No playback sequence found";
        return -1;
    }

    // The tracks of the sequence are added asynchronously as well (one rpc round trip each),
    // wait until no more tracks are added
    size_t trackCount = 0;
    int stableCount = 0;
    for (int i = 0; i < MAX_PUMPS * 8 && stableCount < 8; i++) {
        pump();

        const size_t count = enginePlayback->trackIdList(sequenceId).val.size();
        if (count != 0 && count == trackCount) {
            stableCount++;
        } else {
            stableCount = 0;
            trackCount = count;
        }
    }

    return sequenceId;
}

Synth Synth::start(MainScore score, float starttime) {
    LOGI() << String(u"starttime %1").arg(starttime);

    // use buffer size of 512 frames
    static const size_t renderStep = 512;
    static const size_t channels = 2;
    static const size_t sampleRate = 44100;

    auto engine_playback = std::static_pointer_cast<audio::engine::EnginePlayback>(
        modularity::ioc()->resolve<audio::engine::IEnginePlayback>(""));
    auto audio_engine = modularity::ioc()->resolve<audio::engine::IAudioEngine>("");

    const auto sequenceId = getSequenceId();
    auto sequence = engine_playback->sequence(sequenceId);

    // Seek
    // https://github.com/LibreScore/webmscore/blob/v4.0/src/framework/audio/internal/worker/audiooutputhandler.cpp#L200-L201
    sequence->player()->stop();
    sequence->player()->seek_ms(starttime * 1000 * 1000); // get microsecs

    // Setup audio source
    // https://github.com/LibreScore/webmscore/blob/v4.0/src/framework/audio/internal/soundtracks/soundtrackwriter.cpp#L73-L76
    audio_engine->setMode(audio::RenderMode::OfflineMode);
    auto source = audio_engine->mixer();

    audio::OutputSpec spec;
    spec.sampleRate = sampleRate;
    spec.samplesPerChannel = renderStep;
    spec.audioChannelCount = channels;
    source->setOutputSpec(spec);
    source->setIsActive(true);

    // https://github.com/LibreScore/webmscore/blob/v4.0/src/framework/audio/internal/soundtracks/soundtrackwriter.cpp#L49
    const auto totalDuration = sequence->player()->duration();
    const audio::samples_t totalSamples = (totalDuration / 1000000.f) * sampleRate;
    LOGI() << String(u"totalDuration %1, totalSamples %2").arg(totalDuration).arg((int64_t)totalSamples);

    bool done = false;
    audio::samples_t playedSamples = starttime * sampleRate;
    auto synthIterator = [done, playedSamples, totalSamples, source](bool cancel = false) mutable -> SynthRes* { // must use by-copy capture because variables are destroyed as the `_synthAudio` function ends
        if (done) {
            return new SynthRes{done, -1, -1, 0, {}};
        }

        float buffer[renderStep * channels] = {};
        auto res = (SynthRes*)calloc(1, sizeof(SynthRes) + sizeof(buffer)); 
        res->chunkSize = sizeof(buffer);

        // render audio buffer
        source->process(buffer, renderStep);
        deInterleave((float*)res->chunk, buffer, renderStep);

        auto prevPlayed = playedSamples;
        playedSamples += renderStep;
        if (playedSamples >= totalSamples || cancel) {
            // finished, do cleanup
            source->setIsActive(false);
            done = true;
        }

        res->done = done;
        res->startTime = float(prevPlayed) / sampleRate;
        res->endTime = float(playedSamples) / sampleRate;

        return res;
    };

    // persist this `synthIterator` function
    synthIterators.push_back(synthIterator);

    return Synth(&synthIterators.back());
}

} // namespace MainAudio
