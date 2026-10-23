import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import { AccessToken, RoomServiceClient } from 'livekit-server-sdk';
import { chromium } from 'playwright';
import { ListRoomsRequest, ListRoomsResponse } from '@livekit/protocol';

const here = dirname(fileURLToPath(import.meta.url));
const results = resolve(here, 'results'); mkdirSync(results, {recursive: true});
const port = Number(process.env.LK_TEST_PORT || 17880);
const base = `http://127.0.0.1:${port}`;
const exe = process.env.LK_SERVER || resolve(here, '../out/windows-x64/Release/livekit-server.exe');
const child = spawn(exe, ['--dev', '--port', String(port)], {windowsHide: true});
let serverLog = ''; child.stdout.on('data', b => { serverLog += b; }); child.stderr.on('data', b => { serverLog += b; });
let spawnError; child.on('error', e => { spawnError = e; });
const delay = ms => new Promise(r => setTimeout(r, ms));
async function eventually(fn, label, timeout = 20000) {
  const end = Date.now() + timeout; let last;
  while (Date.now() < end) {
    if (spawnError) throw spawnError;
    if (child.exitCode !== null) throw new Error(`Server exited ${child.exitCode}: ${serverLog}`);
    try { const result = await fn(); if (result) return result; } catch (e) { last = e; }
    await delay(100);
  }
  throw new Error(`Timed out: ${label}; ${last || ''}`);
}
async function token(identity, grant = {}) {
  const access = new AccessToken('devkey', 'secret', {identity, ttl: '10m'});
  access.addGrant({roomJoin: true, room: 'integration', canUpdateOwnMetadata: true, ...grant});
  return access.toJwt();
}
const script = readFileSync(resolve(here, 'node_modules/livekit-client/dist/livekit-client.umd.js'));
const assets = createServer((req, res) => {
  if (req.url === '/sdk.js') { res.writeHead(200, {'content-type': 'text/javascript'}); res.end(script); }
  else { res.writeHead(200, {'content-type': 'text/html'}); res.end('<!doctype html><html><head><title>LiveKit C++ integration</title></head><body><script src="/sdk.js"></script></body></html>'); }
});
let browser; let output = [];
function pass(text) { output.push(text); console.log(`PASS: ${text}`); }
try {
  await eventually(async () => (await fetch(`${base}/healthz`)).ok, 'server startup', 60000);
  assets.listen(0, '127.0.0.1'); await once(assets, 'listening');
  const assetsUrl = `http://127.0.0.1:${assets.address().port}`;
  const api = new RoomServiceClient(base, 'devkey', 'secret');
  const r = await api.createRoom({name: 'integration', metadata: 'created-by-test', departureTimeout: 2});
  assert.match(r.sid, /^RM_/); assert.equal((await api.listRooms(['integration'])).length, 1);
  assert.equal((await fetch(`${base}/rtc/validate`)).status, 401);
  const invalid = (await token('bad')).split('.'); invalid[2] = 'A'.repeat(invalid[2].length);
  assert.equal((await fetch(`${base}/rtc/validate?access_token=${invalid.join('.')}`)).status, 401);
  const adminToken = new AccessToken('devkey', 'secret'); adminToken.addGrant({roomList:true});
  const binaryResponse = await fetch(`${base}/twirp/livekit.RoomService/ListRooms`, {
    method:'POST', headers:{Authorization:`Bearer ${await adminToken.toJwt()}`, 'Content-Type':'application/protobuf'},
    body:new ListRoomsRequest({names:['integration']}).toBinary(),
  });
  assert.equal(binaryResponse.status, 200);
  assert.equal(ListRoomsResponse.fromBinary(new Uint8Array(await binaryResponse.arrayBuffer())).rooms[0].sid, r.sid);
  pass('official server SDK room creation/listing and JWT rejection');
  browser = await chromium.launch({headless: true, channel: 'chrome', args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling']});
  async function client(identity, grant = {}, autoSubscribe = true) {
    const context = await browser.newContext({permissions: ['camera', 'microphone']});
    const page = await context.newPage();
    page.on('console', msg => { if (msg.type() === 'error' || msg.type() === 'warning') output.push(`[${identity}] ${msg.text()}`); });
    page.on('pageerror', e => output.push(`[${identity}] ${e.stack}`));
    await page.goto(assetsUrl);
    await page.evaluate(async ({url, jwt, autoSubscribe}) => {
      const {Room, RoomEvent} = LivekitClient;
      window.events = []; window.received = []; window.tracks = new Map();
      window.room = new Room({adaptiveStream: false, dynacast: false, publishDefaults: {simulcast: false, videoCodec: 'vp8', red: false}});
      window.publishVideo = async () => {
        const canvas = document.createElement('canvas'); canvas.width = 320; canvas.height = 240;
        document.body.appendChild(canvas); const ctx = canvas.getContext('2d'); let frame = 0;
        setInterval(() => { ctx.fillStyle = `hsl(${frame++ % 360},80%,50%)`; ctx.fillRect(0,0,320,240); ctx.fillStyle = 'white'; ctx.font = '30px sans-serif'; ctx.fillText(String(frame),40,80); }, 66);
        await room.localParticipant.publishTrack(canvas.captureStream(15).getVideoTracks()[0], {source:LivekitClient.Track.Source.Camera, simulcast:false, videoCodec:'vp8'});
      };
      room.on(RoomEvent.TrackSubscribed, (track, pub, participant) => {
        tracks.set(pub.trackSid, track); events.push({type: 'track', kind: track.kind, sid: pub.trackSid, identity: participant.identity});
        const el = track.attach(); el.autoplay = true; document.body.appendChild(el);
      });
      room.on(RoomEvent.TrackUnsubscribed, (_track, pub) => { tracks.delete(pub.trackSid); events.push({type:'unsubscribed', sid:pub.trackSid}); });
      room.on(RoomEvent.DataReceived, (data, participant, kind, topic) => received.push({text: new TextDecoder().decode(data), identity: participant?.identity, topic}));
      room.on(RoomEvent.Disconnected, reason => events.push({type: 'disconnected', reason}));
      room.on(RoomEvent.ParticipantMetadataChanged, (metadata, participant) => events.push({type:'metadata', metadata, identity:participant.identity}));
      await room.connect(url, jwt, {autoSubscribe});
    }, {url: base.replace('http:', 'ws:'), jwt: await token(identity, grant), autoSubscribe});
    return page;
  }
  const alice = await client('alice'); const bob = await client('bob');
  assert.equal((await api.listParticipants('integration')).length, 2);
  pass('two browser SDK clients join over binary signaling and ICE/DTLS');
  await alice.evaluate(async () => {
    await publishVideo();
    window.audioContext = new AudioContext();
    const oscillator = audioContext.createOscillator(); oscillator.frequency.value = 440;
    const gain = audioContext.createGain(); gain.gain.value = 0.1;
    const destination = audioContext.createMediaStreamDestination();
    oscillator.connect(gain).connect(destination); oscillator.start(); await audioContext.resume();
    await room.localParticipant.publishTrack(destination.stream.getAudioTracks()[0], {source:LivekitClient.Track.Source.Microphone, red:false});
  });
  await eventually(() => bob.evaluate(() => [...tracks.values()].some(t => t.kind === 'audio') && [...tracks.values()].some(t => t.kind === 'video')), 'audio/video subscriptions');
  async function mediaStats(page) {
    return page.evaluate(async () => {
      const report = [];
      for (const t of tracks.values()) {
        const stats = await t.getRTCStatsReport();
        stats?.forEach(s => { if (s.type === 'inbound-rtp') report.push({kind:s.kind, bytes:s.bytesReceived, frames:s.framesDecoded || 0, energy:s.totalAudioEnergy || 0, samples:s.totalSamplesReceived || 0}); });
      }
      return report;
    });
  }
  const stats = await eventually(async () => {
    const stats = await mediaStats(bob);
    return stats.some(s => s.kind === 'video' && s.frames > 5) && stats.some(s => s.kind === 'audio' && s.bytes > 500 && s.energy > 0.001) ? stats : false;
  }, 'decoded video frames and non-silent audio', 45000);
  pass(`native WebRTC media relay: ${JSON.stringify(stats)}`);
  await alice.evaluate(async () => { await room.localParticipant.publishData(new TextEncoder().encode('reliable-hello'), {reliable:true, topic:'test'}); });
  await eventually(() => bob.evaluate(() => received.some(d => d.text === 'reliable-hello' && d.identity === 'alice' && d.topic === 'test')), 'reliable data');
  await bob.evaluate(async () => { await room.localParticipant.publishData(new TextEncoder().encode('lossy-hello'), {reliable:false}); });
  await eventually(() => alice.evaluate(() => received.some(d => d.text === 'lossy-hello' && d.identity === 'bob')), 'lossy data');
  pass('reliable and lossy data channel fanout with authenticated sender identity');
  await api.sendData('integration', new TextEncoder().encode('from-server'), 0, {destinationIdentities:['bob'], topic:'server'});
  await eventually(() => bob.evaluate(() => received.some(d => d.text === 'from-server')), 'server data');
  assert.equal(await alice.evaluate(() => received.some(d => d.text === 'from-server')), false);
  await alice.evaluate(() => room.localParticipant.setMetadata('alice-updated'));
  await eventually(() => bob.evaluate(() => room.remoteParticipants.get('alice')?.metadata === 'alice-updated'), 'metadata update');
  await api.updateRoomMetadata('integration', 'updated-room');
  await eventually(() => alice.evaluate(() => room.metadata === 'updated-room'), 'room metadata');
  pass('targeted SendData, participant metadata and room metadata');
  const info = await api.getParticipant('integration', 'alice');
  const video = info.tracks.find(t => t.type === 1); assert.ok(video);
  await api.mutePublishedTrack('integration', 'alice', video.sid, true);
  await eventually(() => bob.evaluate(sid => room.remoteParticipants.get('alice')?.getTrackPublicationBySid(sid)?.isMuted, video.sid), 'mute');
  await api.mutePublishedTrack('integration', 'alice', video.sid, false);
  await eventually(() => bob.evaluate(sid => !room.remoteParticipants.get('alice')?.getTrackPublicationBySid(sid)?.isMuted, video.sid), 'unmute');
  await api.updateSubscriptions('integration', 'bob', [video.sid], false);
  await eventually(() => bob.evaluate(sid => !tracks.has(sid), video.sid), 'unsubscribe');
  await api.updateSubscriptions('integration', 'bob', [video.sid], true);
  await eventually(() => bob.evaluate(sid => tracks.has(sid), video.sid), 'resubscribe');
  await eventually(async () => (await mediaStats(bob)).some(s => s.kind === 'video' && s.frames > 5), 'video decoding after resubscribe');
  pass('mute/unmute and unsubscribe/resubscribe with SDP renegotiation');
  const late = await client('late', {}, false);
  assert.equal(await late.evaluate(() => tracks.size), 0);
  await api.updateSubscriptions('integration', 'late', [video.sid], true);
  await eventually(async () => (await mediaStats(late)).some(s => s.kind === 'video' && s.frames > 5), 'late explicit subscription');
  await bob.evaluate(() => publishVideo());
  await eventually(async () => (await mediaStats(alice)).some(s => s.kind === 'video' && s.frames > 5), 'reverse video publication');
  pass('late join with autoSubscribe=false and reverse-direction video publication');
  const denied = await client('denied', {canPublish:false, canPublishData:false, canSubscribe:false});
  assert.equal(await denied.evaluate(() => room.localParticipant.permissions.canPublish), false);
  assert.equal(await denied.evaluate(() => tracks.size), 0);
  await api.updateParticipant('integration', 'bob', undefined, {canPublish:false, canPublishData:false, canSubscribe:true});
  await eventually(() => alice.evaluate(() => tracks.size === 0), 'publication permission revocation');
  const dup = await client('alice');
  await eventually(() => alice.evaluate(() => room.state === 'disconnected'), 'duplicate identity replacement');
  await api.removeParticipant('integration', 'bob');
  await eventually(() => bob.evaluate(() => room.state === 'disconnected'), 'participant removal');
  await api.deleteRoom('integration');
  await eventually(() => dup.evaluate(() => room.state === 'disconnected'), 'room deletion');
  assert.equal((await api.listRooms(['integration'])).length, 0);
  pass('permissions, duplicate identity, participant removal and room deletion');
  console.log(`PASS: ${output.filter(s => !s.startsWith('[')).length} integration checks`);
} catch (e) {
  output.push(`FAIL: ${e.stack}`); console.error(e); process.exitCode = 1;
} finally {
  if (browser) await browser.close();
  assets.close(); child.kill();
  writeFileSync(resolve(results, 'server.log'), serverLog);
  writeFileSync(resolve(results, 'integration.log'), output.join('\n') + '\n');
}
