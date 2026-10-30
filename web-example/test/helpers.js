import WebMscore from 'webmscore4'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

export { WebMscore }

const here = path.dirname(fileURLToPath(import.meta.url))
export const repoRoot = path.resolve(here, '../..')

/** Path of a test score: the fixtures directory, or `Aequale_No_1.mscz` shared with example.js */
export function fixturePath(name) {
    const p = path.join(here, 'fixtures', name)
    return fs.existsSync(p) ? p : path.join(here, '..', name)
}

export const AEQUALE = 'Aequale_No_1.mscz'

/** Load a test score, the format is taken from the file extension */
export async function loadScore(name = AEQUALE, { fonts = [], doLayout = true } = {}) {
    await WebMscore.ready
    const format = path.extname(name).slice(1)
    return WebMscore.load(format, fs.readFileSync(fixturePath(name)), fonts, doLayout)
}

export const SOUNDFONT = path.join(repoRoot, 'share/sound/FluidR3Mono_GM.sf3')

/** The soundfont is global to the wasm instance, set it once per process */
export async function setSoundFont() {
    // `WebMscore.setSoundFont` needs the runtime, but does not wait for it
    await WebMscore.ready
    if (!WebMscore.hasSoundfont) {
        await WebMscore.setSoundFont(fs.readFileSync(SOUNDFONT))
    }
}

/** MSC_VERSION from src/engraving/types/constants.h */
export function mscVersion() {
    const src = fs.readFileSync(path.join(repoRoot, 'src/engraving/types/constants.h'), 'utf8')
    return Number(src.match(/MSC_VERSION\s*=\s*(\d+)/)[1])
}

/** @param {Uint8Array} data @param {string} magic latin1 */
export function hasMagic(data, magic, offset = 0) {
    return Buffer.from(data.subarray(offset, offset + magic.length)).toString('latin1') === magic
}

export function approxEqual(actual, expected, eps) {
    return Math.abs(actual - expected) <= eps
}

/** Root mean square of float32 PCM samples */
export function rms(samples) {
    let sum = 0
    for (const v of samples) sum += v * v
    return samples.length ? Math.sqrt(sum / samples.length) : 0
}

/** Float32 samples of a synthAudio chunk */
export function chunkSamples(chunk) {
    return new Float32Array(chunk.buffer, chunk.byteOffset, chunk.byteLength / 4)
}

/**
 * Synthesize the first `seconds` of a score with `synthAudio`
 * @returns {Promise<{ results: import('webmscore4/schemas').SynthRes[], rms: number }>}
 */
export async function synth(score, seconds = 5) {
    const fn = await score.synthAudio(0)
    const results = []
    let sum = 0, n = 0
    let r
    do {
        r = await fn()
        results.push(r)
        for (const v of chunkSamples(r.chunk)) { sum += v * v; n++ }
    } while (!r.done && r.endTime < seconds)
    if (!r.done) await fn(true)
    return { results, rms: n ? Math.sqrt(sum / n) : 0 }
}

/** Parse a RIFF/WAVE file into its format and samples as floats (PCM 16/24/32 bit or float32) */
export function parseWav(data) {
    const buf = Buffer.from(data.buffer, data.byteOffset, data.byteLength)
    let fmt, samples
    for (let off = 12; off + 8 <= buf.length;) {
        const id = buf.toString('latin1', off, off + 4)
        const size = buf.readUInt32LE(off + 4)
        const body = off + 8
        if (id === 'fmt ') {
            fmt = {
                audioFormat: buf.readUInt16LE(body),
                channels: buf.readUInt16LE(body + 2),
                sampleRate: buf.readUInt32LE(body + 4),
                bitsPerSample: buf.readUInt16LE(body + 14),
            }
        } else if (id === 'data') {
            const bytes = fmt.bitsPerSample / 8
            const count = Math.floor(Math.min(size, buf.length - body) / bytes)
            samples = new Float32Array(count)
            for (let i = 0; i < count; i++) {
                const p = body + i * bytes
                if (fmt.audioFormat === 3) samples[i] = buf.readFloatLE(p)
                else if (bytes === 2) samples[i] = buf.readInt16LE(p) / 0x8000
                else if (bytes === 3) samples[i] = buf.readIntLE(p, 3) / 0x800000
                else samples[i] = buf.readInt32LE(p) / 0x80000000
            }
        }
        off = body + size + (size % 2)
    }
    return { ...fmt, samples, duration: samples.length / fmt.channels / fmt.sampleRate }
}
