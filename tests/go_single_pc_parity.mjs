// Same-host, headless official SDK media checks against pinned Go and C++.
import assert from 'node:assert/strict';
import { gzipSync, gunzipSync } from 'node:zlib';
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
const modernDeps = resolve(opts['modern-node-modules'] || 'D:/workspace/livekit-cli-cpp/web/node_modules');
const modernRequire = createRequire(resolve(modernDeps, '../package.json'));
const { SignalRequest, SignalResponse, JoinRequest, WrappedJoinRequest } = modernRequire('@livekit/protocol');
const { AccessToken, RoomServiceClient } = require('livekit-server-sdk');
const out = resolve(opts.output || resolve(here, 'results/go-single-pc-parity'),
  new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, { recursive: true });
const report = { sdkVersion:JSON.parse(readFileSync(resolve(modernDeps,'livekit-client/package.json'))).version, fullReplacementVerified: false, scope: 'official SDK single-PC v1 audio/video/data, bidirectional SFU and resume', servers: {}, comparisons: [] };
report.sourceHashes = Object.fromEntries(['include/lk/rooms.hpp','include/lk/rtc.hpp','src/rooms/rooms.cpp',
  'src/rtc/rtc.cpp','src/transport/http.cpp','tests/unit/core_test.cpp','tests/go_single_pc_parity.mjs','include/lk/sfu.hpp','src/sfu/sfu.cpp','src/transport/join_request.cpp','include/lk/join_request.hpp'].map(file=>
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
const sdk = readFileSync(resolve(modernDeps, 'livekit-client/dist/livekit-client.umd.js'));
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
  const key = 'resumetest', secret = randomBytes(32).toString('hex'), roomName = 'single-pc-parity';
  const p = await port(), base = `http://127.0.0.1:${p}`;
  const configPath = resolve(out, `${kind}.${kind === 'go' ? 'yaml' : 'json'}`);
  writeFileSync(configPath, kind === 'go'
    ? `port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 60600\n  port_range_end: 60780\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`
    : JSON.stringify({ bind: '127.0.0.1', port: p, keys: { [key]: secret }, rtc: { port_range_start: 60800, port_range_end: 60980 } }));
  const record = report.servers[kind] = { binary: resolve(binary), sha256: createHash('sha256').update(readFileSync(binary)).digest('hex'), cases: [] };
  const child = spawn(resolve(binary), ['--config', configPath], { windowsHide: true });
  let log = '', spawnError;
  child.stdout.on('data', b => { log += b; }); child.stderr.on('data', b => { log += b; });
  child.on('error', e => { spawnError = e; });
  const contexts = [], proxies = []; const clients = new Map();
  const api = new RoomServiceClient(base, key, secret);
  async function client(identity, autoSubscribe = true, grant = {}, singlePeerConnection = true, compressed = true) {
    const token = new AccessToken(key, secret, { identity, ttl: '15m' });
    token.addGrant({ roomJoin: true, room: roomName, ...grant });
    const proxy = await signalProxy(p); proxies.push(proxy);
    const context = await browser.newContext(); contexts.push(context);
    const page = await context.newPage();
    const state = { proxy, identity, token: await token.toJwt(), sockets: [] }; clients.set(page, state);
    page.on('websocket', socket => {
      const encoded=new URL(socket.url()).searchParams.get('join_request');
      if(encoded) {
        const wrapped=WrappedJoinRequest.fromBinary(Buffer.from(encoded,'base64url'));
        const join=JoinRequest.fromBinary(wrapped.compression===1 ? gunzipSync(wrapped.joinRequest) : wrapped.joinRequest);
        state.joinEvidence={compression:wrapped.compression,protocol:join.clientInfo?.protocol,hasPublisherOffer:!!join.publisherOffer};
      }
      const frames = []; state.sockets.push(frames);
      socket.on('framereceived', ({payload}) => {
        try {
          const r = typeof payload === 'string' ? SignalResponse.fromJsonString(payload) : SignalResponse.fromBinary(payload);
          const c = r.message.case, value = r.message.value;
          frames.push({type:c,...(c === 'answer' ? {ufrag:value.sdp.match(/a=ice-ufrag:([^\r\n]+)/)?.[1]} : {}),
            ...(c === 'leave' ? {reason:value.reason,action:value.action} : {})});
        } catch (e) { state.decodeError = e.stack; }
      });
    });
    page.on('console', m => { if (m.type() === 'error' || m.type() === 'warning') log += `[${identity}] ${m.text()}\n`; });
    page.on('pageerror', e => { log += `[${identity}] ${e.stack}\n`; });
    if(!compressed) await page.addInitScript(()=>{window.CompressionStream=undefined;});
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async ({ base, token, autoSubscribe, singlePeerConnection }) => {
      const { Room, RoomEvent, Track } = LivekitClient;
      window.room = new Room({ singlePeerConnection, adaptiveStream: false, dynacast: false, publishDefaults: { simulcast: false, videoCodec: 'vp8', red: false } });
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
      window.originalPCs = [room.engine.pcManager.publisher.pc,room.engine.pcManager.subscriber?.pc];
    }, { base: `http://127.0.0.1:${proxy.port}`, token: state.token, autoSubscribe, singlePeerConnection });
    assert.equal(await page.evaluate(()=>room.engine.pcManager.mode),singlePeerConnection ? 'publisher-only' : 'subscriber-primary','SDK must not fall back from v1');
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
  const samePCs = page => page.evaluate(()=>originalPCs[0]===room.engine.pcManager.publisher.pc && originalPCs[1]===room.engine.pcManager.subscriber?.pc);
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
  try {
    await eventually(async()=>{
      if(spawnError || child.exitCode!==null) throw spawnError||new Error('Server exited');
      try{await api.listRooms();return true;}catch{return false;}
    },'server ready',30000);
    await api.createRoom({name:roomName});
    const validateToken = new AccessToken(key,secret,{identity:'validation',ttl:'10m',attributes:{keep:'original'}});
    validateToken.addGrant({roomJoin:true,room:roomName,canUpdateOwnMetadata:true});
    const tokenText=await validateToken.toJwt();
    const wrap=(bytes,compression=0)=>Buffer.from(new WrappedJoinRequest({joinRequest:bytes,compression}).toBinary()).toString('base64').replaceAll('+','-').replaceAll('/','_');
    const valid=new JoinRequest({clientInfo:{protocol:18},connectionSettings:{autoSubscribe:true},metadata:'v1 metadata',participantAttributes:{keep:'',new:'yes'}}).toBinary();
    async function validate(encoded,status) {
      const query=new URLSearchParams({access_token:tokenText}); if(encoded!==undefined) query.set('join_request',encoded);
      const response=await fetch(`${base}/rtc/v1/validate?${query}`); const text=await response.text();
      assert.equal(response.status,status,text); if(status===200) assert.equal(text,'success');
    }
    await check('v1 validates plain and gzip protobuf JoinRequest',async()=>{
      await validate(wrap(valid),200); await validate(wrap(gzipSync(valid),1),200);
      await validate(wrap(Buffer.concat([gzipSync(valid.slice(0,3)),gzipSync(valid.slice(3))]),1),200);
    });
    await check('v1 rejects missing malformed truncated and oversized JoinRequest',async()=>{
      await validate(undefined,400); await validate('not_base64',400); await validate(wrap(Buffer.from([255])),400);
      await validate(wrap(gzipSync(valid).subarray(0,12),1),400);
      const huge=new JoinRequest({metadata:'x'.repeat(1024*1024)}).toBinary();
      await validate(wrap(gzipSync(huge),1),400);
    });
    let pub,sub,video,audio,reverseVideo,reverseAudio;
    await check('v1 joins with exactly one PeerConnection and no dual fallback',async()=>{
      pub=await client('publisher'); sub=await client('subscriber');
      for(const page of [pub,sub]) {
        assert.equal(await page.evaluate(()=>room.engine.pcManager.subscriber===undefined),true);
        assert.equal(clients.get(page).sockets.length,1);
        assert.equal(clients.get(page).joinEvidence.compression,1);
        assert.ok(clients.get(page).joinEvidence.hasPublisherOffer);
        record.initialJoin=clients.get(page).joinEvidence;
      }
    });
    await check('single-PC publisher audio and video decode at subscriber',async()=>{
      video=await pub.evaluate(()=>publishVideo()); audio=await pub.evaluate(()=>publishAudio());
      await media(sub,[video,audio]);
    });
    await check('same PC simultaneously publishes and receives real audio and video',async()=>{
      reverseVideo=await sub.evaluate(()=>publishVideo()); reverseAudio=await sub.evaluate(()=>publishAudio());
      await media(pub,[reverseVideo,reverseAudio]); await media(sub,[video,audio]);
    });
    await check('reliable and lossy data work in both directions',async()=>{
      for(const [from,to] of [[pub,sub],[sub,pub]]) for(const reliable of [true,false]) {
        const text=`single-${reliable}-${Date.now()}`;
        await from.evaluate(({text,reliable})=>room.localParticipant.publishData(new TextEncoder().encode(text),{reliable}),{text,reliable});
        await eventually(()=>to.evaluate(text=>received.includes(text),text),'bidirectional data');
      }
    });
    await check('server requests more client media sections when initial slots exhausted',async()=>{
      const extras=[];
      for(let i=0;i<4;i++) extras.push(await pub.evaluate(()=>publishVideo()));
      await media(sub,extras); await media(pub,[reverseVideo,reverseAudio]);
      assert.ok(clients.get(sub).sockets[0].some(f=>f.type==='mediaSectionsRequirement'));
      for(const sid of extras) await pub.evaluate(sid=>room.localParticipant.unpublishTrack(room.localParticipant.trackPublications.get(sid).track),sid);
    });
    await check('unsubscribe and resubscribe renegotiate on publisher transport',async()=>{
      await sub.evaluate(sid=>room.remoteParticipants.get('publisher').trackPublications.get(sid).setSubscribed(false),video);
      await eventually(()=>sub.evaluate(sid=>!tracks.has(sid),video),'unsubscribe');
      await media(sub,[audio]); await media(pub,[reverseVideo,reverseAudio]);
      await sub.evaluate(sid=>room.remoteParticipants.get('publisher').trackPublications.get(sid).setSubscribed(true),video);
      await media(sub,[video,audio]);
    });
    await check('single-PC signal resume preserves session and both media directions',async()=>{
      const before=clients.get(sub).sockets[0].find(f=>f.type==='answer')?.ufrag;
      const c=await cut(sub); await media(sub,[video,audio]); await media(pub,[reverseVideo,reverseAudio]);
      await resume(sub,c); await media(sub,[video,audio]); await media(pub,[reverseVideo,reverseAudio]);
      await eventually(()=>clients.get(sub).sockets.at(-1).some(f=>f.type==='answer'&&f.ufrag!==before),'resumed publisher ICE credentials');
    });
    await check('resume recovers a media section request lost during signaling gap',async()=>{
      const c=await cut(sub),extras=[];
      for(let i=0;i<5;i++) extras.push(await pub.evaluate(()=>publishVideo()));
      await media(pub,[reverseVideo,reverseAudio]);
      await resume(sub,c); await media(sub,[video,audio,...extras]);
      for(const sid of extras) await pub.evaluate(sid=>room.localParticipant.unpublishTrack(room.localParticipant.trackPublications.get(sid).track),sid);
    });
    await check('dual-PC peer interoperates bidirectionally with single-PC peers',async()=>{
      const dual=await client('dual',true,{},false);
      await media(dual,[video,audio,reverseVideo,reverseAudio]);
      const v=await dual.evaluate(()=>publishVideo()),a=await dual.evaluate(()=>publishAudio());
      await media(pub,[v,a]); await media(sub,[v,a]);
      await dual.evaluate(()=>room.disconnect());
    });
    await check('uncompressed v1 join receives real media',async()=>{
      const plain=await client('plain',true,{},true,false);
      assert.equal(clients.get(plain).joinEvidence.compression,0);
      await media(plain,[video,audio]); await plain.evaluate(()=>room.disconnect());
    });
    await check('autoSubscribe false waits for explicit subscription',async()=>{
      const late=await client('manual',false);
      assert.equal(await late.evaluate(()=>tracks.size),0);
      await api.updateSubscriptions(roomName,'manual',[video,audio],true);
      await media(late,[video,audio]); await late.evaluate(()=>room.disconnect());
    });
    if(kind==='cpp') {
      const access=new AccessToken(key,secret); access.addGrant({roomList:true});
      const response=await fetch(`${base}/metrics`,{headers:{Authorization:`Bearer ${await access.toJwt()}`}});
      assert.equal(response.status,200);
      const text=await response.text();
      const metrics=Object.fromEntries(text.trim().split('\n').map(line=>{const [k,v]=line.split(' '); return [k.replace(/^livekit_cpp_/,''),Number(v)];}));
      assert.ok(metrics.sfu_rtp_forwarded>0); assert.equal(metrics.video_encode_calls,0); assert.equal(metrics.video_decode_calls,0);
      record.packetSfuAudit={forwarded:metrics.sfu_rtp_forwarded,encodeCalls:metrics.video_encode_calls,decodeCalls:metrics.video_decode_calls};
    }
    record.completed=true;
  } catch (e) {
    record.failure = e.stack; console.error(`${kind}: ${e.stack}`);
    record.diagnostics=[];
    for(const [page,c] of clients) if(!page.isClosed()) {
      try { record.diagnostics.push({identity:c.identity,signals:c.sockets,rtc:await page.evaluate(async()=>{
        const pc=room.engine.pcManager?.publisher?.pc;
        return {tracks:[...tracks.keys()],local:pc?.localDescription?.sdp,remote:pc?.remoteDescription?.sdp,
          stats:pc ? [...(await pc.getStats()).values()].filter(s=>['inbound-rtp','outbound-rtp','candidate-pair','transport'].includes(s.type)) : []};
      })}); } catch {}
    }
  }
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
