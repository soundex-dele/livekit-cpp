// Headless official SDK sessions; Go is a test reference, never a runtime dependency.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { createServer as tcpServer } from 'node:net';
import { createRequire } from 'node:module';
import { once } from 'node:events';
import { randomBytes, createHash, createHmac } from 'node:crypto';
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
const { RoomServiceClient } = require('livekit-server-sdk');
const { SignalResponse } = require('@livekit/protocol');
const out = resolve(opts.output || resolve(here, 'results/go-token-parity'),
  new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, { recursive: true });
const report = { fullReplacementVerified: false, scope: 'JWT refresh, mutable claims and permission defaults', servers: {}, comparisons: [] };
report.sourceHashes = Object.fromEntries(['include/lk/auth.hpp','include/lk/rooms.hpp','src/auth/auth.cpp','src/rooms/rooms.cpp',
  'tests/unit/core_test.cpp','tests/go_token_parity.mjs'].map(file => [file,
  createHash('sha256').update(readFileSync(resolve(here,'..',file))).digest('hex')]));
report.periodicRequested = opts.periodic === 'true';
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
  const key = 'tokentest', secret = randomBytes(32).toString('hex');
  const p = await port(), base = `http://127.0.0.1:${p}`, roomName = 'token-refresh';
  const configPath = resolve(out, `${kind}.${kind === 'go' ? 'yaml' : 'json'}`);
  writeFileSync(configPath, kind === 'go'
    ? `port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 59800\n  port_range_end: 59980\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`
    : JSON.stringify({ bind: '127.0.0.1', port: p, keys: { [key]: secret }, rtc: { port_range_start: 60000, port_range_end: 60180 } }));
  const record = report.servers[kind] = { binary: resolve(binary), sha256: createHash('sha256').update(readFileSync(binary)).digest('hex'), cases: [] };
  const child = spawn(resolve(binary), ['--config', configPath], { windowsHide: true });
  let log = '', spawnError;
  child.stdout.on('data', b => { log += b; }); child.stderr.on('data', b => { log += b; });
  child.on('error', e => { spawnError = e; });
  const contexts = [], api = new RoomServiceClient(base, key, secret);
  const encode = value => Buffer.from(JSON.stringify(value)).toString('base64url');
  const sign = payload => {
    const data = `${encode({alg:'HS256',typ:'JWT'})}.${encode(payload)}`;
    return `${data}.${createHmac('sha256', secret).update(data).digest('base64url')}`;
  };
  function decode(token) {
    const [h, body, signature] = token.split('.');
    assert.equal(signature, createHmac('sha256', secret).update(`${h}.${body}`).digest('base64url'));
    assert.equal(JSON.parse(Buffer.from(h, 'base64url')).alg, 'HS256');
    return JSON.parse(Buffer.from(body, 'base64url'));
  }
  async function client(identity, extra = {}, rawToken) {
    const now = Math.floor(Date.now()/1000);
    const payload = { iss:key, sub:identity, nbf:now-1, exp:now+60, ...extra,
      video:{roomJoin:true,room:roomName,...extra.video} };
    const context = await browser.newContext(); contexts.push(context);
    const page = await context.newPage();
    const client = { page, payload, tokens:[], frames:[] };
    page.on('websocket', ws => ws.on('framereceived', ({payload: bytes}) => {
      try {
        const response = typeof bytes === 'string' ? SignalResponse.fromJsonString(bytes) : SignalResponse.fromBinary(bytes);
        if (response.message.case === 'refreshToken') {
          const token = response.message.value, claims = decode(token);
          client.tokens.push({ token, claims, at:Date.now() });
        }
      } catch (e) { client.frameError = e; }
    }));
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async ({base, token}) => {
      window.room = new LivekitClient.Room();
      await room.connect(base.replace('http:', 'ws:'), token, {rtcConfig:{iceServers:[]}});
    }, {base, token: rawToken || sign(payload)});
    return client;
  }
  const close = async c => { await c.page.evaluate(() => room.disconnect()); await c.page.context().close(); };
  async function latest(c, predicate = () => true) {
    await eventually(() => {
      if (c.frameError) throw c.frameError;
      return c.tokens.length && predicate(c.tokens.at(-1).claims);
    }, 'matching signed refresh token');
    return c.tokens.at(-1).claims;
  }
  async function check(name, fn) {
    const started = Date.now();
    try { const evidence = await fn(); record.cases.push({name,passed:true,elapsedMs:Date.now()-started,evidence}); console.log(`${kind}: PASS ${name}`); }
    catch (e) { record.cases.push({name,passed:false,failure:e.stack}); throw e; }
  }
  const permission = extra => ({canPublish:true,canSubscribe:true,canPublishData:true,canUpdateMetadata:true,...extra});
  try {
    await eventually(async () => {
      if (spawnError || child.exitCode !== null) throw spawnError || new Error('Server exited');
      try { await api.listRooms(); return true; } catch { return false; }
    }, 'server ready', 30000);
    await api.createRoom({name:roomName});
    let active;
    await check('join refresh has signed identity and minimum ten minute TTL', async () => {
      active = await client('main', {name:'original',metadata:'original-meta',attributes:{keep:'yes',remove:'yes'},video:{canUpdateOwnMetadata:true}});
      const c = await latest(active);
      assert.equal(c.iss,key); assert.equal(c.sub,'main'); assert.equal(c.identity,'main'); assert.equal(c.kind,'standard');
      assert.equal(c.exp-c.iat,600); assert.equal(c.nbf,c.iat);
      assert.ok(Math.abs(c.iat-Math.floor(Date.now()/1000))<10);
      assert.deepEqual(c.video,{roomJoin:true,room:roomName,canUpdateOwnMetadata:true});
      assert.equal(c.name,'original'); assert.equal(c.metadata,'original-meta');
      assert.deepEqual(c.attributes,{keep:'yes',remove:'yes'});
      return {ttl:c.exp-c.iat,identity:c.sub};
    });
    await check('long original expiry is not shortened', async () => {
      const exp = Math.floor(Date.now()/1000)+7200;
      const c = await client('long',{exp}); assert.ok(Math.abs((await latest(c)).exp-exp)<=1); await close(c);
    });
    await check('missing expiry is rejected before join', async () => {
      await assert.rejects(()=>client('no-expiry',{exp:undefined}), /invalid authorization token|Invalid or expired access token/);
    });
    await check('only Go refresh grant allowlist survives signing', async () => {
      const c = await client('grants',{kind:'sIp',kindDetails:['CLOUD_AGENT'],sip:{admin:true},agent:{admin:true},
        sha256:'digest',aud:'test',jti:'unique',privateExtra:'ignored',video:{roomRecord:true,ingressAdmin:true,destinationRoom:'other',unknownGrant:true}});
      const claims = await latest(c); assert.equal(claims.kind,'sip');
      for (const field of ['kindDetails','sip','agent','sha256','aud','jti','privateExtra']) assert.equal(claims[field],undefined);
      assert.deepEqual(claims.video,{roomJoin:true,room:roomName,roomRecord:true,ingressAdmin:true,destinationRoom:'other'});
      await close(c);
    });
    await check('safe room configuration and preset retained in refresh', async () => {
      const c = await client('config',{roomPreset:'test-preset',roomConfig:{name:roomName,emptyTimeout:75,maxParticipants:12,metadata:'config-meta'}});
      const claims=await latest(c); assert.equal(claims.roomPreset,'test-preset');
      assert.equal(claims.roomConfig.emptyTimeout,75); assert.equal(claims.roomConfig.maxParticipants,12); await close(c);
    });
    await check('sensitive output credentials suppress refresh without closing session', async () => {
      for (const egress of [{tracks:{s3:{bucket:'test',secret:'fixture-secret'}}},
        {room:{streamOutputs:[{urls:['rtmp://example.invalid/live/fixture-key']}]}}]) {
        const c=await client('credentials',{roomConfig:{egress}});
        await delay(500); assert.equal(c.tokens.length,0); assert.equal(c.frameError,undefined);
        assert.equal(await c.page.evaluate(()=>room.state),'connected'); await close(c);
      }
    });
    await check('admin updates refresh each changed field in Go order', async () => {
      const before=active.tokens.length;
      await api.updateParticipant(roomName,'main',{name:'admin-name',metadata:'admin-meta',attributes:{remove:'',new:'value'}});
      await latest(active,c=>c.attributes?.new==='value');
      const updates=active.tokens.slice(before).map(t=>({name:t.claims.name,metadata:t.claims.metadata,attributes:t.claims.attributes}));
      assert.deepEqual(updates,[
        {name:'admin-name',metadata:'original-meta',attributes:{keep:'yes',remove:'yes'}},
        {name:'admin-name',metadata:'admin-meta',attributes:{keep:'yes',remove:'yes'}},
        {name:'admin-name',metadata:'admin-meta',attributes:{keep:'yes',new:'value'}},
      ]); return updates;
    });
    await check('SDK attribute-only update preserves name and metadata', async () => {
      await active.page.evaluate(()=>room.localParticipant.setAttributes({keep:'',self:'yes'}));
      const c=await latest(active,c=>c.attributes?.self==='yes');
      assert.equal(c.name,'admin-name'); assert.equal(c.metadata,'admin-meta'); assert.deepEqual(c.attributes,{new:'value',self:'yes'});
    });
    await check('SDK name and metadata changes refresh token', async () => {
      await active.page.evaluate(()=>room.localParticipant.setName('self-name'));
      await latest(active,c=>c.name==='self-name');
      await active.page.evaluate(()=>room.localParticipant.setMetadata('self-meta'));
      const c=await latest(active,c=>c.metadata==='self-meta'); assert.equal(c.name,'self-name');
    });
    await check('empty and unchanged name or metadata emit no refresh', async () => {
      const before=active.tokens.length;
      await api.updateParticipant(roomName,'main',{name:'self-name',metadata:'self-meta'});
      await api.updateParticipant(roomName,'main',{name:'',metadata:''});
      await active.page.evaluate(()=>room.localParticipant.setMetadata(''));
      await delay(400); assert.equal(active.tokens.length,before);
      assert.equal((await api.getParticipant(roomName,'main')).metadata,'self-meta');
    });
    await check('nonempty identical attribute update still refreshes', async () => {
      const before=active.tokens.length;
      await api.updateParticipant(roomName,'main',{attributes:{self:'yes'}});
      await eventually(()=>active.tokens.length>before,'attribute claims event');
    });
    await check('permission revocation refreshes latest grants without losing room', async () => {
      await api.updateParticipant(roomName,'main',{permission:permission({canPublish:false,canPublishData:false,canUpdateMetadata:false,canPublishSources:[2],hidden:true})});
      const c=await latest(active,c=>c.video.canPublish===false);
      assert.deepEqual(c.video,{roomJoin:true,room:roomName,canPublish:false,canSubscribe:true,canPublishData:false,
        canUpdateOwnMetadata:false,canPublishSources:['microphone'],hidden:true,canSubscribeMetrics:false,canManageAgentSession:false});
      assert.equal(c.name,'self-name'); assert.equal(c.metadata,'self-meta');
    });
    await check('unchanged permission does not emit extra refresh', async () => {
      const before=active.tokens.length;
      await api.updateParticipant(roomName,'main',{permission:permission({canPublish:false,canPublishData:false,canUpdateMetadata:false,canPublishSources:[2],hidden:true})});
      await delay(400); assert.equal(active.tokens.length,before);
    });
    await check('refreshed token rejoins with revoked permissions and updated metadata', async () => {
      const token=active.tokens.at(-1).token; await close(active); active=await client('main',{},token);
      const info=await api.getParticipant(roomName,'main');
      assert.equal(info.permission.canPublish,false); assert.equal(info.permission.canPublishData,false);
      assert.equal(info.permission.canUpdateMetadata,false); assert.equal(info.permission.hidden,true);
      assert.equal(info.name,'self-name'); assert.equal(info.metadata,'self-meta'); assert.deepEqual(info.attributes,{new:'value',self:'yes'});
      await latest(active);
    });
    await check('revoked self metadata update is rejected without issuing token', async () => {
      const before=active.tokens.length;
      await assert.rejects(()=>active.page.evaluate(()=>room.localParticipant.setMetadata('denied')));
      await delay(250); assert.equal(active.tokens.length,before);
      assert.equal((await api.getParticipant(roomName,'main')).metadata,'self-meta');
    });
    await check('explicit empty optional and false value grants normalize like Go', async () => {
      const c=await client('empty-grants',{video:{destinationRoom:'',canPublish:null,canSubscribe:null,
        canUpdateOwnMetadata:null,canPublishSources:[],hidden:false,roomRecord:false,ingressAdmin:false}});
      assert.deepEqual((await latest(c)).video,{roomJoin:true,room:roomName}); await close(c);
    });
    await check('canPublishData defaults to canPublish when omitted or null', async () => {
      for (const data of [undefined,null]) {
        const c=await client(`default-${data}`,{video:{canPublish:false,canPublishData:data}});
        const info=await api.getParticipant(roomName,`default-${data}`); assert.equal(info.permission.canPublishData,false);
        const claims=await latest(c); assert.equal(claims.video.canPublishData,undefined); await close(c);
      }
    });
    await check('explicit data permission overrides publish default', async () => {
      const c=await client('data-only',{video:{canPublish:false,canPublishData:true}});
      const info=await api.getParticipant(roomName,'data-only'); assert.equal(info.permission.canPublishData,true);
      assert.equal((await latest(c)).video.canPublishData,true); await close(c);
    });
    await check('expiry and not-before allow sixty seconds of clock skew', async () => {
      const now=Math.floor(Date.now()/1000);
      for (const extra of [{exp:now-20},{nbf:now+30}]) {
        const c=await client(`skew-${extra.exp?'exp':'nbf'}`,extra); await latest(c); await close(c);
      }
      for (const extra of [{exp:now-90},{nbf:now+120}])
        await assert.rejects(()=>client('rejected-time',extra),/invalid authorization token|Invalid or expired access token/);
    });
    await check('legacy jti identity refreshes as subject', async () => {
      const c=await client('legacy',{sub:'',jti:'legacy'}); const claims=await latest(c);
      assert.equal(claims.sub,'legacy'); assert.equal(claims.identity,'legacy'); assert.equal(claims.jti,undefined); await close(c);
    });
    if (opts.periodic === 'true') await check('unmodified session receives five minute periodic refresh', async () => {
      const c=await client('periodic'); await latest(c); const first=c.tokens[0];
      await eventually(()=>c.tokens.length>=2,'five minute periodic refresh',320000);
      const next=c.tokens[1], elapsed=next.at-first.at;
      assert.ok(elapsed>=295000 && elapsed<=315000); assert.ok(next.claims.exp>=first.claims.exp+295);
      assert.equal(await c.page.evaluate(()=>room.state),'connected'); await close(c);
      return {elapsedMs:elapsed,ttl:next.claims.exp-next.claims.iat};
    });
    record.completed=true;
  } catch (e) { record.failure=e.stack; console.error(`${kind}: ${e.stack}`); }
  finally {
    for (const c of contexts) await c.close();
    if (child.exitCode===null && child.signalCode===null) { const exit=once(child,'exit'); child.kill(); await exit; }
    writeFileSync(resolve(out,`${kind}.log`),log); unlinkSync(configPath);
  }
}
try {
  assets.listen(0,'127.0.0.1'); await once(assets,'listening');
  browser=await chromium.launch({channel:'chrome',headless:true,args:['--autoplay-policy=no-user-gesture-required','--disable-background-timer-throttling']});
  report.browser=browser.version();
  if (opts.periodic === 'true') await Promise.all([exercise('go',opts['go-server']),exercise('cpp',opts['cpp-server'])]);
  else { await exercise('go',opts['go-server']); await exercise('cpp',opts['cpp-server']); }
  for (const expected of report.servers.go.cases) {
    const actual=report.servers.cpp.cases.find(c=>c.name===expected.name);
    report.comparisons.push({name:expected.name,passed:expected.passed && actual?.passed===true});
  }
  process.exitCode=report.servers.go.completed && report.servers.cpp.completed && report.comparisons.every(c=>c.passed)?0:1;
} catch (e) { report.infrastructureError=e.stack; process.exitCode=2; }
finally {
  await browser?.close(); await new Promise(r=>assets.close(r)); report.exitCode=process.exitCode||0;
  writeFileSync(resolve(out,'report.json'),JSON.stringify(report,null,2));
  console.log(`${report.comparisons.filter(c=>c.passed).length}/${report.comparisons.length} matched; ${out}`);
}
