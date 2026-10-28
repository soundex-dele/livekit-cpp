// Official SDK metadata requests plus legacy wire probes, with real synthetic media.
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
const configured = opts['configured-limits'] === 'true';
const nameLimit = configured ? 64 : 256, metadataLimit = configured ? 128 : 524288, attributesLimit = configured ? 64 : 65536;
const out = resolve(opts.output || resolve(here, 'results/go-signal-metadata'), new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, {recursive: true});
const sha = v => createHash('sha256').update(v).digest('hex');
const report = {fullReplacementVerified: false, singlePeerConnection: single, configuredLimits: configured, servers: {}, comparisons: [],
  scope: 'Self metadata permissions, atomic limits, request responses, legacy protocol gating and media continuity',
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
  const config = {port, keys: {[key]: secret}, ...(configured ? {limit: {max_participant_name_length: nameLimit, max_metadata_size: metadataLimit, max_attributes_size: attributesLimit}} : {}), ...(kind === 'go'
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
    const frames = {responses: [], updates: [], refresh: 0, reconnects: 0};
    page.on('websocket', ws => {
      ++frames.reconnects;
      ws.on('framereceived', ({payload}) => {
        try {
          const r = typeof payload === 'string' ? SignalResponse.fromJsonString(payload) : SignalResponse.fromBinary(payload);
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
  const state = async () => (await api.getParticipant(roomName, 'publisher')).toJson();
  async function stablePublisher(pub, before, socketCount) {
    assert.deepEqual(await state(), before, 'Rejected request changed participant state/version');
    assert.equal(pub.frames.reconnects, socketCount, 'Rejected request caused a new signaling session');
    assert.equal(await pub.page.evaluate(() => room.state === 'connected' && initialPCs[0] === room.engine.pcManager.publisher.pc && initialPCs[1] === room.engine.pcManager.subscriber?.pc), true);
    assert.equal(pub.frames.error, undefined);
  }
  try {
    await eventually(async () => {
      if (spawnError || child.exitCode !== null) throw spawnError || Error('Server exited');
      try { await api.listRooms(); return true; } catch { return false; }
    }, 'server ready', 30000);
    const pub = await client('publisher'), sub = await client('subscriber');
    await eventually(async () => (await api.getParticipant(roomName, 'publisher')).state === 2, 'publisher active');
    const sid = await pub.page.evaluate(async () => {
      const canvas = document.createElement('canvas'); canvas.width = 320; canvas.height = 180;
      const ctx = canvas.getContext('2d'); let frame = 0;
      setInterval(() => { ctx.fillStyle = `hsl(${frame++ % 360},80%,50%)`; ctx.fillRect(0, 0, 320, 180); }, 66);
      return (await room.localParticipant.publishTrack(canvas.captureStream(15).getVideoTracks()[0], {source: LivekitClient.Track.Source.Camera})).trackSid;
    });
    async function frames() {
      return sub.page.evaluate(async sid => {
        const stats = await remote.get(sid)?.getRTCStatsReport();
        return [...(stats?.values() || [])].find(s => s.type === 'inbound-rtp' && s.kind === 'video')?.framesDecoded || 0;
      }, sid);
    }
    async function media() {
      const before = await frames(); await eventually(async () => await frames() > before + 3, 'continuing decoded media', 20000);
    }
    await check('initial synthetic video decodes', async () => { await media(); return {decoded: true}; });
    const permission = allowed => ({canPublish: true, canSubscribe: true, canPublishData: true, canUpdateMetadata: allowed});
    async function rawUpdate(payload) {
      const id = await pub.page.evaluate(p => room.engine.client.sendUpdateLocalMetadata(p.metadata || '', p.name || '', p.attributes || {}), payload);
      await eventually(() => pub.frames.responses.some(r => r.requestId === id), 'correlated RequestResponse', 7000);
      return pub.frames.responses.find(r => r.requestId === id);
    }
    const rejectCases = [
      ['name exceeds byte limit', {name: 'n'.repeat(nameLimit + 1), metadata: 'must-not-apply'}, 'exceeds name length limit'],
      ['UTF-8 name counts bytes', {name: '中'.repeat(Math.floor(nameLimit / 3) + 1)}, 'exceeds name length limit'],
      ['metadata exceeds byte limit', {name: 'must-not-apply', metadata: 'm'.repeat(metadataLimit + 1)}, 'exceeds metadata size limit'],
      ['attributes include key bytes', {metadata: 'must-not-apply', attributes: {k: 'a'.repeat(attributesLimit)}}, 'exceeds attributes size limit'],
      ['name precedes other limits', {name: 'n'.repeat(nameLimit + 1), metadata: 'm'.repeat(metadataLimit + 1), attributes: {k: 'a'.repeat(attributesLimit)}}, 'exceeds name length limit'],
    ];
    for (const [name, payload, message] of rejectCases) await check(name, async () => {
      const before = await state(), socketCount = pub.frames.reconnects;
      const refresh = pub.frames.refresh, response = await rawUpdate(payload);
      assert.equal(response.reason, Reason.LIMIT_EXCEEDED); assert.equal(response.message, message);
      const echoed = response.request.value;
      assert.equal(response.request.case, 'updateMetadata'); assert.equal(echoed.requestId, response.requestId);
      assert.equal(echoed.name, payload.name || ''); assert.equal(echoed.metadata, payload.metadata || '');
      assert.deepEqual(echoed.attributes, payload.attributes || {});
      await stablePublisher(pub, before, socketCount); await media();
      assert.equal(pub.frames.refresh, refresh, 'Rejected request refreshed claims');
      return {reason: response.reason, message: response.message, echo: true, stateUnchanged: true, mediaContinues: true};
    });
    await check('SDK setName rejects with server reason, connection survives', async () => {
      const result = await pub.page.evaluate(async nameLimit => {
        try { await room.localParticipant.setName('n'.repeat(nameLimit + 1)); return {accepted: true}; }
        catch (e) { return {accepted: false, message: e.message}; }
      }, nameLimit);
      assert.deepEqual(result, {accepted: false, message: 'exceeds name length limit'}); await media(); return result;
    });
    await check('dynamic revocation precedes limit checks', async () => {
      await api.updateParticipant(roomName, 'publisher', {permission: permission(false)});
      const before = await state(), socketCount = pub.frames.reconnects;
      const response = await rawUpdate({name: 'n'.repeat(nameLimit + 1), metadata: 'm'.repeat(metadataLimit + 1)});
      assert.equal(response.reason, Reason.NOT_ALLOWED); assert.equal(response.message, 'does not have permission to update own metadata');
      await stablePublisher(pub, before, socketCount); await media();
      await api.updateParticipant(roomName, 'publisher', {permission: permission(true)});
      return {reason: response.reason, message: response.message, mediaContinues: true};
    });
    await check('exact name and attribute boundaries accepted', async () => {
      const response = await rawUpdate({name: 'n'.repeat(nameLimit), attributes: {k: 'a'.repeat(attributesLimit - 1)}});
      assert.equal(response.reason, 0); assert.equal(response.message, '');
      await eventually(async () => (await api.getParticipant(roomName, 'publisher')).name.length === nameLimit, 'name boundary applied');
      assert.equal((await api.getParticipant(roomName, 'publisher')).attributes.k.length, attributesLimit - 1);
      await pub.page.evaluate(() => room.localParticipant.setAttributes({k: ''})); await media();
      return {reason: response.reason, nameBytes: nameLimit, attributeBytes: attributesLimit};
    });
    await check('valid SDK update after rejections is seen by peer', async () => {
      await pub.page.evaluate(async () => {
        await room.localParticipant.setName('recovered'); await room.localParticipant.setMetadata('recovered-meta');
        await room.localParticipant.setAttributes({keep: '', added: 'yes'});
      });
      await eventually(() => sub.page.evaluate(() => {
        const p = room.remoteParticipants.get('publisher');
        return p?.name === 'recovered' && p.metadata === 'recovered-meta' && p.attributes.added === 'yes' && !p.attributes.keep;
      }), 'peer update'); await media();
      return {name: 'recovered', metadata: 'recovered-meta', attributes: {added: 'yes'}, mediaContinues: true};
    });
    // Old clients cannot parse unknown response variants. These wire probes
    // intentionally do not negotiate media; modern SDK cases above do.
    for (const [sdk, protocol, expectError, expectSuccess] of [['js', 14, true, false], ['js', 15, true, true], ['rust', 9, false, false], ['rust', 10, true, false], ['rust', 15, true, true]]) {
      await check(`legacy ${sdk}/${protocol} response gating`, async () => {
        const identity = `legacy-${sdk}-${protocol}`;
        const query = new URLSearchParams({access_token: await jwt(identity), sdk, protocol: String(protocol), auto_subscribe: '0'});
        const socket = new WebSocket(`${base.replace('http:', 'ws:')}/rtc?${query}`); sockets.push(socket); socket.binaryType = 'arraybuffer';
        const received = []; let socketError;
        socket.addEventListener('error', () => socketError = 'WebSocket error');
        socket.addEventListener('message', e => received.push(typeof e.data === 'string' ? SignalResponse.fromJsonString(e.data) : SignalResponse.fromBinary(new Uint8Array(e.data))));
        await eventually(() => { if (socketError) throw Error(socketError); return received.some(r => r.message.case === 'join'); }, 'wire join');
        const send = value => socket.send(new SignalRequest({message: value}).toBinary());
        send({case: 'updateMetadata', value: {requestId: 41, name: 'n'.repeat(nameLimit + 1)}});
        send({case: 'updateMetadata', value: {requestId: 42, name: 'accepted'}});
        send({case: 'pingReq', value: {timestamp: 1234n}});
        await eventually(() => received.some(r => r.message.case === 'pongResp'), 'processing barrier');
        const responses = received.filter(r => r.message.case === 'requestResponse').map(r => r.message.value);
        assert.equal(responses.some(r => r.requestId === 41), expectError);
        assert.equal(responses.some(r => r.requestId === 42), expectSuccess);
        if (expectError) assert.equal(responses.find(r => r.requestId === 41).reason, Reason.LIMIT_EXCEEDED);
        if (expectSuccess) assert.equal(responses.find(r => r.requestId === 42).reason, 0);
        assert.equal(socket.readyState, WebSocket.OPEN); socket.close();
        return {errorResponse: expectError, successResponse: expectSuccess, stillConnected: true};
      });
    }
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
    : report.comparisons.length === 15 && report.comparisons.every(c => c.passed) ? 0 : 1;
} catch (e) { report.infrastructureError = e.stack; process.exitCode = 2; }
finally {
  await browser?.close(); await new Promise(r => assets.close(r)); report.exitCode = process.exitCode || 0;
  report.passed = report.comparisons.filter(c => c.passed).length; report.total = report.comparisons.length;
  writeFileSync(resolve(out, 'report.json'), JSON.stringify(report, null, 2)); console.log(`${report.passed}/${report.total}; ${out}`);
}
