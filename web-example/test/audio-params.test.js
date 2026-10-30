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

    it('solo force-mutes the other instruments', async () => {
        const { tracks } = await score.getAudioOutputParams()
        const tenor = tracks.find(t => t.trackName === 'Tenor Trombone')
        const others = tracks.filter(t => /Trombone/.test(t.trackName) && t.trackId !== tenor.trackId)
        await score.setAudioOutputParams({ tracks: [{ trackId: tenor.trackId, solo: true }] })
        try {
            const params = await score.getAudioOutputParams()
            for (const other of others) {
                const p = params.tracks.find(t => t.trackId === other.trackId)
                assert.deepEqual({ muted: p.muted, forceMute: p.forceMute }, { muted: true, forceMute: true }, other.trackName)
            }
            const { rms } = await synth(score, 5)
            assert.ok(rms < reference * 0.8 && rms > reference * 0.05, `rms ${rms} vs ${reference}`)
        } finally {
            await score.setAudioOutputParams({ tracks: [{ trackId: tenor.trackId, solo: false }] })
        }
        // lifting the solo un-mutes the others
        const params = await score.getAudioOutputParams()
        for (const other of others) {
            assert.equal(params.tracks.find(t => t.trackId === other.trackId).muted, false, other.trackName)
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

    it('round-trips a track patch', async () => {
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

    it('a change right after loading is kept', async () => {
        // the set must be the first audio call on the score: a get would first process the
        // controller's pending updates, which used to override the set (track ids are the same
        // for the same file)
        const { tracks } = await score.getAudioOutputParams()
        const tenor = tracks.find(t => t.trackName === 'Tenor Trombone')
        const fresh = await loadScore(AEQUALE)
        await fresh.setAudioOutputParams({ tracks: [{ trackId: tenor.trackId, volume: -6 }] })
        const patched = (await fresh.getAudioOutputParams()).tracks.find(t => t.trackId === tenor.trackId)
        assert.deepEqual({ trackName: patched.trackName, volume: patched.volume }, { trackName: 'Tenor Trombone', volume: -6 })
    })

    it('params are per score, and survive switching scores', async () => {
        const other = await loadScore('repeat01.mscx')
        await score.setAudioOutputParams({ master: { volume: -10 } })
        try {
            const otherParams = await other.getAudioOutputParams()
            assert.equal(otherParams.master.volume, 0)
            assert.ok(otherParams.tracks.some(t => t.trackName === 'Piano'))

            const params = await score.getAudioOutputParams()
            assert.equal(params.master.volume, -10)
            assert.ok(params.tracks.some(t => t.trackName === 'Tenor Trombone'))
            const { rms } = await synth(score, 5)
            assert.ok(approxEqual(rms / reference, dB(-10), 0.02), `ratio ${rms / reference}`)
        } finally {
            await score.setAudioOutputParams({ master: { volume: 0 } })
        }
    })
})
