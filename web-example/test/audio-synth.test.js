import { describe, it, before } from 'node:test'
import assert from 'node:assert/strict'
import { AEQUALE, loadScore, setSoundFont, synth, approxEqual } from './helpers.js'

const CHUNK_FRAMES = 512
const SAMPLE_RATE = 44100
const CHUNK_BYTES = CHUNK_FRAMES * 2 /* channels */ * 4 /* float32 */

describe('synthAudio', () => {
    let score
    before(async () => {
        // the soundfont is global to the wasm instance, set before loading the score
        await setSoundFont()
        score = await loadScore(AEQUALE)
    })

    it('produces contiguous chunks of 512 stereo float frames', async () => {
        const { results, rms } = await synth(score, 3)
        let previousEnd = 0
        for (const r of results) {
            assert.equal(r.chunk.length, CHUNK_BYTES)
            assert.ok(approxEqual(r.startTime, previousEnd, 1e-4), `chunk at ${r.startTime}`)
            assert.ok(approxEqual(r.endTime - r.startTime, CHUNK_FRAMES / SAMPLE_RATE, 1e-4))
            previousEnd = r.endTime
        }
        assert.ok(rms > 0.01, `not silent (rms ${rms})`)
    })

    it('runs to the end of the score', async () => {
        const { duration } = await score.metadata()
        const { results } = await synth(score, Infinity)
        const last = results.at(-1)
        assert.ok(last.done)
        assert.ok(last.endTime >= duration && last.endTime < duration + 10, `${last.endTime}s vs ${duration}s`)
    })

    it('can start at an offset', async () => {
        const fn = await score.synthAudio(30)
        const r = await fn()
        await fn(true)
        assert.ok(approxEqual(r.startTime, 30, 0.05), `starts at ${r.startTime}`)
    })

    it('restarts from the beginning when called again', async () => {
        const first = await synth(score, 2)
        const second = await synth(score, 2)
        assert.equal(second.results[0].startTime, 0)
        assert.ok(approxEqual(second.rms, first.rms, first.rms * 0.05))
    })

    it('synthAudioBatch returns batches with the same timing', async () => {
        const fn = await score.synthAudioBatch(0, 8)
        const batch = await fn()
        await fn(true)
        assert.equal(batch.length, 8)
        batch.forEach((r, i) => {
            assert.equal(r.chunk.length, CHUNK_BYTES)
            assert.ok(approxEqual(r.startTime, i * CHUNK_FRAMES / SAMPLE_RATE, 1e-4))
        })
    })

    it('handles several scores one after the other (test_synth.js scenario)', async () => {
        for (const name of [AEQUALE, 'repeat01.mscx']) {
            const other = await loadScore(name)
            const { tracks } = await other.getAudioOutputParams()
            const instrument = tracks.find(t => !/^(Metronome|Aux)/.test(t.trackName))
            await other.setAudioOutputParams({ master: { volume: -15 }, tracks: [{ trackId: instrument.trackId, volume: 15 }] })
            const { rms } = await synth(other, 2)
            assert.ok(rms > 0, name)
        }
    })

    // the last loaded score is now repeat01.mscx (~8 s)
    it('synthesizes its own score, not the last loaded one', { todo: 'web/audio ignores `score`, uses the current sequence' }, async () => {
        const { duration } = await score.metadata()
        const { results } = await synth(score, Infinity)
        assert.ok(results.at(-1).endTime >= duration, `${results.at(-1).endTime}s vs ${duration}s`)
    })

    it('works for many scores', async () => {
        // used to crash on the 4th score: a destroyed `Player` received rpc responses
        for (let i = 0; i < 4; i++) {
            const other = await loadScore(AEQUALE)
            assert.ok((await other.getAudioOutputParams()).tracks.length > 0)
            const { rms } = await synth(other, 1)
            assert.ok(rms > 0, `score ${i}`)
        }
    })
})
