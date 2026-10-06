// Generates tests/fixtures/crt/reference.json from nesterm's CPU reference models
// (vendor/crt/reference/*.mjs, MIT, kathoc; exported from crt-physical-model). The XCTest
// CRTConformanceTests compares the Metal port (apps/macos/Sources/Core/CRT) against these values.
//
//   NESTERM_CRT=/path/to/nesterm/vendor/crt node tools/crt-reference/generate-fixtures.mjs
//
// Inputs are deterministic formulas mirrored in the Swift test (CRTConformanceTests.swift).
// Only every `step`-th row is stored to keep the fixture small.
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const crt = process.env.NESTERM_CRT;
if (!crt) throw new Error('set NESTERM_CRT to nesterm/vendor/crt');
const load = p => import(pathToFileURL(resolve(crt, p)).href);
const { AGCReceiver } = await load('reference/rf-agc.mjs');
const { signalFixture: v, makeCodeFrame, synthesizeLine, decodeLine, toLinearDrive } = await load('reference/composite.mjs');
const { designIF, receiveRF, rfFixture, carrierToNoiseDb, noiseSigma, noiseKey, hash32, gaussianAt } = await load('reference/rf.mjs');
const { GatedAGC } = await load('reference/agc.mjs');
const { renderTube, createTubePlan, evaluateGrowthPlan, mixTubeOutput } = await load('reference/tube.mjs');
const { SupplyState, rowMeans, applySupply } = await load('reference/supply.mjs');
const { applySpotH } = await load('reference/spot-h.mjs');
const { frameFractions } = await load('reference/phosphor.mjs');
const { rasterRowWeights } = await load('backends/raster-webgl.mjs');

const out = { generator: 'tools/crt-reference/generate-fixtures.mjs', cases: {} };
const pack = (data, width, height, step, offset) => {
  const rows = [];
  for (let y = offset; y < height; y += step) rows.push(y);
  const f = new Float32Array(rows.length * width * 3);
  rows.forEach((y, i) => f.set(data.subarray(y * width * 3, (y + 1) * width * 3), i * width * 3));
  return { width, height, rows, data: Buffer.from(f.buffer).toString('base64') };
};

// ---- codes pattern (mirrored in Swift: CRTConformanceTests.codes)
const bars = [0x30, 0x28, 0x2C, 0x2A, 0x24, 0x16, 0x12, 0x0F];
const codes = new Uint16Array(256 * 240);
for (let y = 0; y < 240; y++) for (let x = 0; x < 256; x++) {
  let c;
  if (y < 100) c = bars[x >> 5];
  else if (y < 150) c = ((x >> 4) & 15) | ((((y - 100) / 13 | 0) & 3) << 4);
  else if (y < 200) c = ((x >> 4) & 15) | (2 << 4) | ((((y - 150) / 7 | 0) & 7) << 6);
  else c = (((x >> 3) + (y >> 3)) % 2 === 0) ? 0x30 : 0x0F;
  codes[y * 256 + x] = c;
}
const burst = 1, ordinal = 7;
const starts = Float64Array.from({ length: 240 }, (_, r) => 36 + 4 * burst + 2728 * r);
const packet = { complete: true, codes, timing: { origin: 'core-observed-ppu-ticks', phaseOrigin: 'assumed-relative',
  absolutePhaseKnown: false, epoch: 0, frameOrdinal: ordinal, samplesPerPPUTick: 8,
  tickHz: { numerator: 472500000, denominator: 11 }, lineStartSamples: starts } };

// ---- 1. receiver, no noise: AGCReceiver (reference backend) -> linear drive
{
  const rx = new AGCReceiver({ backend: 'reference' });
  const r = rx.process(packet);
  const drive = toLinearDrive({ width: 512, height: 240, data: r.data });
  out.cases.receiver = { burstPhase: burst, ordinal, finalGain: r.metadata.finalGain, ...pack(drive.data, 512, 240, 24, 5) };
}
// ---- 2. receiver with M3-NOISE at 30 dBuV: rf-agc.mjs process() glue around receiveRF(noise)
{
  const antenna = 30, filter = designIF();
  const sigma = noiseSigma(carrierToNoiseDb(antenna), 1, filter), key = noiseKey(1, ordinal) | 0;
  const frame = makeCodeFrame(codes, { timing: packet.timing, initialPhase: 0 });
  const count = v.lineSamples * v.height, samples = new Float64Array(count);
  for (let row = 0; row < 240; row++) samples.set(synthesizeLine(frame, row).samples, row * v.lineSamples);
  const received = receiveRF(samples, { signalLevel: 1, filter, noise: { sigma, key } });
  const candidate = new GatedAGC({ initialGain: 1 });
  const data = new Float64Array(512 * 240 * 3), scale = rfFixture.modulationDepth / (v.white - v.sync), delay = filter.delaySamples;
  for (let row = 0; row < 240; row++) {
    const offset = row * v.lineSamples, gainBefore = candidate.gain, corrected = new Float64Array(v.lineSamples);
    for (let n = 0; n < v.lineSamples; n++) corrected[n] = v.sync + (1 - gainBefore * (1 - scale * (received.samples[offset + n] - v.sync))) / scale;
    data.set(decodeLine({ samples: corrected }).data, row * 512 * 3);
    // receiver-webgl.mjs gate: sync mean over 2324..2363, valid when the IF support is inside the packet.
    const gateValid = offset + 2324 - delay >= 0 && offset + 2363 + delay < count;
    let detectedSync = null;
    if (gateValid) { detectedSync = 0; for (let n = 2324; n < 2364; n++) detectedSync += gainBefore * (1 - scale * (received.samples[offset + n] - v.sync)); detectedSync /= 40; }
    candidate.advance({ elapsedSeconds: v.lineSamples / rfFixture.sampleRateHz, detectedSync, gateValid: gateValid && detectedSync > 0 });
  }
  const drive = toLinearDrive({ width: 512, height: 240, data });
  out.cases.receiverNoise = { burstPhase: burst, ordinal, antennaDbuv: antenna, sigma, key, ...pack(drive.data, 512, 240, 24, 5) };
  out.cases.noiseHash = { key, hash: [0, 1, 7, 654719].map(n => hash32(n)), gauss: [0, 1, 1000, 654719].map(n => gaussianAt(n, key)) };
}

// ---- drive pattern (mirrored in Swift: CRTConformanceTests.drive)
function drive(lines) {
  const data = new Float64Array(512 * lines * 3);
  for (let y = 0; y < lines; y++) for (let x = 0; x < 512; x++) for (let c = 0; c < 3; c++) {
    let val;
    if (x < 128) val = ((x + 3 * y + 50 * c) % 200) / 199;
    else if (x < 256) val = (((x >> 4) + ((y / 30) | 0) + c) % 3 === 0) ? 1 : 0.05 * c;
    else if (x < 384) val = (((x >> 1) + y) % 2) ? 0.8 : 0;
    else val = ((x * 37 + y * 101 + c * 53) % 1000) / 999;
    data[(y * 512 + x) * 3 + c] = val;
  }
  return { width: 512, height: lines, beamReferenceWidth: 256, beamReferenceHeight: 240, data };
}
const OW = 256, OH = 192;
// ---- 3. tube, fixed spot (renderTube = ambient + emission*(1-f) + scatter*f)
{
  const t = renderTube(drive(240), { outputWidth: OW, outputHeight: OH, samples: 4, ambientLux: 40, beamGrowth: 0 });
  out.cases.tube = pack(t.data, OW, OH, 12, 3);
}
// ---- 4. tube with beam growth: the GPU-conformance evaluator (evaluateGrowthPlan + mixTubeOutput)
{
  const input = drive(240);
  const plan = createTubePlan(input, { outputWidth: OW, outputHeight: OH, samples: 4, ambientLux: 40, beamGrowth: 1 });
  const ambient = renderTube(input, { outputWidth: OW, outputHeight: OH, samples: 4, ambientLux: 40, powered: false });
  const t = mixTubeOutput(evaluateGrowthPlan(plan, input), ambient);
  out.cases.tubeGrowth = pack(t.data, OW, OH, 12, 3);
}
// ---- 5. reduced raster (160 lines) through the tube, fixed spot
{
  const src = drive(240), lines = 160, data = new Float64Array(512 * lines * 3);
  for (let y = 0; y < lines; y++) for (const [row, w] of rasterRowWeights(lines, y))
    for (let i = 0; i < 512 * 3; i++) data[y * 512 * 3 + i] += src.data[row * 512 * 3 + i] * w;
  out.cases.raster160 = pack(data, 512, lines, 16, 1);
}
// ---- 6. supply/ABL over three frames (state carried), then horizontal spot growth
{
  const st = new SupplyState();
  let last;
  for (let f = 0; f < 3; f++) {
    const img = drive(240);
    if (f === 1) for (let i = 0; i < img.data.length; i++) img.data[i] = Math.min(1, img.data[i] + 0.5);
    last = applySupply(img, st.frame(rowMeans(img), 240));
  }
  out.cases.supply = pack(last.data, 512, 240, 24, 2);
  const spot = applySpotH(drive(240), 1);
  out.cases.spotH = pack(spot.data, 512, 240, 24, 2);
}
// ---- 7. phosphor persistence: 3 consecutive frames, scan-aligned (tube-webgl.mjs ring weights)
{
  const ambient = renderTube(drive(240), { outputWidth: OW, outputHeight: OH, samples: 4, ambientLux: 40, powered: false }).data;
  const lits = [];
  for (let f = 0; f < 3; f++) {
    const img = drive(240);
    if (f !== 2) for (let i = 0; i < img.data.length; i++) img.data[i] = f === 0 ? 1 - img.data[i] : img.data[i] * 0.25;
    const full = renderTube(img, { outputWidth: OW, outputHeight: OH, samples: 4, ambientLux: 40, beamGrowth: 0 }).data;
    lits.unshift(full.map((x, i) => x - ambient[i]));   // emitted light (ambient-free), newest first
  }
  const fr = [0, 1, 2].map(c => frameFractions(c));
  const res = Float64Array.from(ambient);
  lits.forEach((lit, m) => { for (let i = 0; i < res.length; i++) res[i] += lit[i] * (fr[i % 3][m] ?? 0); });
  out.cases.persistence = pack(res, OW, OH, 12, 3);
}
const file = join(dirname(fileURLToPath(import.meta.url)), '../../tests/fixtures/crt/reference.json');
mkdirSync(dirname(file), { recursive: true });
writeFileSync(file, JSON.stringify(out));
console.log('wrote', file);
