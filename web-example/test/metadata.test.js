import { describe, it, before } from 'node:test'
import assert from 'node:assert/strict'
import { AEQUALE, loadScore } from './helpers.js'

const BOOL = ['true', 'false']

describe('metadata', () => {
    let score, md
    before(async () => {
        score = await loadScore(AEQUALE)
        md = await score.metadata()
    })

    it('matches the ScoreMetadata schema (web-public/schemas.ts)', () => {
        for (const k of ['title', 'subtitle', 'composer', 'poet', 'mscoreVersion', 'previousSource', 'timesig', 'lyrics', 'tempoText']) {
            assert.equal(typeof md[k], 'string', k)
        }
        for (const k of ['fileVersion', 'pages', 'measures', 'keysig', 'duration', 'tempo']) {
            assert.equal(typeof md[k], 'number', k)
        }
        for (const k of ['hasLyrics', 'hasHarmonies']) {
            assert.ok(BOOL.includes(md[k]), k)
        }
        assert.ok(Array.isArray(md.excerpts))
        for (const part of md.parts) {
            for (const k of ['name', 'instrumentId', 'instrumentName']) assert.equal(typeof part[k], 'string', k)
            for (const k of ['program', 'lyricCount', 'harmonyCount']) assert.equal(typeof part[k], 'number', k)
            for (const k of ['hasPitchedStaff', 'hasTabStaff', 'hasDrumStaff', 'isVisible']) assert.ok(BOOL.includes(part[k]), k)
        }
        assert.equal(typeof md.pageFormat.width, 'number')
        assert.equal(typeof md.pageFormat.height, 'number')
        assert.ok(BOOL.includes(md.pageFormat.twosided))
        for (const k of ['titles', 'subtitles', 'composers', 'poets']) {
            assert.ok(Array.isArray(md.textFramesData[k]), k)
        }
    })

    it('describes the score', async () => {
        assert.equal(md.title, await score.title())
        assert.equal(md.composer, 'Anton Bruckner')
        assert.equal(md.pages, await score.npages())
        assert.equal(md.measures, 34)
        assert.equal(md.timesig, '2/2')
        assert.equal(md.keysig, -3)
        assert.equal(md.fileVersion, 302, 'version of the file as loaded, not MSC_VERSION')
        assert.deepEqual(md.parts.map(p => p.instrumentId), ['alto-trombone', 'tenor-trombone', 'bass-trombone'])
        assert.deepEqual(md.pageFormat, { height: 297, twosided: 'true', width: 210 })
        assert.deepEqual(md.textFramesData.titles, ['Aequale No. 1'])
        // 34 bars of 2/2 at half = 50, plus the fermatas
        assert.ok(md.duration >= 34 * 2 * 60 / 50 && md.duration < 34 * 2 * 60 / 50 + 30, `duration ${md.duration}`)
    })
})

describe('excerpts', () => {
    it('generates one excerpt per part, each rendered on its own', async () => {
        const score = await loadScore(AEQUALE)
        const fullXml = await score.saveXml()
        await score.generateExcerpts()
        const { excerpts } = await score.metadata()
        assert.deepEqual(excerpts.map(e => e.title), ['Alto Trombone', 'Tenor Trombone', 'Bass Trombone'])

        for (const excerpt of excerpts) {
            assert.equal(excerpt.parts.length, 1)
            await score.setExcerptId(excerpt.id)
            assert.equal(await score.getExcerptId(), excerpt.id)
            assert.ok(await score.npages() >= 1)
            const xml = await score.saveXml()
            assert.equal(xml.match(/<score-part /g).length, 1)
            assert.match(xml, new RegExp(`<part-name[^>]*>${excerpt.parts[0].name}</part-name>`))
            assert.match(await score.saveSvg(0), /<svg/)
        }

        // back to the full score
        await score.setExcerptId(-1)
        assert.equal(await score.saveXml(), fullXml)
    })
})
