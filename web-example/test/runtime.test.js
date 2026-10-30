import { describe, it } from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs'
// nothing must touch the runtime before these tests: no helper that awaits `ready`
import WebMscore from 'webmscore4'
import { SOUNDFONT } from './helpers.js'

describe('before the runtime is ready', () => {
    it('setSoundFont waits for the runtime', async () => {
        await WebMscore.setSoundFont(fs.readFileSync(SOUNDFONT))
        assert.ok(WebMscore.hasSoundfont)
    })
})
