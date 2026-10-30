import { describe, it, before } from 'node:test'
import assert from 'node:assert/strict'
import { AEQUALE, loadScore, setSoundFont, synth, approxEqual } from './helpers.js'

const dB = (db) => 10 ** (db / 20)

describe('audio output params', () => {
    let score, reference
    before(async () => {
        await setSoundFont()
        score = await loadScore(AEQUALE)
        reference = (await synth(score, 5)).rms
    })

    it('lists the master and one track per instrument', async () => {
        const params = await score.getAudioOutputParams()
        assert.deepEqual(params.master, { balance: 0, forceMute: false, muted: false, solo: false, volume: 0 })
        const names = params.tracks.map(t => t.trackName)
        for (const name of ['Alto Trombone', 'Tenor Trombone', 'Bass Trombone']) {
            assert.ok(names.includes(name), name)
        }
        assert.equal(new Set(params.tracks.map(t => t.trackId)).size, params.tracks.length)
    })

    it('master volume scales the output (-15 dB)', async () => {
        await score.setAudioOutputParams({ master: { volume: -15 } })
        try {
            assert.equal((await score.getAudioOutputParams()).master.volume, -15)
            const { rms } = await synth(score, 5)
            assert.ok(approxEqual(rms / reference, dB(-15), 0.02), `ratio ${rms / reference}`)
        } finally {
            await score.setAudioOutputParams({ master: { volume: 0 } })
        }
    })

    it('muting every instrument silences the output', async () => {
        const { tracks } = await score.getAudioOutputParams()
        const instruments = tracks.filter(t => !t.muted && !/^(Metronome|Aux)/.test(t.trackName))
        await score.setAudioOutputParams({ tracks: instruments.map(t => ({ trackId: t.trackId, muted: true })) })
        try {
            const { rms } = await synth(score, 5)
            assert.ok(rms < reference * 0.1, `rms ${rms} vs ${reference}`)
        } finally {
            await score.setAudioOutputParams({ tracks: instruments.map(t => ({ trackId: t.trackId, muted: false })) })
        }
    })

    it('rejects an unknown track', async () => {
        await assert.rejects(score.setAudioOutputParams({ tracks: [{ trackId: 999, volume: 3 }] }))
    })

    it('rejects a value of the wrong type', async () => {
        await assert.rejects(score.setAudioOutputParams({ master: { volume: '-6' } }), /'volume': number expected/)
        await assert.rejects(score.setAudioOutputParams({ master: { muted: 1 } }), /'muted': boolean expected/)
        assert.equal((await score.getAudioOutputParams()).master.volume, 0)
    })

    it('round-trips a track patch', { todo: 'track params read back stale values right after a set' }, async () => {
        const { tracks } = await score.getAudioOutputParams()
        const track = tracks.find(t => t.trackName === 'Tenor Trombone')
        await score.setAudioOutputParams({ tracks: [{ trackId: track.trackId, volume: -6 }] })
        await score.setAudioOutputParams({ tracks: [{ trackId: track.trackId, solo: true }] })
        try {
            const patched = (await score.getAudioOutputParams()).tracks.find(t => t.trackId === track.trackId)
            assert.deepEqual(
                { volume: patched.volume, solo: patched.solo, trackName: patched.trackName },
                { volume: -6, solo: true, trackName: track.trackName },
            )
        } finally {
            await score.setAudioOutputParams({ tracks: [{ trackId: track.trackId, volume: 0, solo: false }] })
        }
    })

    // keep last: it loads another score, which takes over the audio of `score`
    it('params are per score', { todo: 'audio params act on the last loaded score (web/audio ignores `score`)' }, async () => {
        const other = await loadScore('repeat01.mscx')
        await score.setAudioOutputParams({ master: { volume: -10 } })
        try {
            assert.equal((await other.getAudioOutputParams()).master.volume, 0)
            assert.ok((await score.getAudioOutputParams()).tracks.some(t => t.trackName === 'Tenor Trombone'))
        } finally {
            await score.setAudioOutputParams({ master: { volume: 0 } })
        }
    })
})
