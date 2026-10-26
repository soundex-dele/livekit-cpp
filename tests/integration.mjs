import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { once } from 'node:events';
import { createHmac } from 'node:crypto';
import { AccessToken, RoomServiceClient } from 'livekit-server-sdk';
import { chromium } from 'playwright';
import { ListRoomsRequest, ListRoomsResponse } from '@livekit/protocol';

const here = dirname(fileURLToPath(import.meta.url));
const results = process.env.LK_TEST_RESULTS ? resolve(process.env.LK_TEST_RESULTS) : resolve(here, 'results');
mkdirSync(results, {recursive: true});
const port = Number(process.env.LK_TEST_PORT || 17880);
const externalServer = process.env.LK_EXTERNAL_SERVER === '1';
const forceRelay = process.env.LK_FORCE_RELAY === '1';
const turnTransport = process.env.LK_TURN_TRANSPORT;
const expectTurnRest = process.env.LK_EXPECT_TURN_REST === '1';
const resubscribeRounds = Number(process.env.LK_RESUBSCRIBE_ROUNDS || 1);
assert.ok(Number.isInteger(resubscribeRounds) && resubscribeRounds >= 1 && resubscribeRounds <= 20,
  'LK_RESUBSCRIBE_ROUNDS must be an integer in 1..20');
const issuedTurnUsernames = new Set();
const turnCredentialAudit = [];
const base = externalServer ? (process.env.LK_TEST_URL || `http://127.0.0.1:${port}`) : `http://127.0.0.1:${port}`;
const exe = process.env.LK_SERVER || resolve(here, process.platform === 'win32'
  ? '../out/windows-x64/Release/livekit-server.exe' : '../out/linux-x64/bin/livekit-server');
const child = externalServer ? null : spawn(exe, ['--dev', '--port', String(port)], {windowsHide: true});
let serverLog = ''; child?.stdout.on('data', b => { serverLog += b; }); child?.stderr.on('data', b => { serverLog += b; });
let spawnError; child?.on('error', e => { spawnError = e; });
const delay = ms => new Promise(r => setTimeout(r, ms));
async function eventually(fn, label, timeout = 20000) {
  const end = Date.now() + timeout; let last;
  while (Date.now() < end) {
    if (spawnError) throw spawnError;
    if (child && child.exitCode !== null) throw new Error(`Server exited ${child.exitCode}: ${serverLog}`);
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
let browser; let browserVersion; let output = []; let checks = 0;
function pass(text) { ++checks; output.push(text); console.log(`PASS: ${text}`); }
async function auditSfu() {
  const token = new AccessToken('devkey', 'secret'); token.addGrant({roomList:true});
  const text = await (await fetch(`${base}/metrics`, {headers:{Authorization:`Bearer ${await token.toJwt()}`}})).text();
  const metrics = Object.fromEntries(text.trim().split('\n').map(line => {
    const [name,value] = line.split(' '); return [name.replace(/^livekit_cpp_/, ''),Number(value)];
  }));
  assert.ok(metrics.sfu_rtp_forwarded > 0, 'SFU must forward encoded RTP');
  assert.ok(metrics.sfu_video_frames_assembled > 0 && metrics.sfu_video_frames_forwarded > 0,
    'SFU must assemble and repacketize video with native WebRTC');
  assert.ok(metrics.sfu_probe_packets > 0 && metrics.sfu_probe_feedback_packets > 0,
    'Native bandwidth probes must reach receiver feedback');
  assert.equal(metrics.video_encode_calls, 0, 'SFU must not encode video');
  assert.equal(metrics.video_decode_calls, 0, 'SFU must not decode video');
  assert.ok(metrics.sfu_audio_reserved_bps+metrics.sfu_video_allocated_bps<=metrics.sfu_media_budget_bps,
    'Per-transport media allocations must fit the shared budget');
  return metrics;
}
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
  browser = await chromium.launch({headless: true,
    ...(process.env.LK_CHROME ? {executablePath: process.env.LK_CHROME} : {channel: 'chrome'}),
    args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling',
      ...(process.env.LK_CHROME_RTC_LOG === '1'
        ? ['--enable-logging', `--log-file=${resolve(results, 'chrome-rtc.log')}`, '--vmodule=*/webrtc/*=1'] : [])]});
  browserVersion = browser.version();
  async function client(identity, grant = {}, autoSubscribe = true) {
    const context = await browser.newContext({permissions: ['camera', 'microphone']});
    const page = await context.newPage();
    page.on('console', msg => { if (msg.type() === 'error' || msg.type() === 'warning') output.push(`[${identity}] ${msg.text()}`); });
    page.on('pageerror', e => output.push(`[${identity}] ${e.stack}`));
    await page.goto(assetsUrl);
    await page.evaluate(async ({url, jwt, autoSubscribe, forceRelay}) => {
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
      await room.connect(url, jwt, {autoSubscribe, ...(forceRelay ? {rtcConfig:{iceTransportPolicy:'relay'}} : {})});
    }, {url: base.replace('http:', 'ws:'), jwt: await token(identity, grant), autoSubscribe, forceRelay});
    if (expectTurnRest) {
      const configured = await page.evaluate(() => ({sid:room.localParticipant.sid,
        servers:room.engine.pcManager.subscriber.pc.getConfiguration().iceServers}));
      const servers = configured.servers.filter(s => (Array.isArray(s.urls) ? s.urls : [s.urls]).some(url => /^turns?:/.test(url)));
      assert.equal(servers.length, 1, 'Expected exactly one server-issued TURN credential entry');
      const entry = servers[0];
      const expiresAt = Number(entry.username.split(':')[0]);
      assert.ok(entry.username === `${expiresAt}:${configured.sid}`, 'TURN username must contain only expiry and opaque participant SID');
      assert.ok(Math.abs(expiresAt-Math.floor(Date.now()/1000)-3600) < 30, 'Unexpected TURN expiry');
      const expected = createHmac('sha1', process.env.LK_TEST_COTURN_SECRET).update(entry.username).digest('base64');
      assert.ok(entry.credential === expected, 'Server TURN credential failed independent HMAC verification');
      assert.ok(entry.credential !== process.env.LK_TEST_COTURN_SECRET, 'Server exposed TURN signing secret');
      assert.ok(!issuedTurnUsernames.has(entry.username), 'Different joins reused TURN credentials');
      issuedTurnUsernames.add(entry.username);
      turnCredentialAudit.push({sid:configured.sid, expiresAt, hmacVerified:true});
    }
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
        stats?.forEach(s => { if (s.type === 'inbound-rtp') report.push({id:s.id, kind:s.kind, bytes:s.bytesReceived, frames:s.framesDecoded || 0, energy:s.totalAudioEnergy || 0, samples:s.totalSamplesReceived || 0}); });
      }
      return report;
    });
  }
  const stats = await eventually(async () => {
    const stats = await mediaStats(bob);
    return stats.some(s => s.kind === 'video' && s.frames > 5) && stats.some(s => s.kind === 'audio' && s.bytes > 500 && s.energy > 0.001) ? stats : false;
  }, 'decoded video frames and non-silent audio', 45000);
  pass(`native WebRTC packet SFU: ${JSON.stringify(stats)}`);
  await eventually(async () => {
    const metrics = await auditSfu();
    return metrics.sfu_downstream_feedback > 0 && metrics.sfu_feedback_packets > 0;
  }, 'native downstream congestion feedback');
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
  const simulcastSid = await alice.evaluate(async () => {
    const canvas = document.createElement('canvas'); canvas.width=1280; canvas.height=720;
    const ctx=canvas.getContext('2d'); document.body.appendChild(canvas); let frame=0;
    window.simulcastTimer=setInterval(() => {
      ctx.fillStyle=`hsl(${frame++ % 360},80%,50%)`; ctx.fillRect(0,0,1280,720);
      ctx.fillStyle='white'; ctx.font='60px sans-serif'; ctx.fillText(`simulcast ${frame}`,100,200);
    },66);
    const pub=await room.localParticipant.publishTrack(canvas.captureStream(15).getVideoTracks()[0], {
      name:'simulcast-check', source:LivekitClient.Track.Source.Camera, simulcast:true, videoCodec:'vp8',
      videoEncoding:{maxBitrate:1800000,maxFramerate:15},
      videoSimulcastLayers:[new LivekitClient.VideoPreset(320,180,150000,15),new LivekitClient.VideoPreset(640,360,500000,15)],
    });
    window.simulcastTrack=pub.track; return pub.trackSid;
  });
  const advertised=(await api.getParticipant('integration','alice')).tracks.find(t => t.sid===simulcastSid);
  assert.ok(advertised?.simulcast && advertised.codecs[0]?.layers.length===3,'Simulcast layers must be advertised');
  await api.updateSubscriptions('integration','late',[simulcastSid],true);
  await eventually(() => bob.evaluate(sid => tracks.has(sid),simulcastSid),'simulcast subscription');
  await eventually(() => late.evaluate(sid => tracks.has(sid),simulcastSid),'second simulcast subscription');
  const layerStats = page => page.evaluate(async sid => {
    const track=tracks.get(sid); const stats=await track?.getRTCStatsReport();
    return stats ? [...stats.values()].filter(s => s.type==='inbound-rtp' && s.kind==='video')
      .map(s => ({width:s.frameWidth,height:s.frameHeight,frames:s.framesDecoded||0,ssrc:s.ssrc})) : [];
  },simulcastSid);
  const quality = (page,level) => page.evaluate(({sid,level}) =>
    room.remoteParticipants.get('alice').getTrackPublicationBySid(sid).setVideoQuality(LivekitClient.VideoQuality[level]),
    {sid:simulcastSid,level});
  const resolution = async (page,width,height,after=0) => { const started=Date.now(); return eventually(async () => {
    const rows=await layerStats(page);
    const found=rows.find(s => s.width===width && s.height===height && s.frames>after+3);
    if (found) output.push(`Layer decoded: ${width}x${height} after ${Date.now()-started} ms, frames=${found.frames}`);
    return found || false;
  },`simulcast decoded ${width}x${height}`,30000); };
  const audioBefore=(await mediaStats(bob)).find(s => s.kind==='audio').samples;
  await quality(bob,'LOW'); await quality(late,'HIGH');
  const low=await resolution(bob,320,180); await resolution(late,1280,720);
  await quality(bob,'HIGH'); const high=await resolution(bob,1280,720,low.frames);
  assert.equal(high.ssrc,low.ssrc,'Layer switching must keep the subscriber SSRC');
  await bob.evaluate(sid => room.remoteParticipants.get('alice').getTrackPublicationBySid(sid).setVideoDimensions({width:600,height:300}),simulcastSid);
  const middle=await resolution(bob,640,360,high.frames);
  assert.equal(middle.ssrc,high.ssrc);
  await resolution(late,1280,720);
  await eventually(async () => (await mediaStats(bob)).some(s => s.kind==='audio' && s.samples>audioBefore+48000),'audio continues while switching');
  for (let round=0; round<resubscribeRounds; ++round) {
    const before=await mediaStats(bob);
    await api.updateSubscriptions('integration','bob',[simulcastSid],false);
    await eventually(() => bob.evaluate(sid => !tracks.has(sid),simulcastSid),'simulcast unsubscribe');
    await api.updateSubscriptions('integration','bob',[simulcastSid],true);
    await resolution(bob,640,360);
    await eventually(async () => {
      const after=await mediaStats(bob);
      return before.some(old => old.kind==='audio' && after.some(row => row.id===old.id && row.samples>old.samples))
        && before.some(old => old.kind==='video' && after.some(row => row.id===old.id && row.frames>old.frames));
    },`existing audio/video continue after resubscription ${round+1}`);
    output.push(`Simulcast resubscription ${round+1}/${resubscribeRounds} decoded; existing audio/video advanced`);
  }
  await quality(bob,'HIGH'); await resolution(bob,1280,720);
  await alice.evaluate(async () => {
    const sender=simulcastTrack.sender; const params=sender.getParameters();
    if (params.encodings.length!==3) throw new Error('Expected three native simulcast encodings');
    params.encodings.find(e => e.rid==='f').active=false; await sender.setParameters(params);
  });
  await resolution(bob,640,360); await resolution(late,640,360);
  await alice.evaluate(async () => {
    const sender=simulcastTrack.sender; const params=sender.getParameters();
    params.encodings.find(e => e.rid==='f').active=true; await sender.setParameters(params);
  });
  await resolution(bob,1280,720); await resolution(late,1280,720);
  const layerMetrics=await auditSfu(); assert.ok(layerMetrics.sfu_layer_switches>=2);
  assert.ok(layerMetrics.sfu_audio_reserved_bps>=64000 && layerMetrics.sfu_video_allocated_bps>0,
    'Real audio/video subscriptions must participate in bandwidth allocation');
  output.push(`Simulcast audit: ${JSON.stringify({low,high,middle,layerMetrics})}`);
  await alice.evaluate(async () => { clearInterval(simulcastTimer); await room.localParticipant.unpublishTrack(simulcastTrack); });
  pass('three-layer VP8 simulcast: independent subscriptions, LOW/HIGH/dimensions, audio, resubscription and inactive-layer recovery');
  if (forceRelay) {
    const routes = [];
    for (const [page, minimum] of [[alice,2],[bob,2],[late,1]]) {
      const selected = await page.evaluate(async () => {
        const result = [];
        for (const role of ['publisher','subscriber']) {
          const pc = room.engine.pcManager[role]?.pc;
          if (!pc || pc.connectionState !== 'connected') continue;
          const stats = await pc.getStats();
          for (const transport of stats.values()) if (transport.type === 'transport' && transport.selectedCandidatePairId) {
            const pair = stats.get(transport.selectedCandidatePairId);
            const candidate = stats.get(pair.localCandidateId);
            result.push({identity:room.localParticipant.identity, role, state:pair.state,
              candidateType:candidate.candidateType, relayProtocol:candidate.relayProtocol,
              bytesSent:pair.bytesSent, bytesReceived:pair.bytesReceived});
          }
        }
        return result;
      });
      assert.ok(selected.length >= minimum, 'Every active media connection must expose its selected route');
      for (const route of selected) {
        assert.equal(route.state, 'succeeded');
        assert.equal(route.candidateType, 'relay', 'Relay-only test must actually use TURN');
        if (turnTransport) assert.equal(route.relayProtocol, turnTransport);
        assert.ok(route.bytesSent > 0 && route.bytesReceived > 0);
      }
      routes.push(...selected);
    }
    output.push(`TURN selected routes: ${JSON.stringify(routes)}`);
    writeFileSync(resolve(results, 'turn-routes.json'), JSON.stringify(routes, null, 2));
    pass(`TURN-only media uses verified ${turnTransport || 'relay'} candidates`);
  }
  if (expectTurnRest) {
    const good = await alice.evaluate(() => room.engine.pcManager.subscriber.pc.getConfiguration().iceServers
      .find(s => (Array.isArray(s.urls) ? s.urls : [s.urls]).some(url => /^turns?:/.test(url))));
    const expiredUsername = `${Math.floor(Date.now()/1000)-120}:PA_expired`;
    const expiredPassword = createHmac('sha1',process.env.LK_TEST_COTURN_SECRET).update(expiredUsername).digest('base64');
    const denied = [];
    for (const [label, credentials] of [['wrong-password',{...good,credential:'invalid-rest-credential'}],
      ['expired',{...good,username:expiredUsername,credential:expiredPassword}]]) {
      const result = await alice.evaluate(async iceServer => {
        const pc = new RTCPeerConnection({iceServers:[iceServer],iceTransportPolicy:'relay'});
        const result = {relayCandidates:0,errorCodes:[],complete:false};
        let timer;
        const gathered = new Promise(resolve => {
          pc.onicecandidate = event => { if (event.candidate?.type === 'relay') result.relayCandidates++; };
          pc.onicecandidateerror = event => result.errorCodes.push(event.errorCode);
          pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') { result.complete=true; resolve(); } };
          timer = setTimeout(resolve,15000);
        });
        try {
          pc.createDataChannel('credential-validation');
          await pc.setLocalDescription(await pc.createOffer());
          await gathered;
          return result;
        } finally { clearTimeout(timer); pc.close(); }
      }, credentials);
      assert.ok(result.complete && result.relayCandidates===0 && result.errorCodes.length>0,
        `coturn must reject ${label} credentials with completed ICE gathering and no relay candidate`);
      denied.push({label,...result});
    }
    writeFileSync(resolve(results,'turn-auth-denials.json'),JSON.stringify(denied,null,2));
    pass('coturn REST authentication rejects wrong and expired credentials');
  }
  if (process.env.LK_IMPAIRMENT_URL) {
    assert.ok(forceRelay && turnTransport === 'udp', 'Outage harness requires verified TURN/UDP-only media');
    const control = async action => {
      const response = await fetch(`${process.env.LK_IMPAIRMENT_URL}/${action}`, {
        method:'POST', headers:{Authorization:`Bearer ${process.env.LK_IMPAIRMENT_TOKEN}`}});
      assert.ok(response.ok, 'UDP impairment controller failed');
      return response.json();
    };
    const before = await mediaStats(bob);
    const started = await control('outage');
    assert.ok(started.active && started.duration_seconds === 5);
    await delay(3000);
    const during = await mediaStats(bob);
    const outage = await eventually(async () => {
      const state = await control('status');
      return !state.active ? state : false;
    }, 'five-second UDP outage completes', 10000);
    assert.ok(outage.elapsed_seconds >= 5 && outage.dropped.upstream > 0 && outage.dropped.downstream > 0);
    assert.deepEqual(outage.errors, []);
    const released = await mediaStats(bob);
    const frames = rows => rows.filter(s => s.kind === 'video').reduce((n,s) => n+s.frames,0);
    assert.ok(frames(released) <= frames(during)+2, 'Video must actually stall while all TURN datagrams are dropped');
    const baseline = new Map(released.map(s => [s.id,s]));
    const recoveryStarted = performance.now();
    const recovered = await eventually(async () => {
      const rows = await mediaStats(bob);
      const video = rows.some(s => s.kind === 'video' && s.frames > (baseline.get(s.id)?.frames || 0)+5);
      const audio = rows.some(s => s.kind === 'audio' && s.bytes > (baseline.get(s.id)?.bytes || 0)+500
        && s.energy > (baseline.get(s.id)?.energy || 0)+.001);
      return video && audio ? rows : false;
    }, 'decoded video and audible audio recover after five-second outage', 10000);
    const recoveryMs = Math.round(performance.now()-recoveryStarted);
    const finished = await control('status');
    const recoveryRawMs = Math.round((finished.elapsed_seconds-finished.duration_seconds)*1000);
    assert.ok(recoveryRawMs <= 10000, 'Recovery must also fit ten seconds on the unadjusted Linux clock');
    const recovery = {outage, recoveryMs, recoveryRawMs, before, during, released, recovered};
    writeFileSync(resolve(results, 'recovery.json'), JSON.stringify(recovery, null, 2));
    output.push(`UDP outage recovery: ${JSON.stringify(recovery)}`);
    pass(`Five-second bidirectional UDP loss: decoded audio/video recovered in ${recoveryRawMs} ms (unadjusted clock)`);
  }
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
  const finalMetrics = await auditSfu();
  assert.equal(finalMetrics.sfu_pacer_queue_bytes, 0, 'Closed rooms must release paced packets');
  assert.equal(finalMetrics.sfu_target_bps, 0, 'Closed rooms must release congestion controllers');
  assert.equal(finalMetrics.sfu_media_budget_bps, 0, 'Closed rooms must release allocation tasks');
  assert.equal(finalMetrics.sfu_video_budget_paused, 0, 'Closed rooms must release paused outputs');
  output.push(`SFU audit: ${JSON.stringify(finalMetrics)}`);
  pass('permissions, duplicate identity, participant removal and room deletion');
  console.log(`PASS: ${checks} integration checks`);
} catch (e) {
  output.push(`FAIL: ${e.stack}`); console.error(e); process.exitCode = 1;
  try {
    const diagnosticToken = new AccessToken('devkey', 'secret'); diagnosticToken.addGrant({roomList:true});
    const metrics = await (await fetch(`${base}/metrics`, {headers:{Authorization:`Bearer ${await diagnosticToken.toJwt()}`}})).text();
    output.push(metrics); console.error(metrics);
    for (const context of browser?.contexts() || []) for (const page of context.pages()) {
      const diagnostics = await page.evaluate(async () => {
        const result = [];
        for (const track of window.tracks?.values() || []) {
          const report = await track.getRTCStatsReport();
          report?.forEach(s => { if (s.type === 'inbound-rtp') result.push(s); });
        }
        const pc = window.room?.engine?.pcManager?.subscriber?.pc;
        const transportStats = pc ? [...(await pc.getStats()).values()]
          .filter(s => s.type === 'transport')
          .map(s => ({bytesSent:s.bytesSent,bytesReceived:s.bytesReceived,dtlsState:s.dtlsState})) : [];
        const pub = window.room?.engine?.pcManager?.publisher?.pc;
        const outbound = pub ? [...(await pub.getStats()).values()].filter(s => s.type === 'outbound-rtp' || s.type === 'media-source') : [];
        return {identity:window.room?.localParticipant.identity,stats:result,transportStats,outbound};
      });
      output.push(JSON.stringify(diagnostics)); console.error(JSON.stringify(diagnostics));
    }
  } catch (diagnosticError) { output.push(`Diagnostics unavailable: ${diagnosticError.message}`); }
} finally {
  try {
    if (browser) await browser.close();
  } catch (error) {
    output.push(`Browser cleanup failed: ${error.message}`); process.exitCode = 1;
  }
  assets.close();
  if (child && !spawnError && child.exitCode === null && child.signalCode === null) {
    const exited = once(child, 'exit');
    child.kill();
    const timeout = setTimeout(() => {
      output.push('Server failed to exit within 10 seconds'); process.exitCode = 1;
      child.kill('SIGKILL');
    }, 10000);
    try { await exited; } finally { clearTimeout(timeout); }
  }
  if (child && process.platform !== 'win32' && child.exitCode !== 0) {
    output.push(`Server shutdown failed: exit=${child.exitCode}, signal=${child.signalCode}`); process.exitCode = 1;
  }
  writeFileSync(resolve(results, 'server.log'), serverLog);
  writeFileSync(resolve(results, 'integration.log'), output.join('\n') + '\n');
  if (expectTurnRest) writeFileSync(resolve(results,'turn-credentials.json'),JSON.stringify(turnCredentialAudit,null,2));
  writeFileSync(resolve(results, 'result.json'), JSON.stringify({ok:process.exitCode !== 1, checks,
    server:base, externalServer, serverExitCode:child?.exitCode, serverSignal:child?.signalCode,
    forceRelay, turnTransport, expectTurnRest, resubscribeRounds, platform:process.platform, arch:process.arch,
    nodeVersion:process.version, browserVersion}, null, 2));
}
