// Export a score as audio in a fresh process: node save-audio.js <score> <format> <output>
// (audio export only works once per wasm instance, see audio-export.test.js)
import fs from 'node:fs'
import { loadScore, setSoundFont } from '../helpers.js'

const [score, format, output] = process.argv.slice(2)

await setSoundFont()
const s = await loadScore(score)
fs.writeFileSync(output, await s.saveAudio(format))
