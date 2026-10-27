// Same-host, headless official SDK media checks against pinned Go and C++.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { createServer as tcpServer, createConnection } from 'node:net';
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
const { SignalRequest, SignalResponse } = require('@livekit/protocol');
const { AccessToken, RoomServiceClient } = require('livekit-server-sdk');
const out = resolve(opts.output || resolve(here, 'results/go-resume-parity'),
  new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, { recursive: true });
const report = { fullReplacementVerified: false, scope: 'dual-PC signal resume, media continuity, SyncState and stale socket ownership', servers: {}, comparisons: [] };
report.sourceHashes = Object.fromEntries(['include/lk/rooms.hpp','include/lk/rtc.hpp','src/rooms/rooms.cpp',
  'src/rtc/rtc.cpp','src/transport/http.cpp','tests/unit/core_test.cpp','tests/go_resume_parity.mjs'].map(file=>
  [file,createHash('sha256').update(readFileSync(resolve(here,'..',file))).digest('hex')]));
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

// A byte-transparent TCP relay controls only HTTP/WebSocket signaling. ICE/DTLS
// media bypasses it. Held new TCP connections retain bytes until release.
async function signalProxy(upstreamPort) {
  let paused = false; const sockets = new Set(), pending = new Set();
  function connect(client) {
    if (client.destroyed) return;
    pending.delete(client);
    const upstream = createConnection({host:'127.0.0.1',port:upstreamPort}); sockets.add(upstream);
    upstream.on('error',()=>client.destroy()); upstream.on('close',()=>{sockets.delete(upstream);client.destroy();});
    client.on('close',()=>upstream.destroy());
    client.pipe(upstream); upstream.pipe(client); client.resume();
  }
  const server=tcpServer(client=>{
    sockets.add(client); client.pause(); client.on('error',()=>{});
    client.on('close',()=>{sockets.delete(client);pending.delete(client);});
    if (paused) pending.add(client); else connect(client);
  });
  server.listen(0,'127.0.0.1'); await once(server,'listening');
  return {port:server.address().port,
    cut(){ paused=true; for(const socket of [...sockets]) socket.destroy(); },
    release(){ paused=false; for(const client of [...pending]) connect(client); },
    async close(){ for(const socket of [...sockets]) socket.destroy(); await new Promise(r=>server.close(r)); },
  };
}

let browser;
async function exercise(kind, binary) {
  const key = 'resumetest', secret = randomBytes(32).toString('hex'), roomName = 'resume-parity';
  const p = await port(), base = `http://127.0.0.1:${p}`;
  const configPath = resolve(out, `${kind}.${kind === 'go' ? 'yaml' : 'json'}`);
  writeFileSync(configPath, kind === 'go'
    ? `port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 60200\n  port_range_end: 60380\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`
    : JSON.stringify({ bind: '127.0.0.1', port: p, keys: { [key]: secret }, rtc: { port_range_start: 60400, port_range_end: 60580 } }));
  const record = report.servers[kind] = { binary: resolve(binary), sha256: createHash('sha256').update(readFileSync(binary)).digest('hex'), cases: [] };
  const child = spawn(resolve(binary), ['--config', configPath], { windowsHide: true });
  let log = '', spawnError;
  child.stdout.on('data', b => { log += b; }); child.stderr.on('data', b => { log += b; });
  child.on('error', e => { spawnError = e; });
  const contexts = [], proxies = []; const clients = new Map();
  const api = new RoomServiceClient(base, key, secret);
  async function client(identity, autoSubscribe = true, grant = {}, participantKind) {
    const token = new AccessToken(key, secret, { identity, ttl: '15m' });
    if (participantKind) token.kind = participantKind;
    token.addGrant({ roomJoin: true, room: roomName, ...grant });
    const proxy = await signalProxy(p); proxies.push(proxy);
    const context = await browser.newContext(); contexts.push(context);
    const page = await context.newPage();
    const state = { proxy, identity, token: await token.toJwt(), sockets: [] }; clients.set(page, state);
    page.on('websocket', socket => {
      const frames = []; state.sockets.push(frames);
      socket.on('framereceived', ({payload}) => {
        try {
          const r = typeof payload === 'string' ? SignalResponse.fromJsonString(payload) : SignalResponse.fromBinary(payload);
          const c = r.message.case, value = r.message.value;
          frames.push({type:c,...(c === 'offer' ? {ufrag:value.sdp.match(/a=ice-ufrag:([^\r\n]+)/)?.[1]} : {}),
            ...(c === 'leave' ? {reason:value.reason,action:value.action} : {})});
        } catch (e) { state.decodeError = e.stack; }
      });
    });
    page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') log += `[${identity}] ${m.text()}\n`; });
    page.on('pageerror', e => { log += `[${identity}] ${e.stack}\n`; });
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async ({ base, token, autoSubscribe }) => {
      const { Room, RoomEvent, Track } = LivekitClient;
      window.room = new Room({ adaptiveStream: false, dynacast: false, publishDefaults: { simulcast: false, videoCodec: 'vp8', red: false } });
      window.tracks = new Map(); window.received = []; window.permissions = []; window.roomEvents = [];
      for (const event of [RoomEvent.Reconnecting, RoomEvent.Reconnected, RoomEvent.Disconnected]) room.on(event, () => roomEvents.push(event));
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
      await room.connect(base.replace('http:', 'ws:'), token, { autoSubscribe, websocketTimeout:45000, rtcConfig: { iceServers: [] } });
      window.originalPCs = [room.engine.pcManager.publisher.pc,room.engine.pcManager.subscriber.pc];
    }, { base: `http://127.0.0.1:${proxy.port}`, token: state.token, autoSubscribe });
    return page;
  }
  async function check(name, fn) {
    const started = Date.now();
    try { const evidence=await fn(); record.cases.push({ name, passed: true, elapsedMs: Date.now() - started, evidence }); console.log(`${kind}: PASS ${name}`); }
    catch (e) { record.cases.push({ name, passed: false, failure: e.stack }); throw e; }
  }
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

  const sid = page => page.evaluate(()=>room.localParticipant.sid);
  const samePCs = page => page.evaluate(()=>originalPCs[0]===room.engine.pcManager.publisher.pc && originalPCs[1]===room.engine.pcManager.subscriber.pc);
  const permission = extra => ({canPublish:true,canSubscribe:true,canPublishData:true,canUpdateMetadata:true,...extra});
  async function cut(page) {
    const c=clients.get(page); c.sid=await sid(page); c.beforeSockets=c.sockets.length;
    c.proxy.cut(); await delay(600); return c;
  }
  async function resume(page, c) {
    c.proxy.release();
    await eventually(()=>c.sockets.length>c.beforeSockets && c.sockets.at(-1).some(f=>f.type==='reconnect'), 'ReconnectResponse',20000);
    await eventually(()=>page.evaluate(()=>room.state==='connected' && room.engine.client.currentState===1), 'SDK connected after resume',20000);
    assert.equal(await sid(page),c.sid); assert.equal(await samePCs(page),true);
    assert.equal(c.sockets.at(-1)[0]?.type,'reconnect','ReconnectResponse must precede replay/SDP');
    assert.equal(c.decodeError,undefined);
  }
  async function raw(identity, reconnect) {
    const token=new AccessToken(key,secret,{identity,ttl:'10m'}); token.addGrant({roomJoin:true,room:roomName});
    const context=await browser.newContext(); contexts.push(context); const page=await context.newPage();
    const frames=[];
    page.on('websocket',ws=>ws.on('framereceived',({payload})=>frames.push(typeof payload==='string'?SignalResponse.fromJsonString(payload):SignalResponse.fromBinary(payload))));
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(url=>{window.rawSocket=new WebSocket(url);},`${base.replace('http:','ws:')}/rtc?access_token=${await token.toJwt()}&protocol=16&sdk=js&version=2.15.6${reconnect?'&reconnect=1':''}`);
    await eventually(()=>frames.length>0,'raw initial response'); return {ws:{close:()=>page.evaluate(()=>rawSocket.close())},frames};
  }
  try {
    await eventually(async()=>{
      if(spawnError || child.exitCode!==null) throw spawnError||new Error('Server exited');
      try{await api.listRooms();return true;}catch{return false;}
    },'server ready',30000);
    await api.createRoom({name:roomName});
    const pub=await client('publisher'), sub=await client('subscriber');
    const video=await pub.evaluate(()=>publishVideo()), audio=await pub.evaluate(()=>publishAudio()), both=[video,audio];
    await check('initial audio and video decode',async()=>{await media(sub,both);});
    let interrupted;
    await check('subscriber signal loss preserves participant and decoded media',async()=>{
      interrupted=await cut(sub);
      await media(sub,both); await delay(1000);
      assert.equal((await api.getParticipant(roomName,'subscriber')).sid,interrupted.sid);
      assert.equal(await samePCs(sub),true);
    });
    await check('subscriber resumes same SID and PCs with ICE restart',async()=>{
      const initialUfrag=clients.get(sub).sockets[0].find(f=>f.type==='offer').ufrag;
      await resume(sub,interrupted); await media(sub,both);
      await eventually(()=>clients.get(sub).sockets.at(-1).some(f=>f.type==='offer' && f.ufrag!==initialUfrag),'new ICE credentials');
    });
    await check('reliable and lossy data continue after resume',async()=>{
      for(const reliable of [true,false]) {
        const text=`resumed-${reliable}`;
        await pub.evaluate(({text,reliable})=>room.localParticipant.publishData(new TextEncoder().encode(text),{reliable}),{text,reliable});
        await eventually(()=>sub.evaluate(text=>received.includes(text),text),'resumed data');
      }
    });
    await check('publisher resumes without republishing media tracks',async()=>{
      const c=await cut(pub); await media(sub,both); await delay(1000); await resume(pub,c); await media(sub,both);
      assert.deepEqual((await api.getParticipant(roomName,'publisher')).tracks.map(t=>t.sid).sort(),[...both].sort());
    });
    const departed=await client('departed');
    await eventually(()=>sub.evaluate(()=>room.remoteParticipants.has('departed')),'departed member initially visible');
    await check('resume replays departures and current room participant state',async()=>{
      const c=await cut(sub);
      await departed.evaluate(()=>room.disconnect()); await departed.context().close();
      await api.updateParticipant(roomName,'publisher',{metadata:'publisher-updated-during-gap'});
      await api.updateRoomMetadata(roomName,'room-updated-during-gap');
      const late=await client('late'), hidden=await client('hidden',true,{hidden:true});
      await resume(sub,c);
      await eventually(()=>sub.evaluate(()=>!room.remoteParticipants.has('departed') && room.remoteParticipants.has('late') &&
        room.remoteParticipants.get('publisher')?.metadata==='publisher-updated-during-gap' && room.metadata==='room-updated-during-gap'),'state resync');
      assert.equal(await sub.evaluate(()=>room.remoteParticipants.has('hidden')),false);
      await late.evaluate(()=>room.disconnect()); await late.context().close();
      await hidden.evaluate(()=>room.disconnect()); await hidden.context().close();
      await media(sub,both);
    });
    await check('stale reconnect token cannot restore revoked permissions',async()=>{
      const c=await cut(sub);
      await api.updateParticipant(roomName,'subscriber',{name:'latest-name',metadata:'latest-meta',permission:permission({canPublish:false,canPublishData:false,canUpdateMetadata:false})});
      await sub.evaluate(token=>{room.engine.token=token;},c.token);
      await resume(sub,c);
      const info=await api.getParticipant(roomName,'subscriber');
      assert.equal(info.permission.canPublish,false); assert.equal(info.permission.canPublishData,false);
      await eventually(()=>sub.evaluate(()=>room.localParticipant.name==='latest-name' && room.localParticipant.metadata==='latest-meta' && !room.localParticipant.permissions.canPublish),'current claims on client');
      await media(sub,both);
    });
    await check('explicit unsubscribe survives SyncState and ICE restart',async()=>{
      await sub.evaluate(video=>room.remoteParticipants.get('publisher').getTrackPublicationBySid(video).setSubscribed(false),video);
      await eventually(()=>sub.evaluate(video=>!tracks.has(video),video),'unsubscribe');
      const c=await cut(sub); await resume(sub,c); await media(sub,[audio]);
      assert.equal(await sub.evaluate(video=>tracks.has(video),video),false);
      await sub.evaluate(video=>room.remoteParticipants.get('publisher').getTrackPublicationBySid(video).setSubscribed(true),video);
      await media(sub,both);
    });
    await check('new publication during signal gap appears after resume',async()=>{
      const c=await cut(sub); const extra=await pub.evaluate(()=>publishVideo());
      await resume(sub,c); await media(sub,[extra,...both]);
      await pub.evaluate(extra=>room.localParticipant.unpublishTrack(room.localParticipant.trackPublications.get(extra).track),extra);
    });
    await check('three successive resumes preserve original transport and tracks',async()=>{
      for(let round=0;round<3;round++){const c=await cut(sub);await resume(sub,c);await media(sub,both);}
    });
    await check('disabled video remains disabled through SyncState',async()=>{
      const bytes=()=>sub.evaluate(async video=>{
        let sum=0; (await tracks.get(video)?.getRTCStatsReport())?.forEach(s=>{if(s.type==='inbound-rtp')sum+=s.bytesReceived||0;});return sum;
      },video);
      await sub.evaluate(video=>room.remoteParticipants.get('publisher').getTrackPublicationBySid(video).setEnabled(false),video);
      await delay(700); const before=await bytes();
      const c=await cut(sub); await resume(sub,c); await media(sub,[audio]); await delay(500);
      assert.equal(await bytes(),before);
      assert.equal(await sub.evaluate(video=>room.remoteParticipants.get('publisher').getTrackPublicationBySid(video).isEnabled,video),false);
      await sub.evaluate(video=>room.remoteParticipants.get('publisher').getTrackPublicationBySid(video).setEnabled(true),video);await media(sub,both);
    });
    await check('signal absence beyond old thirty second timeout preserves media session',async()=>{
      // Keep the SDK in its normal resuming state while holding the new TCP
      // handshake. The configured 45s client handshake timeout exceeds this gap.
      const c=await cut(sub), began=Date.now(); let samples=0;
      while(Date.now()-began<31000) { await media(sub,both); ++samples; await delay(Math.min(4500,Math.max(0,31000-(Date.now()-began)))); }
      await media(sub,both); const signalGapMs=Date.now()-began;
      assert.equal((await api.getParticipant(roomName,'subscriber')).sid,c.sid);
      await resume(sub,c); await media(sub,both);
      return {signalGapMs,decodedProgressSamples:samples+1};
    });
    await check('missing resume session requests full reconnect',async()=>{
      const c=await raw('absent',true);
      assert.equal(c.frames[0].message.case,'leave'); assert.equal(c.frames[0].message.value.reason,6); assert.equal(c.frames[0].message.value.action,2);
      c.ws.close();
      await assert.rejects(()=>api.getParticipant(roomName,'absent'));
    });
    await check('signal closes before ICE connection removes participant',async()=>{
      const c=await raw('no-ice',false); assert.equal(c.frames[0].message.case,'join'); c.ws.close();
      await eventually(async()=>{try{await api.getParticipant(roomName,'no-ice');return false;}catch{return true;}},'no ICE cleanup');
    });
    await check('unknown published track in SyncState forces new session',async()=>{
      const bad=await client('bad-sync'); const initial=await sid(bad); const c=clients.get(bad);
      const bytes=new SignalRequest({message:{case:'syncState',value:{publishTracks:[{cid:'unknown',track:{sid:'TR_missing'}}]}}}).toBinary();
      await bad.evaluate(bytes=>room.engine.client.ws.send(new Uint8Array(bytes)),Array.from(bytes));
      await eventually(()=>c.sockets.flat().some(f=>f.type==='leave' && f.reason===6 && f.action===2),'full reconnect for unknown publication');
      await eventually(async()=>await sid(bad)!==initial && await bad.evaluate(()=>room.state==='connected'),'replacement session',25000);
      await bad.evaluate(()=>room.disconnect()); await bad.context().close();
    });
    record.completed=true;
  } catch (e) { record.failure = e.stack; console.error(`${kind}: ${e.stack}`); }
  finally {
    for (const proxy of proxies) proxy.release();
    for (const c of contexts) await c.close();
    for (const proxy of proxies) await proxy.close();
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
