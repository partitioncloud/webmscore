import { describe, it } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { WebMscore, AEQUALE, loadScore, fixturePath, mscVersion } from './helpers.js'

describe('runtime', () => {
    it('supports the file format version of src/engraving (MSC_VERSION)', async () => {
        assert.equal(await WebMscore.version(), mscVersion())
    })
})

describe('load', () => {
    it('loads a native mscz score', async () => {
        const score = await loadScore(AEQUALE)
        assert.equal(await score.title(), 'Aequale No. 1')
        assert.equal(await score.npages(), 4)
        assert.equal((await score.metadata()).measures, 34)
        score.destroy()
    })

    it('loads an uncompressed mscx score', async () => {
        const score = await loadScore('repeat01.mscx')
        assert.ok(await score.npages() >= 1)
        assert.equal((await score.metadata()).measures, 6)
    })

    it('imports a Guitar Pro score', async () => {
        const score = await loadScore('heavy-accent.gp5')
        assert.ok(await score.npages() >= 1)
        assert.ok((await score.metadata()).measures >= 1)
    })

    it('imports MusicXML, MXL and MIDI produced by its own exports', async () => {
        const score = await loadScore(AEQUALE)
        const xml = await WebMscore.load('musicxml', new TextEncoder().encode(await score.saveXml()))
        const mxl = await WebMscore.load('mxl', await score.saveMxl())
        const midi = await WebMscore.load('midi', await score.saveMidi())
        for (const imported of [xml, mxl, midi]) {
            const md = await imported.metadata()
            assert.equal(md.measures, 34)
            assert.equal(md.parts.length, 3)
        }
        assert.equal(await xml.title(), 'Aequale No. 1')
    })

    it('loads without layout (metadata and MIDI only)', async () => {
        const score = await loadScore(AEQUALE, { doLayout: false })
        assert.equal((await score.metadata()).measures, 34)
        assert.ok((await score.saveMidi()).length > 0)
    })

    it('rejects corrupt data, and keeps working afterwards', async () => {
        await assert.rejects(
            WebMscore.load('mscz', fs.readFileSync(fixturePath('corrupt.mscz'))),
            { name: 'WasmError' },
        )
        const score = await loadScore(AEQUALE)
        assert.equal(await score.title(), 'Aequale No. 1')
    })

    it('can destroy a score and load another one', async () => {
        for (let i = 0; i < 3; i++) {
            const score = await loadScore(AEQUALE)
            assert.equal(await score.npages(), 4)
            score.destroy()
        }
    })
})

describe('roundtrip', () => {
    for (const format of ['mscz', 'mscx']) {
        it(`saveMsc('${format}') reloads into the same score`, async () => {
            const score = await loadScore(AEQUALE)
            const saved = await WebMscore.load(format, await score.saveMsc(format))
            const [a, b] = [await score.metadata(), await saved.metadata()]
            for (const key of ['title', 'composer', 'measures', 'pages', 'duration', 'keysig', 'timesig']) {
                assert.deepEqual(b[key], a[key], key)
            }
            assert.deepEqual(b.parts.map(p => p.instrumentId), a.parts.map(p => p.instrumentId))
            assert.equal(b.fileVersion, mscVersion())
        })
    }
})
