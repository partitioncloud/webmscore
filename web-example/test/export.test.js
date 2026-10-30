import { describe, it, before } from 'node:test'
import assert from 'node:assert/strict'
import zlib from 'node:zlib'
import { AEQUALE, loadScore, hasMagic } from './helpers.js'

const ZIP = 'PK\x03\x04'

/** Pages of a PDF file */
function pdfPageCount(data) {
    return (Buffer.from(data).toString('latin1').match(/\/Type\s*\/Page(?![s\w])/g) || []).length
}

/** Width, height, color type and decoded RGBA pixels of a PNG file (8 bit, non interlaced) */
function parsePng(data) {
    const buf = Buffer.from(data)
    const width = buf.readUInt32BE(16), height = buf.readUInt32BE(20)
    const bitDepth = buf[24], colorType = buf[25]
    const idat = []
    for (let off = 8; off < buf.length;) {
        const len = buf.readUInt32BE(off)
        if (buf.toString('latin1', off + 4, off + 8) === 'IDAT') idat.push(buf.subarray(off + 8, off + 8 + len))
        off += 12 + len
    }
    return { width, height, bitDepth, colorType, raw: zlib.inflateSync(Buffer.concat(idat)) }
}

describe('export', () => {
    let score, npages
    before(async () => {
        score = await loadScore(AEQUALE)
        npages = await score.npages()
    })

    it('MusicXML has every part and measure', async () => {
        const xml = await score.saveXml()
        assert.match(xml, /^<\?xml/)
        assert.match(xml, /<score-partwise/)
        assert.equal(xml.match(/<score-part /g).length, 3)
        assert.equal(xml.match(/<measure /g).length, 3 * 34)
        assert.match(xml, /<work-title>Aequale No. 1<\/work-title>/)
    })

    it('MXL and MSCZ are zip files', async () => {
        assert.ok(hasMagic(await score.saveMxl(), ZIP))
        assert.ok(hasMagic(await score.saveMsc('mscz'), ZIP))
    })

    it('MSCX is a museScore document', async () => {
        const mscx = Buffer.from(await score.saveMsc('mscx')).toString('utf8')
        assert.match(mscx, /<museScore version="4\.\d+"/)
    })

    it('SVG: one document per page, the background is optional', async () => {
        for (let page = 0; page < npages; page++) {
            const svg = await score.saveSvg(page, false)
            assert.match(svg, /<svg[\s\S]*<\/svg>\s*$/, `page ${page}`)
            assert.match(svg, /viewBox="0 0 [\d.]+ [\d.]+"/)
        }
        const last = npages - 1
        const plain = await score.saveSvg(last, false)
        const withBackground = await score.saveSvg(last, true)
        assert.ok(withBackground.length > plain.length)
        // the music is on the last two pages, the first ones are front matter
        assert.ok((plain.match(/class="Note"/g) || []).length > 0)
    })

    it('PNG: valid image, transparent unless asked otherwise', async () => {
        const transparent = parsePng(await score.savePng(npages - 1, false, true))
        const opaque = parsePng(await score.savePng(npages - 1, true, false))
        for (const png of [transparent, opaque]) {
            assert.ok(png.width > 0 && png.height > png.width, 'A4 portrait')
        }
        // RGBA, 8 bit: the first pixel of the first row (after the filter byte) is the page corner
        assert.equal(transparent.colorType, 6)
        assert.equal(transparent.raw[1 + 3], 0, 'corner pixel is transparent')
        if (opaque.colorType === 6) {
            assert.equal(opaque.raw[1 + 3], 255, 'corner pixel is opaque')
        }
    })

    it('PDF has one page per score page', async () => {
        const pdf = await score.savePdf()
        assert.ok(hasMagic(pdf, '%PDF-'))
        assert.equal(pdfPageCount(pdf), npages)
    })

    it('MIDI is a standard MIDI file', async () => {
        const midi = await score.saveMidi()
        assert.ok(hasMagic(midi, 'MThd'))
        const buf = Buffer.from(midi)
        assert.equal(buf.readUInt16BE(8), 1, 'format 1')
        assert.ok(buf.readUInt16BE(10) >= 3, 'one track per part at least')
    })

    it('MIDI expands repeats only when asked', async () => {
        const repeats = await loadScore('repeat01.mscx')
        const expanded = await repeats.saveMidi(true)
        const unexpanded = await repeats.saveMidi(false)
        assert.ok(expanded.length > unexpanded.length)
    })
})
