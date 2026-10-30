import { describe, it, before } from 'node:test'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { loadScore, setSoundFont, hasMagic, parseWav, rms } from './helpers.js'

const SAVE_AUDIO = path.join(path.dirname(fileURLToPath(import.meta.url)), 'support/save-audio.js')
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'webmscore-audio-'))

// a short score: rendering is about 2.5x real time
const SCORE = 'repeat01.mscx'

/** saveAudio in a fresh wasm instance */
function saveAudioIsolated(format, score = SCORE) {
    const out = path.join(tmp, `${path.basename(score)}.${format}`)
    execFileSync(process.execPath, [SAVE_AUDIO, score, format, out], { stdio: ['ignore', 'ignore', 'inherit'] })
    return fs.readFileSync(out)
}

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

    it('wav: the whole score, not silent', () => {
        const wav = parseWav(saveAudioIsolated('wav'))
        assert.equal(wav.channels, 2)
        assert.equal(wav.sampleRate, 44100)
        assert.ok(wav.duration >= duration && wav.duration < duration + 10, `${wav.duration}s vs ${duration}s`)
        assert.ok(rms(wav.samples) > 0.01, 'not silent')
    })

    for (const format of ['ogg', 'flac', 'mp3']) {
        it(`${format}: valid file of a plausible size`, () => {
            const data = saveAudioIsolated(format)
            assert.ok(MAGIC[format](data), 'magic bytes')
            // > 8 kbit/s: a silent or truncated file is a few KB at most
            assert.ok(data.length > duration * 1000, `${data.length} bytes`)
        })
    }

    it('rejects an unknown format', async () => {
        await setSoundFont()
        await assert.rejects(score.saveAudio('aiff'))
    })

    // Known bug: once the audio engine has rendered anything in a wasm instance, later exports
    // are empty (wav: 0 bytes, ogg/flac/mp3: ~1 KB of silence) or fail with
    // Err[349] NoAudioToExport, even for a newly loaded score.
    const todo = 'saveAudio only works once per wasm instance'

    it('can export the same score twice', { todo }, async () => {
        await setSoundFont()
        const first = await score.saveAudio('ogg')
        const second = await score.saveAudio('ogg')
        assert.ok(second.length > first.length / 2, `${first.length} then ${second.length} bytes`)
    })

    it('can export after synthAudio', { todo }, async () => {
        const fn = await score.synthAudio(0)
        await fn()
        await fn(true)
        const data = await (await loadScore(SCORE)).saveAudio('ogg')
        assert.ok(data.length > duration * 1000, `${data.length} bytes`)
    })
})
