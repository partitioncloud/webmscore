import { describe, it, before } from 'node:test'
import assert from 'node:assert/strict'
import { loadScore, setSoundFont, hasMagic, parseWav, rms } from './helpers.js'

// a short score: rendering is about 2.5x real time
const SCORE = 'repeat01.mscx'

const MAGIC = {
    wav: (d) => hasMagic(d, 'RIFF') && hasMagic(d, 'WAVE', 8),
    ogg: (d) => hasMagic(d, 'OggS'),
    flac: (d) => hasMagic(d, 'fLaC'),
    mp3: (d) => hasMagic(d, 'ID3') || (d[0] === 0xff && (d[1] & 0xe0) === 0xe0),
}

describe('saveAudio', () => {
    let score, duration
    before(async () => {
        score = await loadScore(SCORE)
        duration = (await score.metadata()).duration
    })

    it('throws without a soundfont', async () => {
        await assert.rejects(score.saveAudio('ogg'), /soundfont/)
    })

    it('wav: the whole score, not silent', async () => {
        await setSoundFont()
        const wav = parseWav(await score.saveAudio('wav'))
        assert.equal(wav.channels, 2)
        assert.equal(wav.sampleRate, 44100)
        assert.ok(wav.duration >= duration && wav.duration < duration + 10, `${wav.duration}s vs ${duration}s`)
        assert.ok(rms(wav.samples) > 0.01, 'not silent')
    })

    // each format is a new export of the same score (used to work only once per instance)
    for (const format of ['ogg', 'flac', 'mp3']) {
        it(`${format}: valid file of a plausible size`, async () => {
            const data = await score.saveAudio(format)
            assert.ok(MAGIC[format](data), 'magic bytes')
            // > 8 kbit/s: a silent or truncated file is a few KB at most
            assert.ok(data.length > duration * 1000, `${data.length} bytes`)
        })
    }

    it('rejects an unknown format', async () => {
        await assert.rejects(score.saveAudio('aiff'))
    })

    it('exports the same audio twice', async () => {
        const first = parseWav(await score.saveAudio('wav'))
        const second = parseWav(await score.saveAudio('wav'))
        assert.equal(second.samples.length, first.samples.length)
        assert.ok(Math.abs(rms(second.samples) - rms(first.samples)) < rms(first.samples) * 0.05)
    })

    it('exports after synthAudio', async () => {
        const fn = await score.synthAudio(0)
        await fn()
        await fn(true)
        const wav = parseWav(await score.saveAudio('wav'))
        assert.ok(wav.duration >= duration, `${wav.duration}s vs ${duration}s`)
    })

    it('exports its own score, not the last loaded one', async () => {
        const other = await loadScore('Aequale_No_1.mscz')
        const wav = parseWav(await score.saveAudio('wav'))
        assert.ok(wav.duration >= duration && wav.duration < duration + 10, `${wav.duration}s vs ${duration}s`)
        const { duration: otherDuration } = await other.metadata()
        assert.ok(otherDuration > duration + 10)
    })
})
