// Official browser SDK <-> server RoomService RPC differential integration.
// Synthetic data only, headless Chrome; no desktop, camera, or microphone use.
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
const options = Object.fromEntries(process.argv.slice(2).reduce((pairs, arg, i, all) => {
  if (arg.startsWith('--')) pairs.push([arg.slice(2), all[i + 1]]);
  return pairs;
}, []));
assert.ok(options['go-server'] && options['cpp-server'], 'Pass --go-server and --cpp-server');
const deps = resolve(options['node-modules'] || resolve(here, 'node_modules'));
const requireDeps = createRequire(resolve(deps, '../package.json'));
const { chromium } = requireDeps('playwright');
const { AccessToken } = requireDeps('livekit-server-sdk');
const { proto3, ScalarType } = requireDeps('@bufbuild/protobuf');
// These two RPC messages are newer than the pinned test SDK's protocol package.
// Use its protobuf runtime with the exact fields from livekit_room.proto; both
// encoders/decoders are exercised against the real Go reference, not a mock.
const RpcRequest = proto3.makeMessageType('livekit.PerformRpcRequest', [
  ...['room', 'destination_identity', 'method', 'payload'].map((name, i) =>
    ({ no: i + 1, name, kind: 'scalar', T: ScalarType.STRING })),
  { no: 5, name: 'response_timeout_ms', kind: 'scalar', T: ScalarType.UINT32 },
]);
const RpcResponse = proto3.makeMessageType('livekit.PerformRpcResponse', [
  { no: 1, name: 'payload', kind: 'scalar', T: ScalarType.STRING },
]);
const relocationRequests = Object.fromEntries(['ForwardParticipant', 'MoveParticipant'].map(method =>
  [method, proto3.makeMessageType(`livekit.${method}Request`,
    ['room', 'identity', 'destination_room'].map((name, i) => ({ no: i + 1, name, kind: 'scalar', T: ScalarType.STRING })))]));
const out = resolve(options.output || resolve(here, 'results/go-rpc-parity'),
  new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, { recursive: true });
const report = { fullReplacementVerified: false, scope: 'RoomService PerformRpc, data-channel RPC, ForwardParticipant/MoveParticipant failure contracts', servers: {}, comparisons: [] };
// Removing a participant races PSRPC retry against endpoint unregistration.
// Both exact errors were observed on the unchanged pinned Go reference:
// participant-regression/2026-10-27T03-23-07-957Z-2c9a3f (503), and
// token-regression/2026-10-27T03-48-16-236Z-8abee6 (404).
const disconnectOutcomes = [
  { status: 404, body: { code: 'not_found', msg: 'participant does not exist' } },
  { status: 503, body: { code: 'unavailable', msg: 'no response from servers' } },
];
const isDisconnectOutcome = result => disconnectOutcomes.some(value => {
  try { assert.deepEqual(result, value); return true; } catch { return false; }
});
report.disconnectContract = { allowedOutcomes: disconnectOutcomes, maximumCompletionMs: 5000,
  participantRemovalRequired: true, cppPendingRequestCleanupRequired: true };
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function eventually(fn, timeout = 10000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { if (await fn()) return; await delay(25); }
  throw new Error('Timed out waiting for condition');
}
async function port() {
  const server = tcpServer(); server.listen(0, '127.0.0.1'); await once(server, 'listening');
  const value = server.address().port; await new Promise(r => server.close(r)); return value;
}
function compact(value) {
  if (typeof value === 'string' && value.length > 1024)
    return { bytes: Buffer.byteLength(value), sha256: createHash('sha256').update(value).digest('hex') };
  if (Array.isArray(value)) return value.map(compact);
  if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, compact(v)]));
  return value;
}
let browser;
const sdk = readFileSync(resolve(deps, 'livekit-client/dist/livekit-client.umd.js'));
const assets = createServer((req, res) => {
  if (req.url === '/sdk.js') { res.setHeader('Content-Type', 'application/javascript'); res.end(sdk); }
  else { res.setHeader('Content-Type', 'text/html'); res.end('<!doctype html><script src="/sdk.js"></script>'); }
});

async function exercise(kind, binary) {
  const key = 'rpctest', secret = randomBytes(32).toString('hex'), roomName = 'rpc-parity';
  const httpPort = await port();
  const base = `http://127.0.0.1:${httpPort}`;
  const configPath = resolve(out, `${kind}.${kind === 'go' ? 'yaml' : 'json'}`);
  const config = kind === 'go'
    ? `port: ${httpPort}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 59200\n  port_range_end: 59280\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`
    : JSON.stringify({ bind: '127.0.0.1', port: httpPort, keys: { [key]: secret }, rtc: { port_range_start: 59300, port_range_end: 59380 } });
  writeFileSync(configPath, config);
  const record = report.servers[kind] = { binary: resolve(binary), sha256: createHash('sha256').update(readFileSync(binary)).digest('hex'), cases: [] };
  const process = spawn(resolve(binary), ['--config', configPath], { windowsHide: true });
  let log = '', spawnError;
  process.stdout.on('data', bytes => { log += bytes; });
  process.stderr.on('data', bytes => { log += bytes; });
  process.on('error', error => { spawnError = error; });
  const contexts = [];
  async function jwt(identity, grant) {
    const t = new AccessToken(key, secret, { identity, ttl: '10m' }); t.addGrant(grant); return t.toJwt();
  }
  const admin = await jwt('administrator', { room: roomName, roomAdmin: true, roomCreate: true, roomList: true });
  async function api(method, payload, { binary = false, auth = admin, headers = {}, signal } = {}) {
    const response = await fetch(`${base}/twirp/livekit.RoomService/${method}`, {
      method: 'POST', headers: { Authorization: `Bearer ${auth}`, 'Content-Type': binary ? 'application/protobuf' : 'application/json', ...headers },
      body: binary ? new (relocationRequests[method] || RpcRequest)(payload).toBinary() : JSON.stringify(payload), signal: signal || AbortSignal.timeout(25000),
    });
    const body = binary && response.ok ? RpcResponse.fromBinary(new Uint8Array(await response.arrayBuffer())).toJson() : await response.json();
    // Default empty protobuf strings may be omitted by toJson; keep semantics.
    if (response.ok && method === 'PerformRpc' && body.payload === undefined) body.payload = '';
    return { status: response.status, body };
  }
  const call = (method, payload = '', timeout = 2500, extra = {}) =>
    api('PerformRpc', { room: roomName, destinationIdentity: 'callee', method, payload, responseTimeoutMs: timeout }, extra);
  async function check(name, fn) {
    const started = Date.now();
    try { const result = await fn(); record.cases.push({ name, result: compact(result), elapsedMs: Date.now() - started }); console.log(`${kind}: ${name}`); }
    catch (error) { record.cases.push({ name, failure: error.stack, elapsedMs: Date.now() - started }); console.log(`${kind}: FAIL ${name}: ${error.message}`); }
  }
  async function client(identity, grant = {}) {
    const context = await browser.newContext(); contexts.push(context);
    const page = await context.newPage(); await page.goto(`http://127.0.0.1:${assets.address().port}`);
    page.on('console', message => { log += `[browser:${identity}:${message.type()}] ${message.text()}\n`; });
    page.on('pageerror', error => { log += `[browser:${identity}:error] ${error.stack}\n`; });
    const token = await jwt(identity, { room: roomName, roomJoin: true, ...grant });
    await page.evaluate(async ({ base, token }) => {
      window.room = new LivekitClient.Room({ adaptiveStream: false, dynacast: false });
      window.invocations = [];
      room.registerRpcMethod('echo', async data => { invocations.push(data); return data.payload; });
      room.registerRpcMethod('empty', async () => '');
      room.registerRpcMethod('delayed', async data => { invocations.push(data); await new Promise(r => setTimeout(r, 350)); return data.payload; });
      room.registerRpcMethod('hold', async data => { invocations.push(data); return new Promise(resolve => { window.releaseRpc = resolve; }); });
      room.registerRpcMethod('application', async () => { throw new Error('test exception'); });
      room.registerRpcMethod('custom', async () => { throw new LivekitClient.RpcError(2001, 'business rejection', 'details'); });
      room.registerRpcMethod('oversize', async () => 'x'.repeat(15361));
      let retries = 0;
      room.registerRpcMethod('recoverable', async () => {
        if (++retries < 3) throw new LivekitClient.RpcError(1503, 'Recipient disconnected');
        return `recovered-${retries}`;
      });
      // This suite tests RPC on the same host. Explicitly disable external STUN
      // for both servers so local connection setup does not depend on the WAN.
      await room.connect(base.replace('http:', 'ws:'), token, { rtcConfig: { iceServers: [] } });
    }, { base, token });
    return page;
  }
  try {
    await eventually(async () => {
      if (spawnError || process.exitCode !== null) throw spawnError || new Error('Server exited');
      try { return (await api('ListRooms', {})).status === 200; } catch { return false; }
    }, 30000);
    await api('CreateRoom', { name: roomName });
    const callee = await client('callee'), attacker = await client('attacker');
    await check('JSON echo and server caller identity', async () => {
      const result = await call('echo', 'hello'); assert.equal(result.status, 200); assert.equal(result.body.payload, 'hello');
      const data = await callee.evaluate(() => invocations.at(-1));
      assert.match(data.requestId, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
      assert.equal(data.callerIdentity, data.requestId); return result;
    });
    await check('protobuf echo', async () => { const r = await call('echo', 'protobuf', 2500, { binary: true }); assert.equal(r.status, 200); assert.equal(r.body.payload, 'protobuf'); return r; });
    await check('empty response payload', async () => { const r = await call('empty'); assert.equal(r.status, 200); assert.equal(r.body.payload, ''); return r; });
    await check('empty protobuf response', async () => { const r = await call('empty', '', 2500, { binary: true }); assert.equal(r.status, 200); assert.equal(r.body.payload, ''); return r; });
    await check('default response timeout', async () => {
      const r = await call('echo', 'default', 0); assert.equal(r.status, 200);
      const data = await callee.evaluate(() => invocations.at(-1));
      assert.ok(data.responseTimeout > 14000 && data.responseTimeout <= 15000); return r;
    });
    await check('unsupported method', () => call('unregistered'));
    await check('protobuf error response is JSON', () => call('unregistered', '', 2500, { binary: true }));
    await check('application error', () => call('application'));
    await check('custom error', () => call('custom'));
    await check('recoverable error retries', async () => { const r = await call('recoverable'); assert.equal(r.status, 200); assert.equal(r.body.payload, 'recovered-3'); return r; });
    await check('response payload too large', () => call('oversize'));
    await check('request payload too large', () => call('echo', 'x'.repeat(15361)));
    await check('UTF-8 payload boundary', async () => { const text = '中'.repeat(5120); const r = await call('echo', text); assert.equal(r.status, 200); assert.equal(r.body.payload, text); return r; });
    await check('missing destination identity', () => api('PerformRpc', { room: roomName, method: 'echo' }));
    await check('unknown destination', () => api('PerformRpc', { room: roomName, destinationIdentity: 'absent', method: 'echo', responseTimeoutMs: 500 }));
    await check('destination joins during discovery', async () => {
      const pending = api('PerformRpc', { room: roomName, destinationIdentity: 'late', method: 'echo', payload: 'late-join', responseTimeoutMs: 2500 });
      await delay(150); const late = await client('late');
      const r = await pending; assert.equal(r.status, 200); assert.equal(r.body.payload, 'late-join');
      await late.evaluate(() => room.disconnect()); return r;
    });
    await check('data permission blocks RPC replies', async () => {
      const silent = await client('silent', { canPublishData: false });
      const r = await api('PerformRpc', { room: roomName, destinationIdentity: 'silent', method: 'echo', payload: 'blocked', responseTimeoutMs: 200 });
      assert.notEqual(r.status, 200); await silent.evaluate(() => room.disconnect()); return r;
    });
    const denied = await jwt('denied', { roomJoin: true, room: roomName });
    await check('permission denied', () => call('echo', '', 2500, { auth: denied }));
    await check('response timeout after ACK', () => call('delayed', 'late', 100));
    await delay(400);
    await check('HTTP deadline', () => call('delayed', 'deadline', 2500, { headers: { 'X-Twirp-Timeout-Ms': '50' } }));
    await delay(400);
    await check('concurrent requests preserve correlation', async () => {
      const values = await Promise.all(Array.from({ length: 16 }, (_, i) => call(i % 2 ? 'delayed' : 'echo', String(i))));
      values.forEach((v, i) => { assert.equal(v.status, 200); assert.equal(v.body.payload, String(i)); });
      return values;
    });
    await check('forged response from another participant', async () => {
      await callee.evaluate(() => { invocations.length = 0; });
      let finished = false;
      const pending = call('hold', 'guard').then(r => { finished = true; return r; });
      await eventually(() => callee.evaluate(() => invocations.length > 0));
      const data = await callee.evaluate(() => invocations.at(-1));
      await attacker.evaluate(async data => { await room.engine.publishRpcResponse(data.callerIdentity, data.requestId, 'forged', null); }, data);
      await delay(100); assert.equal(finished, false);
      await callee.evaluate(() => releaseRpc('legitimate'));
      const r = await pending; assert.equal(r.status, 200); assert.equal(r.body.payload, 'legitimate'); return r;
    });
    await check('client to client RPC', async () => {
      const payload = await attacker.evaluate(() => room.localParticipant.performRpc({ destinationIdentity: 'callee', method: 'echo', payload: 'peer', responseTimeout: 3000 }));
      assert.equal(payload, 'peer'); return { payload };
    });
    for (const method of ['ForwardParticipant', 'MoveParticipant']) {
      const body = { room: roomName, identity: 'callee', destinationRoom: 'destination' };
      const auth = await jwt('administrator', { room: roomName, roomAdmin: true, destinationRoom: 'destination' });
      const invoke = (overrides = {}, opts = {}) => api(method, { ...body, ...overrides }, { auth, ...opts });
      await check(`${method}: missing destination grant`, () => invoke({}, { auth: admin }));
      await check(`${method}: wrong source`, () => invoke({ room: 'other' }));
      await check(`${method}: wrong destination`, () => invoke({ destinationRoom: 'other' }));
      const noAdmin = await jwt('administrator', { room: roomName, destinationRoom: 'destination' });
      await check(`${method}: missing admin grant`, () => invoke({}, { auth: noAdmin }));
      const sameRoom = await jwt('administrator', { room: roomName, roomAdmin: true, destinationRoom: roomName });
      await check(`${method}: same source and destination`, () => invoke({ destinationRoom: roomName }, { auth: sameRoom }));
      // The pinned open-source Go backend deliberately returns "not implemented".
      await check(`${method}: existing participant`, () => invoke());
      await check(`${method}: protobuf existing participant`, () => invoke({}, { binary: true }));
      await check(`${method}: missing participant`, () => invoke({ identity: 'absent' }));
      await check(`${method}: empty identity`, () => invoke({ identity: '' }));
      await check(`${method}: empty destination`, () => invoke({ destinationRoom: '' }, { auth: admin }));
      await check(`${method}: discovery HTTP deadline`, () => invoke({ identity: 'absent' }, { headers: { 'X-Twirp-Timeout-Ms': '50' } }));
      await check(`${method}: source unchanged after rejection`, async () => {
        const r = await api('GetParticipant', { room: roomName, identity: 'callee' });
        assert.equal(r.status, 200); assert.equal(r.body.identity, 'callee');
        return { status: r.status, identity: r.body.identity };
      });
    }
    await check('client HTTP cancellation releases pending request', async () => {
      await callee.evaluate(() => { invocations.length = 0; });
      const abort = new AbortController();
      const pending = call('hold', 'cancel', 30000, { signal: abort.signal }).catch(() => null);
      await eventually(() => callee.evaluate(() => invocations.length > 0)); abort.abort(); await pending;
      if (kind === 'cpp') await eventually(async () => {
        const response = await fetch(`${base}/metrics`, { headers: { Authorization: `Bearer ${admin}` } });
        return /^livekit_cpp_rpc_pending 0$/m.test(await response.text());
      });
      await callee.evaluate(() => releaseRpc('late after cancellation'));
      return { httpRequestAborted: true };
    });
    await check('ACK timeout', async () => {
      await callee.evaluate(() => { window.savedAck = room.engine.publishRpcAck; room.engine.publishRpcAck = () => new Promise(() => {}); });
      try { return await call('echo', 'no-ack', 12000); }
      finally { await callee.evaluate(() => { room.engine.publishRpcAck = savedAck; }); }
    });
    await check('recipient disconnect', async () => {
      await callee.evaluate(() => { invocations.length = 0; });
      const pending = call('hold', 'disconnect', 15000);
      await eventually(() => callee.evaluate(() => invocations.length > 0));
      const disconnectedAt = Date.now();
      await callee.evaluate(() => room.disconnect());
      const result = await pending;
      assert.ok(Date.now() - disconnectedAt < report.disconnectContract.maximumCompletionMs, 'Disconnect did not promptly end RPC');
      assert.ok(isDisconnectOutcome(result), `Unexpected disconnect response: ${JSON.stringify(result)}`);
      await eventually(async () => (await api('GetParticipant', { room: roomName, identity: 'callee' })).status === 404);
      if (kind === 'cpp') await eventually(async () => {
        const response = await fetch(`${base}/metrics`, { headers: { Authorization: `Bearer ${admin}` } });
        return /^livekit_cpp_rpc_pending 0$/m.test(await response.text());
      });
      return result;
    });
  } finally {
    for (const context of contexts) await context.close();
    if (process.exitCode === null && process.signalCode === null) {
      const exit = once(process, 'exit'); process.kill(); await exit;
    }
    writeFileSync(resolve(out, `${kind}.log`), log);
    unlinkSync(configPath);
  }
}

try {
  assets.listen(0, '127.0.0.1'); await once(assets, 'listening');
  browser = await chromium.launch({ channel: 'chrome', headless: true });
  report.browser = browser.version();
  await exercise('go', options['go-server']);
  await exercise('cpp', options['cpp-server']);
  const reference = report.servers.go.cases, candidate = report.servers.cpp.cases;
  for (const actual of reference) {
    const other = candidate.find(c => c.name === actual.name);
    let failure, exactMatch = false;
    const comparison = actual.name === 'recipient disconnect' ? 'verified-disconnect-outcomes-and-cleanup' : 'exact';
    try {
      assert.ok(other); assert.ok(!actual.failure && !other.failure);
      try { assert.deepEqual(other.result, actual.result); exactMatch = true; } catch {}
      if (comparison === 'exact') assert.deepEqual(other.result, actual.result);
      else { assert.ok(isDisconnectOutcome(actual.result)); assert.ok(isDisconnectOutcome(other.result)); }
    }
    catch (error) { failure = error.message; }
    report.comparisons.push({ name: actual.name, passed: !failure, comparison, exactMatch, ...(failure ? { failure } : {}) });
  }
  assert.equal(reference.length, candidate.length);
  process.exitCode = report.comparisons.every(c => c.passed) ? 0 : 1;
} catch (error) { report.infrastructureError = error.stack; process.exitCode = 2; }
finally {
  await browser?.close(); await new Promise(r => assets.close(r));
  report.exitCode = process.exitCode || 0;
  writeFileSync(resolve(out, 'report.json'), JSON.stringify(report, null, 2));
  console.log(`${report.comparisons.filter(c => c.passed).length}/${report.comparisons.length} contracts passed (${report.comparisons.filter(c => c.exactMatch).length} exact responses); ${out}`);
}
