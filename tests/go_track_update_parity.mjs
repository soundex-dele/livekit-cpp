// Track update differential using official SDK sessions and synthetic audio/video.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {createServer as tcpServer} from 'node:net';
import {once} from 'node:events';
import {createRequire} from 'node:module';
import {randomBytes, createHash} from 'node:crypto';
import {readFileSync, writeFileSync, mkdirSync, unlinkSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const opts = Object.fromEntries(process.argv.slice(2).reduce((a, s, i, args) => {
  if (s.startsWith('--')) a.push([s.slice(2), args[i + 1]]); return a;
}, []));
assert.ok(opts['go-server'] && opts['cpp-server'], 'Pass --go-server and --cpp-server');
const require = createRequire(resolve(here, 'package.json'));
const modern = resolve(opts['modern-node-modules'] || 'D:/workspace/livekit-cli-cpp/web/node_modules');
const modernRequire = createRequire(resolve(modern, '../package.json'));
const {chromium} = require('playwright');
const {AccessToken, RoomServiceClient} = require('livekit-server-sdk');
const {SignalRequest, SignalResponse, RequestResponse_Reason: Reason} = modernRequire('@livekit/protocol');
const single = opts['single-pc'] === 'true';
const out = resolve(opts.output || resolve(here, 'results/go-track-update'), new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, {recursive: true});
const sha = v => createHash('sha256').update(v).digest('hex');
const report = {fullReplacementVerified: false, singlePeerConnection: single, servers: {}, comparisons: [],
  scope: 'Local audio/video track updates, ownership and media continuity',
  sdkVersion: JSON.parse(readFileSync(resolve(modern, 'livekit-client/package.json'))).version,
  testSha256: sha(readFileSync(fileURLToPath(import.meta.url)))};
const delay = ms => new Promise(r => setTimeout(r, ms));
async function eventually(fn, message, timeout = 12000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) { if (await fn()) return; await delay(30); }
  throw Error('Timed out: ' + message);
}
async function freePort() {
  const s = tcpServer(); s.listen(0, '127.0.0.1'); await once(s, 'listening');
  const port = s.address().port; await new Promise(r => s.close(r)); return port;
}
function compact(v) {
  if (typeof v === 'string' && v.length > 1024) return {bytes: Buffer.byteLength(v), sha256: sha(v)};
  if (Array.isArray(v)) return v.map(compact);
  if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, compact(x)]));
  return v;
}
const sdk = readFileSync(resolve(modern, 'livekit-client/dist/livekit-client.umd.js'));
const assets = createServer((req, res) => {
  res.setHeader('Content-Type', req.url === '/sdk.js' ? 'application/javascript' : 'text/html');
  res.end(req.url === '/sdk.js' ? sdk : '<!doctype html><script src="/sdk.js"></script>');
});
let browser;
async function exercise(kind, binary) {
  const record = report.servers[kind] = {binary: resolve(binary), sha256: sha(readFileSync(binary)), cases: []};
  const key = 'metadata', secret = randomBytes(32).toString('hex'), roomName = 'metadata';
  const port = await freePort(), base = `http://127.0.0.1:${port}`;
  const config = {port, keys: {[key]: secret}, ...(kind === 'go'
    ? {bind_addresses: ['127.0.0.1'], rtc: {tcp_port: 0, port_range_start: 61000, port_range_end: 61100, use_external_ip: false, node_ip: '127.0.0.1', enable_loopback_candidate: true}}
    : {bind: '127.0.0.1', rtc: {port_range_start: 61110, port_range_end: 61210}})};
  record.config = {...config, keys: '[ephemeral, redacted]'};
  const path = resolve(out, kind + '.json'); writeFileSync(path, JSON.stringify(config));
  const child = spawn(resolve(binary), ['--config', path], {windowsHide: true});
  let log = '', spawnError;
  child.on('error', e => spawnError = e); child.stdout.on('data', b => log += b); child.stderr.on('data', b => log += b);
  const contexts = [], sockets = [];
  const api = new RoomServiceClient(base, key, secret);
  async function jwt(identity, grant = {}) {
    const t = new AccessToken(key, secret, {identity, ttl: '15m', name: 'initial', metadata: 'initial-meta', attributes: {keep: 'yes'}});
    t.addGrant({roomJoin: true, room: roomName, canUpdateOwnMetadata: true, ...grant}); return t.toJwt();
  }
  async function check(name, fn) {
    const start = Date.now();
    try { const evidence = await fn(); record.cases.push({name, passed: true, evidence: compact(evidence), elapsedMs: Date.now() - start}); }
    catch (e) { record.cases.push({name, passed: false, failure: e.stack, elapsedMs: Date.now() - start}); }
    console.log(`${kind}: ${record.cases.at(-1).passed ? 'PASS' : 'FAIL'} ${name}`);
  }
  async function client(identity) {
    const context = await browser.newContext(); contexts.push(context); const page = await context.newPage();
    const frames = {responses: [], updates: [], pongs: [], refresh: 0, reconnects: 0};
    page.on('websocket', ws => {
      ++frames.reconnects;
      ws.on('framereceived', ({payload}) => {
        try {
          const r = typeof payload === 'string' ? SignalResponse.fromJsonString(payload) : SignalResponse.fromBinary(payload);
          if (r.message.case === 'pongResp') frames.pongs.push(r.message.value);
          if (r.message.case === 'requestResponse') frames.responses.push(r.message.value);
          if (r.message.case === 'update') frames.updates.push(...r.message.value.participants);
          if (r.message.case === 'refreshToken') ++frames.refresh;
        } catch (e) { frames.error = e.message; }
      });
    });
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async ({base, token, single}) => {
      window.room = new LivekitClient.Room({singlePeerConnection: single, publishDefaults: {simulcast: false, videoCodec: 'vp8', red: false}});
      window.remote = new Map();
      room.on(LivekitClient.RoomEvent.TrackSubscribed, (track, pub) => { remote.set(pub.trackSid, track); document.body.appendChild(track.attach()); });
      await room.connect(base.replace('http:', 'ws:'), token, {rtcConfig: {iceServers: []}});
      window.initialPCs = [room.engine.pcManager.publisher.pc, room.engine.pcManager.subscriber?.pc];
    }, {base, token: await jwt(identity), single});
    assert.equal(await page.evaluate(() => room.engine.pcManager.mode), single ? 'publisher-only' : 'subscriber-primary');
    return {page, frames};
  }
  try {
    await eventually(async () => {
      if (spawnError || child.exitCode !== null) throw spawnError || Error('Server exited');
      try { await api.listRooms(); return true; } catch { return false; }
    }, 'server ready', 30000);
    const pub = await client('publisher'), sub = await client('subscriber');
    await eventually(async () => (await api.getParticipant(roomName, 'publisher')).state === 2, 'publisher active');
    const tracks = await pub.page.evaluate(async () => {
      window.canvas = document.createElement('canvas'); canvas.width = 320; canvas.height = 180;
      const ctx = canvas.getContext('2d'); let frame = 0;
      setInterval(() => { ctx.fillStyle = `hsl(${frame++ % 360},80%,50%)`; ctx.fillRect(0, 0, canvas.width, canvas.height); }, 66);
      const video = await room.localParticipant.publishTrack(canvas.captureStream(15).getVideoTracks()[0], {source: LivekitClient.Track.Source.Camera});
      window.audio = new AudioContext(); const oscillator = audio.createOscillator();
      oscillator.frequency.value = 440; const gain = audio.createGain(); gain.gain.value = 0.1;
      const destination = audio.createMediaStreamDestination(); oscillator.connect(gain).connect(destination); oscillator.start(); await audio.resume();
      const sound = await room.localParticipant.publishTrack(destination.stream.getAudioTracks()[0], {source: LivekitClient.Track.Source.Microphone, dtx: false});
      return {video: video.trackSid, audio: sound.trackSid};
    });
    const trackState = async sid => (await api.getParticipant(roomName, 'publisher')).tracks.find(t => t.sid === sid);
    async function stats() {
      return sub.page.evaluate(async tracks => {
        const result = {};
        for (const [kind, sid] of Object.entries(tracks)) {
          const report = await remote.get(sid)?.getRTCStatsReport();
          const s = [...(report?.values() || [])].find(s => s.type === 'inbound-rtp' && s.kind === kind);
          result[kind] = {frames: s?.framesDecoded || 0, samples: s?.totalSamplesReceived || 0, energy: s?.totalAudioEnergy || 0, width: s?.frameWidth || 0};
        }
        return result;
      }, tracks);
    }
    async function media() {
      const before = await stats();
      await eventually(async () => {
        const now = await stats();
        return now.video.frames > before.video.frames + 3 && now.audio.samples > before.audio.samples && now.audio.energy > before.audio.energy;
      }, 'video decoding and audible audio continue', 20000);
      for (const c of [pub, sub]) assert.equal(await c.page.evaluate(() => room.state === 'connected' && initialPCs[0] === room.engine.pcManager.publisher.pc && initialPCs[1] === room.engine.pcManager.subscriber?.pc), true);
    }
    let ping = 2000;
    async function send(c, type, payload) {
      const barrier = ++ping, start = c.frames.responses.length;
      const bytes = Array.from(new SignalRequest({message: {case: type, value: payload}}).toBinary());
      const pingBytes = Array.from(new SignalRequest({message: {case: 'pingReq', value: {timestamp: BigInt(barrier)}}}).toBinary());
      await c.page.evaluate(async ({bytes, pingBytes, type, payload}) => {
        if (type === 'updateAudioTrack') await room.engine.client.sendUpdateLocalAudioTrack(payload.trackSid, payload.features || []);
        else await room.engine.client.streamWriter.write(new Uint8Array(bytes).buffer);
        await room.engine.client.streamWriter.write(new Uint8Array(pingBytes).buffer);
      }, {bytes, pingBytes, type, payload});
      await eventually(() => c.frames.pongs.some(p => p.lastPingTimestamp === BigInt(barrier)), 'signal processing barrier');
      return c.frames.responses.slice(start);
    }
    async function rejected(c, type, payload) {
      const before = (await api.getParticipant(roomName, 'publisher')).toJson();
      const responses = await send(c, type, payload);
      assert.equal(responses.length, 1); const r = responses[0];
      assert.equal(r.reason, Reason.NOT_FOUND); assert.equal(r.message, ''); assert.equal(r.requestId, 0);
      assert.equal(r.request.case, type);
      const expected = new SignalRequest({message: {case: type, value: payload}}).message.value;
      assert.deepEqual(r.request.value.toJson(), expected.toJson());
      assert.deepEqual((await api.getParticipant(roomName, 'publisher')).toJson(), before);
      await media(); return {reason: r.reason, echo: true, stateUnchanged: true, mediaContinues: true};
    }
    async function accepted(type, payload, match, peerMatch = match) {
      const start = sub.frames.updates.length;
      assert.deepEqual(await send(pub, type, payload), [], 'Go sends no successful response for track updates');
      await eventually(async () => match(await trackState(payload.trackSid)), 'updated server track info');
      try {
        await eventually(() => sub.frames.updates.slice(start).some(p => p.identity === 'publisher' && p.tracks.some(t => t.sid === payload.trackSid && peerMatch(t))), 'updated peer track info');
      } catch (e) { throw new Error(e.message + JSON.stringify({server: (await trackState(payload.trackSid))?.toJson(), events: sub.frames.updates.slice(start).flatMap(p => p.tracks.filter(t => t.sid === payload.trackSid).map(t => t.toJson()))})); }
      await media(); return {stateAndPeerUpdated: true, successResponse: false, mediaContinues: true};
    }
    await check('initial audio and video decode', async () => { await media(); return {decoded: true}; });
    await check('missing audio track returns correlated error', () => rejected(pub, 'updateAudioTrack', {trackSid: 'TR_missing', features: [1, 0, 1]}));
    await check('missing video track returns correlated error', () => rejected(pub, 'updateVideoTrack', {trackSid: 'TR_missing', width: 640, height: 360}));
    await check('foreign audio track cannot be updated', () => rejected(sub, 'updateAudioTrack', {trackSid: tracks.audio, features: [0]}));
    await check('foreign video track cannot be updated', () => rejected(sub, 'updateVideoTrack', {trackSid: tracks.video, width: 1, height: 1}));
    await check('audio features sorted deduplicated and stereo DTX synchronized', () => accepted('updateAudioTrack', {trackSid: tracks.audio, features: [5, 1, 0, 5, 99, 1]}, t => t && t.stereo && t.disableDtx && JSON.stringify(t.audioFeatures) === '[0,1,5,99]'));
    await check('empty audio features clears stereo and DTX', () => accepted('updateAudioTrack', {trackSid: tracks.audio, features: []}, t => t && !t.stereo && !t.disableDtx && t.audioFeatures.length === 0));
    await check('video dimensions update and new size decodes', async () => {
      await pub.page.evaluate(() => { canvas.width = 640; canvas.height = 360; });
      const result = await accepted('updateVideoTrack', {trackSid: tracks.video, width: 640, height: 360}, t => t?.width === 640 && t.height === 360);
      await eventually(async () => (await stats()).video.width === 640, 'resized video decoded'); return {...result, decodedWidth: 640};
    });
    await check('video zero dimensions accepted', () => accepted('updateVideoTrack', {trackSid: tracks.video, width: 0, height: 0}, t => t && t.width === 0 && t.height === 0));
    await check('audio update on video ignores fields', async () => {
      const before = (await trackState(tracks.video)).toJson();
      const peerBefore = sub.frames.updates.flatMap(p => p.tracks).findLast(t => t.sid === tracks.video).toJson();
      return accepted('updateAudioTrack', {trackSid: tracks.video, features: [0, 1]},
        t => t && JSON.stringify(t.toJson()) === JSON.stringify(before),
        t => JSON.stringify(t.toJson()) === JSON.stringify(peerBefore));
    });
    await check('video update on audio ignores fields', async () => {
      const before = (await trackState(tracks.audio)).toJson();
      const peerBefore = sub.frames.updates.flatMap(p => p.tracks).findLast(t => t.sid === tracks.audio).toJson();
      return accepted('updateVideoTrack', {trackSid: tracks.audio, width: 100, height: 200},
        t => t && JSON.stringify(t.toJson()) === JSON.stringify(before),
        t => JSON.stringify(t.toJson()) === JSON.stringify(peerBefore));
    });
    await check('repeat audio update remains usable', async () => {
      const match = t => t && t.audioFeatures.length === 1 && t.audioFeatures[0] === 4;
      await accepted('updateAudioTrack', {trackSid: tracks.audio, features: [4]}, match);
      return accepted('updateAudioTrack', {trackSid: tracks.audio, features: [4, 4]}, match);
    });
    record.completed = true;
  } catch (e) { record.infrastructureError = e.stack; }
  finally {
    for (const ws of sockets) ws.close(); for (const c of contexts) await c.close();
    if (child.exitCode === null && child.signalCode === null) { const exited = once(child, 'exit'); child.kill(); await exited; }
    writeFileSync(resolve(out, kind + '.log'), log); unlinkSync(path);
  }
}
try {
  assets.listen(0, '127.0.0.1'); await once(assets, 'listening');
  browser = await chromium.launch({channel: 'chrome', headless: true, args: ['--autoplay-policy=no-user-gesture-required']});
  report.browser = browser.version();
  await exercise('go', opts['go-server']); await exercise('cpp', opts['cpp-server']);
  for (const expected of report.servers.go.cases) {
    const actual = report.servers.cpp.cases.find(c => c.name === expected.name); let failure;
    try { assert.ok(expected.passed && actual?.passed); assert.deepEqual(actual.evidence, expected.evidence); }
    catch (e) { failure = e.message; }
    report.comparisons.push({name: expected.name, passed: !failure, ...(failure ? {failure} : {})});
  }
  process.exitCode = Object.values(report.servers).some(s => !s.completed) ? 2
    : report.comparisons.length === 12 && report.comparisons.every(c => c.passed) ? 0 : 1;
} catch (e) { report.infrastructureError = e.stack; process.exitCode = 2; }
finally {
  await browser?.close(); await new Promise(r => assets.close(r)); report.exitCode = process.exitCode || 0;
  report.passed = report.comparisons.filter(c => c.passed).length; report.total = report.comparisons.length;
  writeFileSync(resolve(out, 'report.json'), JSON.stringify(report, null, 2)); console.log(`${report.passed}/${report.total}; ${out}`);
}
