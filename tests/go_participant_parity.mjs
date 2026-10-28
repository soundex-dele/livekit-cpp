// Same-host, headless official SDK media checks against pinned Go and C++.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { createServer as tcpServer } from 'node:net';
import { createRequire } from 'node:module';
import { once } from 'node:events';
import { randomBytes, createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync, unlinkSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const opts = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1]]); return a;
}, []));
assert.ok(opts['go-server'] && opts['cpp-server'], 'Pass --go-server and --cpp-server');
const deps = resolve(opts['node-modules'] || resolve(here, 'node_modules'));
const require = createRequire(resolve(deps, '../package.json'));
const { chromium } = require('playwright');
const { AccessToken, RoomServiceClient } = require('livekit-server-sdk');
const { UpdateParticipantRequest } = require('@livekit/protocol');
const out = resolve(opts.output || resolve(here, 'results/go-participant-parity'),
  new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, { recursive: true });
const report = { fullReplacementVerified: false, scope: 'hidden participants, kinds, dependent room lifecycle and atomic API limit rejection with live media', servers: {}, comparisons: [] };
const delay = ms => new Promise(r => setTimeout(r, ms));
async function eventually(fn, label, timeout = 15000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { if (await fn()) return; await delay(50); }
  throw new Error(`Timed out: ${label}`);
}
async function port() {
  const s = tcpServer(); s.listen(0, '127.0.0.1'); await once(s, 'listening');
  const p = s.address().port; await new Promise(r => s.close(r)); return p;
}
const sdk = readFileSync(resolve(deps, 'livekit-client/dist/livekit-client.umd.js'));
const assets = createServer((req, res) => {
  if (req.url === '/sdk.js') { res.setHeader('Content-Type', 'application/javascript'); res.end(sdk); }
  else res.end('<!doctype html><script src="/sdk.js"></script>');
});
let browser;
async function exercise(kind, binary) {
  const key = 'participanttest', secret = randomBytes(32).toString('hex');
  let roomName = 'participants';
  const p = await port(), base = `http://127.0.0.1:${p}`;
  const configPath = resolve(out, `${kind}.${kind === 'go' ? 'yaml' : 'json'}`);
  writeFileSync(configPath, kind === 'go'
    ? `port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 59800\n  port_range_end: 59980\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`
    : JSON.stringify({ bind: '127.0.0.1', port: p, keys: { [key]: secret }, rtc: { port_range_start: 60000, port_range_end: 60180 } }));
  const record = report.servers[kind] = { binary: resolve(binary), sha256: createHash('sha256').update(readFileSync(binary)).digest('hex'), cases: [] };
  const child = spawn(resolve(binary), ['--config', configPath], { windowsHide: true });
  let log = '', spawnError;
  child.stdout.on('data', b => { log += b; }); child.stderr.on('data', b => { log += b; });
  child.on('error', e => { spawnError = e; });
  const contexts = [];
  const api = new RoomServiceClient(base, key, secret);
  async function client(identity, autoSubscribe = true, grant = {}, participantKind, details = []) {
    const token = new AccessToken(key, secret, { identity, ttl: '15m' });
    if (participantKind) token.kind = participantKind;
    token.grants.kindDetails = details;
    token.addGrant({ roomJoin: true, room: roomName, ...grant });
    const context = await browser.newContext(); contexts.push(context);
    const page = await context.newPage();
    page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') log += `[${identity}] ${m.text()}\n`; });
    page.on('pageerror', e => { log += `[${identity}] ${e.stack}\n`; });
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async ({ base, token, autoSubscribe }) => {
      const { Room, RoomEvent, Track } = LivekitClient;
      window.room = new Room({ adaptiveStream: false, dynacast: false, publishDefaults: { simulcast: false, videoCodec: 'vp8', red: false } });
      window.tracks = new Map(); window.received = []; window.permissions = []; window.events = [];
      room.on(RoomEvent.ParticipantConnected, p => events.push({ type: "joined", identity: p.identity }));
      room.on(RoomEvent.ParticipantDisconnected, p => events.push({ type: "left", identity: p.identity }));
      room.on(RoomEvent.ParticipantMetadataChanged, (metadata, p) => events.push({ type: "metadata", identity: p.identity, metadata }));
      const old = room.engine.client.onSubscriptionPermissionUpdate;
      room.engine.client.onSubscriptionPermissionUpdate = update => { permissions.push(update); old?.(update); };
      room.on(RoomEvent.TrackSubscribed, (track, pub) => {
        tracks.set(pub.trackSid, track); const el = track.attach(); el.autoplay = true; document.body.appendChild(el);
      });
      room.on(RoomEvent.TrackUnsubscribed, (track, pub) => { tracks.delete(pub.trackSid); track.detach().forEach(el => el.remove()); });
      room.on(RoomEvent.DataReceived, (bytes, p) => received.push({text: new TextDecoder().decode(bytes), identity: p?.identity || ''}));
      window.publishVideo = async () => {
        const canvas = document.createElement('canvas'); canvas.width = 320; canvas.height = 240;
        document.body.appendChild(canvas); const ctx = canvas.getContext('2d'); let frame = 0;
        setInterval(() => { ctx.fillStyle = `hsl(${frame++ % 360},80%,50%)`; ctx.fillRect(0, 0, 320, 240); }, 66);
        return (await room.localParticipant.publishTrack(canvas.captureStream(15).getVideoTracks()[0], { source: Track.Source.Camera })).trackSid;
      };
      window.publishAudio = async () => {
        window.audioContext = new AudioContext();
        const oscillator = audioContext.createOscillator(), gain = audioContext.createGain(); gain.gain.value = 0.1;
        const dest = audioContext.createMediaStreamDestination(); oscillator.connect(gain).connect(dest); oscillator.start(); await audioContext.resume();
        return (await room.localParticipant.publishTrack(dest.stream.getAudioTracks()[0], { source: Track.Source.Microphone, red: false })).trackSid;
      };
      await room.connect(base.replace('http:', 'ws:'), token, { autoSubscribe, rtcConfig: { iceServers: [] } });
    }, { base, token: await token.toJwt(), autoSubscribe });
    return page;
  }
  async function check(name, fn) {
    const started = Date.now();
    try { await fn(); record.cases.push({ name, passed: true, elapsedMs: Date.now() - started }); console.log(`${kind}: PASS ${name}`); }
    catch (e) { record.cases.push({ name, passed: false, failure: e.stack }); throw e; }
  }
  const set = (pub, all, entries = []) => pub.evaluate(({ all, entries }) => room.localParticipant.setTrackSubscriptionPermissions(all, entries), { all, entries });
  const raw = (pub, all, entries) => pub.evaluate(({ all, entries }) => room.engine.client.sendUpdateSubscriptionPermissions(all, entries), { all, entries });
  async function media(page, sids) {
    const baseline = await page.evaluate(async sids => {
      const values = {};
      for (const sid of sids) (await tracks.get(sid)?.getRTCStatsReport())?.forEach(s => {
        if (s.type === 'inbound-rtp') values[sid] = { id: s.id, frames: s.framesDecoded || 0, energy: s.totalAudioEnergy || 0 };
      });
      return values;
    }, sids);
    await eventually(() => page.evaluate(async ({ sids, baseline }) => {
      for (const sid of sids) {
        const track = tracks.get(sid); if (!track) return false;
        let active = false;
        (await track.getRTCStatsReport())?.forEach(s => {
          const before = baseline[sid]?.id === s.id ? baseline[sid] : { frames: 0, energy: 0 };
          if (s.type === 'inbound-rtp' && (s.kind === 'video'
            ? s.framesDecoded > Math.max(5, before.frames + 2)
            : s.totalAudioEnergy > Math.max(0.001, before.energy + 0.00001))) active = true;
        });
        if (!active) return false;
      }
      return true;
    }, { sids, baseline }), 'advancing decoded media for allowed tracks', 30000);
  }
  try {
    await eventually(async () => {
      if (spawnError || child.exitCode !== null) throw spawnError || new Error('Server exited');
      try { await api.listRooms(); return true; } catch { return false; }
    }, 'server ready', 30000);
    const visible = page => page.evaluate(() => [...room.remoteParticipants.keys()].sort());
    const disconnect = async page => { await page.evaluate(() => room.disconnect()); await page.context().close(); };
    const closeContexts = async () => { for (const c of contexts) await c.close(); contexts.length = 0; };
    const permission = extra => ({ canPublish: true, canSubscribe: true, canPublishData: true, ...extra });
    async function count(expected, recording) {
      await eventually(async () => {
        const r = (await api.listRooms([roomName]))[0];
        return r && r.numParticipants === expected && (recording === undefined || r.activeRecording === recording);
      }, `room participants=${expected}, recording=${recording}`);
    }
    await api.createRoom({ name: roomName });
    const observer = await client('observer'); let hidden, late;
    await check('hidden join succeeds but is not announced to peers', async () => {
      hidden = await client('hidden', true, { hidden: true }); late = await client('late');
      await eventually(() => observer.evaluate(() => room.remoteParticipants.has('late')), 'visible join barrier');
      assert.deepEqual(await visible(observer), ['late']);
      assert.equal(await observer.evaluate(() => events.some(e => e.identity === 'hidden')), false);
      assert.equal((await api.getParticipant(roomName, 'hidden')).permission.hidden, true);
    });
    await check('join snapshot hides hidden peers but hidden participant sees others', async () => {
      assert.deepEqual(await visible(late), ['observer']);
      await eventually(() => hidden.evaluate(() => room.remoteParticipants.size === 2), 'hidden receives peer updates');
      assert.deepEqual(await visible(hidden), ['late', 'observer']);
    });
    await check('admin can list hidden participants and hidden is counted', async () => {
      assert.deepEqual((await api.listParticipants(roomName)).map(p => p.identity).sort(), ['hidden', 'late', 'observer']);
      await count(3, false);
    });
    await check('hidden metadata updates go only to self', async () => {
      await api.updateParticipant(roomName, 'hidden', { metadata: 'private-state' });
      await eventually(() => hidden.evaluate(() => room.localParticipant.metadata === 'private-state'), 'hidden self metadata');
      await delay(300);
      assert.equal(await observer.evaluate(() => events.some(e => e.identity === 'hidden')), false);
    });
    for (const reliable of [true, false]) await check(`hidden ${reliable ? 'reliable' : 'lossy'} data omits sender identity`, async () => {
      const text = `anonymous-${reliable}`;
      await hidden.evaluate(({text, reliable}) => room.localParticipant.publishData(new TextEncoder().encode(text), { reliable }), {text, reliable});
      await eventually(() => observer.evaluate(text => received.some(d => d.text === text), text), 'hidden data');
      assert.equal(await observer.evaluate(text => received.find(d => d.text === text).identity, text), '');
    });
    await check('hidden participant receives directed data and real video', async () => {
      await observer.evaluate(() => room.localParticipant.publishData(new TextEncoder().encode('directed'), { reliable: true, destinationIdentities: ['hidden'] }));
      await eventually(() => hidden.evaluate(() => received.some(d => d.text === 'directed' && d.identity === 'observer')), 'directed data');
      assert.equal(await late.evaluate(() => received.some(d => d.text === 'directed')), false);
      const sid = await observer.evaluate(() => publishVideo()); await media(hidden, [sid]);
    });
    await check('hidden departure is not broadcast', async () => {
      await disconnect(hidden); await count(2);
      await delay(250); assert.equal(await observer.evaluate(() => events.some(e => e.identity === 'hidden')), false);
    });
    const dynamic = await client('dynamic');
    await eventually(() => observer.evaluate(() => room.remoteParticipants.has('dynamic')), 'dynamic visible');
    await check('dynamic hide suppresses future updates and new join snapshots', async () => {
      await api.updateParticipant(roomName, 'dynamic', { permission: permission({ hidden: true }), metadata: 'hidden-update' });
      await eventually(() => dynamic.evaluate(() => room.localParticipant.metadata === 'hidden-update'), 'dynamic self update');
      const newer = await client('newer');
      assert.equal((await visible(newer)).includes('dynamic'), false);
      // Pinned Go does not retroactively remove an already known participant.
      assert.equal((await visible(observer)).includes('dynamic'), true);
      // Go applies metadata before permission in a combined API request.
      assert.equal(await observer.evaluate(() => room.remoteParticipants.get('dynamic')?.metadata === 'hidden-update'), true);
      await api.updateParticipant(roomName, 'dynamic', { metadata: 'later-private-update' });
      await eventually(() => dynamic.evaluate(() => room.localParticipant.metadata === 'later-private-update'), 'later hidden self update');
      await delay(250);
      assert.equal(await observer.evaluate(() => room.remoteParticipants.get('dynamic')?.metadata === 'later-private-update'), false);
      await disconnect(newer);
    });
    await check('dynamic unhide broadcasts current state', async () => {
      await api.updateParticipant(roomName, 'dynamic', { permission: permission({ hidden: false }), metadata: 'public-update' });
      await eventually(() => observer.evaluate(() => room.remoteParticipants.get('dynamic')?.metadata === 'public-update'), 'visible metadata');
    });
    for (const binary of [false, true]) await check(`${binary ? 'protobuf' : 'json'} over-limit update preserves participant state and media`, async () => {
      const before = await api.getParticipant(roomName, 'observer');
      const admin = new AccessToken(key, secret); admin.addGrant({ room: roomName, roomAdmin: true });
      const jwt = await admin.toJwt();
      for (const invalid of [{ name: 'n'.repeat(257) }, { metadata: 'm'.repeat(524289) }, { attributes: { key: 'a'.repeat(65536) } }]) {
        const payload = { room: roomName, identity: 'observer', name: 'must-not-apply', metadata: 'must-not-apply',
          permission: { canPublish: false, canSubscribe: false, canPublishData: false }, ...invalid };
        const response = await fetch(`${base}/twirp/livekit.RoomService/UpdateParticipant`, {
          method: 'POST', headers: { Authorization: `Bearer ${jwt}`, 'Content-Type': binary ? 'application/protobuf' : 'application/json' },
          body: binary ? new UpdateParticipantRequest(payload).toBinary() : JSON.stringify(payload), signal: AbortSignal.timeout(10000),
        });
        assert.equal(response.status, 400); assert.equal((await response.json()).code, 'invalid_argument');
        const after = await api.getParticipant(roomName, 'observer');
        assert.deepEqual(after.toJson(), before.toJson(), 'Rejected update changed participant state/version');
      }
      assert.equal(await observer.evaluate(() => room.state), 'connected');
      const tracks = before.tracks.filter(t => t.type === 1).map(t => t.sid);
      assert.ok(tracks.length, 'Live publisher required for rejection test'); await media(late, tracks);
    });
    await api.deleteRoom(roomName);
    await closeContexts();

    roomName = 'capacity'; await api.createRoom({ name: roomName, maxParticipants: 1 });
    const human = await client('human');
    let recorder, egress, agent, agentKind;
    await check('recorder flag joins full room without consuming capacity', async () => {
      recorder = await client('recorder', true, { recorder: true }); await count(1, true);
      await eventually(() => human.evaluate(() => room.isRecording), 'recording event');
    });
    await check('egress and agent kinds plus legacy agent grant are dependent', async () => {
      egress = await client('egress', true, {}, 'EGRESS');
      agentKind = await client('agent-kind', true, {}, 'AgEnT');
      agent = await client('agent-flag', true, { agent: true });
      await count(1, true);
      assert.equal((await api.getParticipant(roomName, 'egress')).kind, 2);
      assert.equal((await api.getParticipant(roomName, 'agent-kind')).kind, 4);
      const p = await api.getParticipant(roomName, 'agent-flag'); assert.equal(p.kind, 0); assert.equal(p.permission.agent, true);
    });
    await check('hidden normal participant still obeys capacity', async () => {
      await assert.rejects(() => client('over-capacity', true, { hidden: true }));
      await count(1, true);
      assert.equal((await api.listParticipants(roomName)).some(p => p.identity === 'over-capacity'), false);
    });
    await check('recording stays active until last recorder departs', async () => {
      await disconnect(recorder); await count(1, true); assert.equal(await human.evaluate(() => room.isRecording), true);
      await disconnect(egress); await count(1, false); await eventually(() => human.evaluate(() => !room.isRecording), 'recording stopped event');
    });
    await check('changing agent flag changes room count and capacity', async () => {
      await api.updateParticipant(roomName, 'human', { permission: permission({ agent: true }) }); await count(0);
      const next = await client('next-human'); await count(1); await disconnect(next); await count(0);
      await api.updateParticipant(roomName, 'human', { permission: permission({ agent: false }) }); await count(1);
    });
    await api.deleteRoom(roomName);
    await closeContexts();

    roomName = 'kinds'; await api.createRoom({ name: roomName });
    await check('participant kinds and kindDetails preserve protocol semantics', async () => {
      for (const [name, expected] of [['STANDARD',0], ['ingress',1], ['sip',3], ['connector',7], ['bridge',8], ['unknown',0]]) {
        const page = await client(`kind-${name}`, true, {}, name, ['CLOUD_AGENT', 'forwarded', 'unknown', 'CLOUD_AGENT']);
        const p = await api.getParticipant(roomName, `kind-${name}`);
        // This pinned server SDK predates CONNECTOR/BRIDGE and ignores unknown
        // JSON enum names. Verify the wire value independently as well.
        assert.equal(p.kind, expected <= 4 ? expected : 0); assert.deepEqual(p.kindDetails, [0,1,0]);
        const admin = new AccessToken(key, secret); admin.addGrant({ room: roomName, roomAdmin: true });
        const response = await fetch(`${base}/twirp/livekit.RoomService/GetParticipant`, {
          method: 'POST', headers: { Authorization: `Bearer ${await admin.toJwt()}`, 'Content-Type': 'application/json' },
          body: JSON.stringify({ room: roomName, identity: `kind-${name}` }), signal: AbortSignal.timeout(10000),
        });
        assert.equal(response.status, 200); const wire = await response.json();
        assert.equal(wire.kind, name === 'unknown' ? 'STANDARD' : name.toUpperCase());
        assert.deepEqual(wire.kind_details, ['CLOUD_AGENT','FORWARDED','CLOUD_AGENT']);
        await count(1, false); await disconnect(page); await count(0);
      }
    });
    await check('dynamic recorder flag alone does not start activeRecording', async () => {
      const page = await client('dynamic-recorder');
      await api.updateParticipant(roomName, 'dynamic-recorder', { permission: permission({ recorder: true }) });
      await count(0, false); await disconnect(page); await count(0, false);
    });
    await api.deleteRoom(roomName);
    await closeContexts();

    roomName = 'departure'; await api.createRoom({ name: roomName, emptyTimeout: 60, departureTimeout: 3 });
    await check('dependent participants cannot keep room alive after human departure', async () => {
      const page = await client('departing-human'); const dependent = await client('remaining-agent', true, { agent: true });
      await disconnect(page);
      await eventually(async () => (await api.listRooms([roomName])).length === 0, 'departure timeout closes room', 12000);
      await eventually(() => dependent.evaluate(() => room.state === 'disconnected'), 'dependent is closed with room');
    });
    roomName = 'empty'; await api.createRoom({ name: roomName, emptyTimeout: 7, departureTimeout: 60 });
    await check('dependent-only room expires by emptyTimeout', async () => {
      const page = await client('only-recorder', true, { recorder: true }); await count(0, true);
      await eventually(async () => (await api.listRooms([roomName])).length === 0, 'empty timeout closes dependent-only room', 14000);
      await eventually(() => page.evaluate(() => room.state === 'disconnected'), 'recorder is closed with room');
    });
    record.completed = true;
  } catch (e) { record.failure = e.stack; console.error(`${kind}: ${e.stack}`); }
  finally {
    for (const c of contexts) await c.close();
    if (child.exitCode === null && child.signalCode === null) { const exit = once(child, 'exit'); child.kill(); await exit; }
    writeFileSync(resolve(out, `${kind}.log`), log); unlinkSync(configPath);
  }
}

try {
  assets.listen(0, '127.0.0.1'); await once(assets, 'listening');
  browser = await chromium.launch({ channel: 'chrome', headless: true, args: ['--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling'] });
  report.browser = browser.version();
  await exercise('go', opts['go-server']); await exercise('cpp', opts['cpp-server']);
  for (const expected of report.servers.go.cases) {
    const actual = report.servers.cpp.cases.find(c => c.name === expected.name);
    report.comparisons.push({ name: expected.name, passed: expected.passed && actual?.passed === true });
  }
  process.exitCode = report.servers.go.completed && report.servers.cpp.completed && report.comparisons.every(c => c.passed) ? 0 : 1;
} catch (e) { report.infrastructureError = e.stack; process.exitCode = 2; }
finally {
  await browser?.close(); await new Promise(r => assets.close(r)); report.exitCode = process.exitCode || 0;
  writeFileSync(resolve(out, 'report.json'), JSON.stringify(report, null, 2));
  console.log(`${report.comparisons.filter(c => c.passed).length}/${report.comparisons.length} matched; ${out}`);
}
