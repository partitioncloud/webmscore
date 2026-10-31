import { describe, it } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import { WebMscore, SOUNDFONT } from './helpers.js'

describe('once the runtime is ready', () => {
    it('setSoundFont takes effect synchronously', async () => {
        await WebMscore.ready
        assert.ok(!WebMscore.hasSoundfont)
        // not awaited on purpose, like test_synth.js
        const done = WebMscore.setSoundFont(fs.readFileSync(SOUNDFONT))
        assert.ok(WebMscore.hasSoundfont)
        await done
    })
})
