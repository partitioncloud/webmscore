import { describe, it, before } from 'node:test'
import assert from 'node:assert/strict'
import { AEQUALE, loadScore, approxEqual } from './helpers.js'

/** Group consecutive elements on the same line (page + y) */
function systems(elements) {
    const groups = []
    for (const el of elements) {
        const last = groups.at(-1)
        if (last && last[0].page === el.page && last[0].y === el.y) last.push(el)
        else groups.push([el])
    }
    return groups
}

describe('positions', () => {
    let score, npages, metadata, measures, segments
    before(async () => {
        score = await loadScore(AEQUALE)
        npages = await score.npages()
        metadata = await score.metadata()
        measures = await score.measurePositions()
        segments = await score.segmentPositions()
    })

    for (const kind of ['measures', 'segments']) {
        it(`${kind}: well-formed elements and events`, () => {
            const pos = kind === 'measures' ? measures : segments
            assert.ok(pos.pageSize.width > 0 && pos.pageSize.height > pos.pageSize.width)
            pos.elements.forEach((el, i) => {
                assert.equal(el.id, i)
                assert.ok(Number.isInteger(el.page) && el.page >= 0 && el.page < npages, `page of ${i}`)
                for (const k of ['x', 'y', 'sx', 'sy']) {
                    assert.ok(Number.isFinite(el[k]) && el[k] >= 0, `${k} of ${i}`)
                }
                assert.ok(el.x + el.sx <= pos.pageSize.width + 1, `${i} fits in the page width`)
                assert.ok(el.y + el.sy <= pos.pageSize.height + 1, `${i} fits in the page height`)
            })
            // events: (element, time in ms) in playback order, every element is played
            for (let i = 1; i < pos.events.length; i++) {
                assert.ok(pos.events[i].position >= pos.events[i - 1].position)
            }
            const played = new Set(pos.events.map(e => e.elid))
            assert.equal(played.size, pos.elements.length)
        })
    }

    it('one element per measure, in reading order', () => {
        assert.equal(measures.elements.length, metadata.measures)
        for (let i = 1; i < measures.elements.length; i++) {
            const [a, b] = [measures.elements[i - 1], measures.elements[i]]
            assert.ok(b.page > a.page || (b.page === a.page && (b.y > a.y || (b.y === a.y && b.x > a.x))), `measure ${i}`)
        }
    })

    it('measures of a system are contiguous: x + sx is the next measure x', () => {
        // regression test: `sx` used to be the width of the measure's own bbox (7945a9b5a4)
        for (const system of systems(measures.elements)) {
            for (let i = 1; i < system.length; i++) {
                const [a, b] = [system[i - 1], system[i]]
                assert.ok(approxEqual(a.x + a.sx, b.x, 1), `measure ${a.id}: ${a.x} + ${a.sx} != ${b.x}`)
            }
        }
    })

    it('measures are on the pages that have notes', async () => {
        const musicPages = new Set(measures.elements.map(e => e.page))
        for (let page = 0; page < npages; page++) {
            const hasNotes = /class="Note"/.test(await score.saveSvg(page))
            assert.equal(musicPages.has(page), hasNotes, `page ${page}`)
        }
    })

    it('segments lie within their measure', () => {
        const measureOf = (seg) => measures.elements.find(m =>
            m.page === seg.page && approxEqual(m.y, seg.y, 1) && seg.x >= m.x - 1 && seg.x <= m.x + m.sx + 1)
        for (const seg of segments.elements) {
            assert.ok(measureOf(seg), `segment ${seg.id}`)
        }
    })

    it('the last event is about the duration of the score', () => {
        const last = measures.events.at(-1).position / 1000
        assert.ok(last < metadata.duration && last > metadata.duration - 10, `${last}s vs ${metadata.duration}s`)
    })
})
