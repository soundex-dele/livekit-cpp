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
const out = resolve(opts.output || resolve(here, 'results/go-acl-parity'),
  new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, { recursive: true });
const report = { fullReplacementVerified: false, scope: 'media subscription ACL', servers: {}, comparisons: [] };
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
  const key = 'acltest', secret = randomBytes(32).toString('hex'), roomName = 'acl-parity';
  const p = await port(), base = `http://127.0.0.1:${p}`;
  const configPath = resolve(out, `${kind}.${kind === 'go' ? 'yaml' : 'json'}`);
  writeFileSync(configPath, kind === 'go'
    ? `port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 59400\n  port_range_end: 59580\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`
    : JSON.stringify({ bind: '127.0.0.1', port: p, keys: { [key]: secret }, rtc: { port_range_start: 59600, port_range_end: 59780 } }));
  const record = report.servers[kind] = { binary: resolve(binary), sha256: createHash('sha256').update(readFileSync(binary)).digest('hex'), cases: [] };
  const child = spawn(resolve(binary), ['--config', configPath], { windowsHide: true });
  let log = '', spawnError;
  child.stdout.on('data', b => { log += b; }); child.stderr.on('data', b => { log += b; });
  child.on('error', e => { spawnError = e; });
  const contexts = [];
  const api = new RoomServiceClient(base, key, secret);
  async function client(identity, autoSubscribe = true, grant = {}, participantKind) {
    const token = new AccessToken(key, secret, { identity, ttl: '15m' });
    if (participantKind) token.kind = participantKind;
    token.addGrant({ roomJoin: true, room: roomName, ...grant });
    const context = await browser.newContext(); contexts.push(context);
    const page = await context.newPage();
    page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') log += `[${identity}] ${m.text()}\n`; });
    page.on('pageerror', e => { log += `[${identity}] ${e.stack}\n`; });
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async ({ base, token, autoSubscribe }) => {
      const { Room, RoomEvent, Track } = LivekitClient;
      window.room = new Room({ adaptiveStream: false, dynacast: false, publishDefaults: { simulcast: false, videoCodec: 'vp8', red: false } });
      window.tracks = new Map(); window.received = []; window.permissions = [];
      const old = room.engine.client.onSubscriptionPermissionUpdate;
      room.engine.client.onSubscriptionPermissionUpdate = update => { permissions.push(update); old?.(update); };
      room.on(RoomEvent.TrackSubscribed, (track, pub) => {
        tracks.set(pub.trackSid, track); const el = track.attach(); el.autoplay = true; document.body.appendChild(el);
      });
      room.on(RoomEvent.TrackUnsubscribed, (track, pub) => { tracks.delete(pub.trackSid); track.detach().forEach(el => el.remove()); });
      room.on(RoomEvent.DataReceived, bytes => received.push(new TextDecoder().decode(bytes)));
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
  async function denied(page, sids, publisher = 'publisher') {
    await eventually(() => page.evaluate(({ sids, publisher }) => sids.every(sid =>
      room.remoteParticipants.get(publisher)?.getTrackPublicationBySid(sid)?.permissionStatus === 'not_allowed' && !tracks.has(sid)),
    { sids, publisher }), 'permission denied and tracks removed');
  }
  async function noMedia(page) {
    // Inspect transport byte counters as well as SDK events. Once renegotiation
    // drains, denied RTP must stop, not merely disappear from the SDK track map.
    const bytes = () => page.evaluate(async () => {
      let sum = 0;
      (await room.engine.pcManager.subscriber.pc.getStats()).forEach(s => { if (s.type === 'inbound-rtp') sum += s.bytesReceived || 0; });
      return sum;
    });
    await delay(400); const before = await bytes(); await delay(400);
    assert.equal(await bytes(), before, 'Denied RTP byte counters still advanced');
  }
  try {
    await eventually(async () => {
      if (spawnError || child.exitCode !== null) throw spawnError || new Error('Server exited');
      try { await api.listRooms(); return true; } catch { return false; }
    }, 'server ready', 30000);
    await api.createRoom({ name: roomName });
    const pub = await client('publisher'), bob = await client('bob'), carol = await client('carol');
    const video = await pub.evaluate(() => publishVideo()), audio = await pub.evaluate(() => publishAudio());
    const both = [video, audio];
    await check('default allow publishes audio and video', async () => { await media(bob, both); await media(carol, both); });
    await check('deny all revokes RTP without disconnecting publisher', async () => {
      await set(pub, false); await denied(bob, both); await denied(carol, both); await noMedia(bob);
      assert.equal(await pub.evaluate(() => room.state), 'connected');
    });
    await check('track ACL does not revoke reliable data', async () => {
      await pub.evaluate(() => room.localParticipant.publishData(new TextEncoder().encode('acl-data'), { reliable: true }));
      await eventually(() => bob.evaluate(() => received.includes('acl-data')), 'data while media denied');
    });
    await check('identity grants one track to one subscriber', async () => {
      await set(pub, false, [{ participantIdentity: 'bob', allowedTrackSids: [video] }]);
      await media(bob, [video]); await denied(bob, [audio]); await denied(carol, both);
    });
    await check('replacing ACL revokes old grants', async () => {
      await set(pub, false, [{ participantIdentity: 'carol', allowAll: true }]);
      await denied(bob, both); await noMedia(bob); await media(carol, both);
    });
    await check('all participants restores pending subscriptions', async () => {
      await set(pub, true); await media(bob, both); await media(carol, both);
    });
    const bobSid = await bob.evaluate(() => room.localParticipant.sid);
    await check('SID grants resolve to subscriber identity', async () => {
      await set(pub, false, [{ participantSid: bobSid, allowAll: true }]);
      await media(bob, both); await denied(carol, both);
    });
    await check('explicit identity takes precedence over mismatched SID', async () => {
      await set(pub, false, [{ participantIdentity: 'carol', participantSid: bobSid, allowAll: true }]);
      await denied(bob, both); await media(carol, both);
    });
    await check('invalid entry preserves previous ACL and connection', async () => {
      await raw(pub, false, [{ participantIdentity: 'bob', allTracks: true }, { allTracks: true }]);
      await delay(300); await media(carol, both); await denied(bob, both);
      assert.equal(await pub.evaluate(() => room.state), 'connected');
    });
    await check('duplicate identity uses last entry', async () => {
      await set(pub, false, [{ participantIdentity: 'bob', allowAll: true }, { participantIdentity: 'bob', allowedTrackSids: [audio] }]);
      await media(bob, [audio]); await denied(bob, [video]); await denied(carol, both);
    });
    await check('unknown SID does not grant access', async () => {
      await set(pub, false, [{ participantSid: 'PA_absent', allowAll: true }]);
      await denied(bob, both); await denied(carol, both); await noMedia(bob);
    });
    await check('all participants flag overrides malformed entries', async () => {
      await raw(pub, true, [{ allTracks: false }]); await media(bob, both); await media(carol, both);
    });
    let late;
    await check('identity grant can precede subscriber join', async () => {
      await set(pub, false, [{ participantIdentity: 'late', allowAll: true }]);
      late = await client('late'); await media(late, both); await denied(bob, both);
    });
    await check('explicit unsubscribe stays off across permission changes', async () => {
      await api.updateSubscriptions(roomName, 'late', [video], false);
      await eventually(() => late.evaluate(sid => !tracks.has(sid), video), 'explicit unsubscribe');
      await set(pub, false); await denied(late, [audio]);
      await set(pub, true); await media(late, [audio]); await delay(400);
      assert.equal(await late.evaluate(sid => tracks.has(sid), video), false);
    });
    await check('server subscription request cannot bypass publisher ACL', async () => {
      await set(pub, false); await denied(bob, both);
      await api.updateSubscriptions(roomName, 'bob', both, true); await delay(400);
      await denied(bob, both); await noMedia(bob);
    });
    await check('new publication does not inherit per-track access', async () => {
      await set(pub, false, [{ participantIdentity: 'bob', allowedTrackSids: [video] }]);
      await media(bob, [video]); const added = await pub.evaluate(() => publishVideo());
      await denied(bob, [added]);
      await set(pub, false, [{ participantIdentity: 'bob', allowedTrackSids: [video, added] }]);
      await media(bob, [video, added]);
    });
    let recorder;
    await check('recorder grant bypasses publisher ACL', async () => {
      await set(pub, false); recorder = await client('recorder', true, { recorder: true });
      await media(recorder, both); await denied(bob, both);
    });
    await check('recorder removal preserves existing subscriptions until ACL update', async () => {
      await api.updateParticipant(roomName, 'recorder', { permission: { canSubscribe: true, canPublish: true, canPublishData: true, recorder: false } });
      await delay(400); await media(recorder, both);
      await set(pub, false);
      await denied(recorder, both); await noMedia(recorder);
    });
    await check('dynamic recorder grant restores denied subscriptions', async () => {
      await api.updateParticipant(roomName, 'recorder', { permission: { canSubscribe: true, canPublish: true, canPublishData: true, recorder: true } });
      await media(recorder, both);
    });
    await check('egress kind bypasses publisher ACL without recorder flag', async () => {
      const egress = await client('egress', true, {}, 'EGRESS'); await media(egress, both);
    });
    await check('canSubscribe revocation removes even recorder subscriptions', async () => {
      await api.updateParticipant(roomName, 'recorder', { permission: { canSubscribe: false, canPublish: true, canPublishData: true, recorder: true } });
      await eventually(() => recorder.evaluate(sids => sids.every(sid => !tracks.has(sid)), both), 'recorder global unsubscribe');
      await noMedia(recorder);
      await api.updateParticipant(roomName, 'recorder', { permission: { canSubscribe: true, canPublish: true, canPublishData: true, recorder: true } });
      await media(recorder, both);
    });
    await check('publisher ACL does not affect another publisher', async () => {
      const otherVideo = await bob.evaluate(() => publishVideo());
      await media(carol, [otherVideo]); await denied(carol, both);
    });
    await check('SID grant remains identity based after session replacement', async () => {
      const lateSid = await late.evaluate(() => room.localParticipant.sid);
      await set(pub, false, [{ participantSid: lateSid, allowAll: true }]); await media(late, [audio]);
      await late.evaluate(() => room.disconnect());
      late = await client('late');
      assert.notEqual(await late.evaluate(() => room.localParticipant.sid), lateSid);
      await media(late, both);
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
