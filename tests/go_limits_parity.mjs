// Configured Go/C++ limits over real Twirp and RTC transports, without media.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createServer} from 'node:net';
import {once} from 'node:events';
import {createRequire} from 'node:module';
import {randomBytes, createHash, createHmac} from 'node:crypto';
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
const p = createRequire(resolve(modern, '../package.json'))('@livekit/protocol');
const {AccessToken} = require('livekit-server-sdk');
const sha = data => createHash('sha256').update(data).digest('hex');
const out = resolve(opts.output || resolve(here, 'results/go-limits'), new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, {recursive: true});
const report = {fullReplacementVerified: false, scope: 'Five configured limits, legacy overrides, Twirp and RTC wire behavior (no media)',
  goBaseline: 'a390d03d28c895742d9b13c0894880ee67733801', testSha256: sha(readFileSync(fileURLToPath(import.meta.url))), servers: {}, comparisons: []};
const delay = ms => new Promise(r => setTimeout(r, ms));
async function eventually(fn, message, timeout = 8000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) { if (await fn()) return; await delay(20); }
  throw Error('Timed out: ' + message);
}
async function freePort() {
  const s = createServer(); s.listen(0, '127.0.0.1'); await once(s, 'listening');
  const port = s.address().port; await new Promise(r => s.close(r)); return port;
}
function compact(v) {
  if (typeof v === 'string' && v.length > 1024) return {bytes: Buffer.byteLength(v), sha256: sha(v)};
  if (Array.isArray(v)) return v.map(compact);
  if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, compact(x)]));
  return v;
}
const limited = {max_room_name_length: 12, max_participant_identity_length: 16, max_participant_name_length: 8, max_metadata_size: 32, max_attributes_size: 16};
const profiles = {
  limited: {limit: limited},
  unlimited: {limit: Object.fromEntries(Object.keys(limited).map(k => [k, 0]))},
  legacy: {limit: limited, room: {max_room_name_length: 6, max_participant_identity_length: 9, max_metadata_size: 24}},
  'legacy-zero': {limit: limited, room: {max_room_name_length: 0, max_participant_identity_length: 0, max_metadata_size: 0}},
  negative: {limit: {max_participant_name_length: -1}},
};
const methods = {
  CreateRoom: [p.CreateRoomRequest, p.Room], UpdateRoomMetadata: [p.UpdateRoomMetadataRequest, p.Room],
  UpdateParticipant: [p.UpdateParticipantRequest, p.ParticipantInfo], GetParticipant: [p.RoomParticipantIdentity, p.ParticipantInfo],
  ListRooms: [p.ListRoomsRequest, p.ListRoomsResponse],
};
async function exercise(kind, binary, profile, extra) {
  const record = report.servers[kind + '/' + profile] = {binary: resolve(binary), sha256: sha(readFileSync(binary)), cases: []};
  const key = 'limits', secret = randomBytes(32).toString('hex'), port = await freePort();
  const base = `http://127.0.0.1:${port}`, room = 'limits';
  const config = {port, keys: {[key]: secret}, ...extra, ...(kind === 'go'
    ? {bind_addresses: ['127.0.0.1'], rtc: {tcp_port: 0, port_range_start: 61300, port_range_end: 61340, use_external_ip: false, node_ip: '127.0.0.1', enable_loopback_candidate: true}}
    : {bind: '127.0.0.1', rtc: {port_range_start: 61350, port_range_end: 61390}})};
  record.config = {...config, keys: '[ephemeral, redacted]'};
  const path = resolve(out, kind + '-' + profile + '.json'); writeFileSync(path, JSON.stringify(config));
  const env = Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('LIVEKIT_')));
  const child = spawn(resolve(binary), ['--config', path], {windowsHide: true, env});
  let log = '', spawnError;
  child.on('error', e => spawnError = e); child.stdout.on('data', b => log += b); child.stderr.on('data', b => log += b);
  const sockets = [];
  async function token(identity, grant, fields = {}) {
    if (!identity) {
      // Deliberately invalid identity: the SDK refuses to sign this boundary
      // case, so create the HS256 wire fixture directly with our test key.
      const encode = v => Buffer.from(JSON.stringify(v)).toString('base64url');
      const data = encode({alg: 'HS256', typ: 'JWT'}) + '.' + encode({iss: key, sub: '', exp: Math.floor(Date.now() / 1000) + 900, video: grant});
      return data + '.' + createHmac('sha256', secret).update(data).digest('base64url');
    }
    const t = new AccessToken(key, secret, {identity, ttl: '15m', ...fields}); t.addGrant(grant); return t.toJwt();
  }
  const admin = await token('admin', {room, roomAdmin: true, roomCreate: true, roomList: true});
  const denied = await token('denied', {});
  async function api(method, data = {}, binary = false, jwt = admin) {
    const response = await fetch(`${base}/twirp/livekit.RoomService/${method}`, {method: 'POST',
      headers: {'Content-Type': binary ? 'application/protobuf' : 'application/json', Authorization: `Bearer ${jwt}`},
      body: binary ? methods[method][0].fromJson(data).toBinary() : JSON.stringify(data), signal: AbortSignal.timeout(10000)});
    const bytes = new Uint8Array(await response.arrayBuffer());
    const body = response.ok ? (binary ? methods[method][1].fromBinary(bytes) : methods[method][1].fromJsonString(Buffer.from(bytes).toString())).toJson()
      : JSON.parse(Buffer.from(bytes).toString());
    return {status: response.status, body};
  }
  async function check(name, fn) {
    const start = Date.now();
    try { record.cases.push({name, passed: true, evidence: compact(await fn()), elapsedMs: Date.now() - start}); }
    catch (e) { record.cases.push({name, passed: false, failure: e.stack, elapsedMs: Date.now() - start}); }
    console.log(`${kind}/${profile}: ${record.cases.at(-1).passed ? 'PASS' : 'FAIL'} ${name}`);
  }
  async function connect(identity, fields = {}, grant = {}) {
    const query = new URLSearchParams({access_token: await token(identity, {room, roomJoin: true, canUpdateOwnMetadata: true, ...grant}, fields), sdk: 'js', protocol: '15', auto_subscribe: '0'});
    const socket = new WebSocket(`${base.replace('http:', 'ws:')}/rtc?${query}`); sockets.push(socket); socket.binaryType = 'arraybuffer';
    const received = []; let error;
    socket.addEventListener('error', () => error = Error('WebSocket error'));
    socket.addEventListener('message', e => received.push(typeof e.data === 'string' ? p.SignalResponse.fromJsonString(e.data) : p.SignalResponse.fromBinary(new Uint8Array(e.data))));
    await eventually(() => { if (error) throw error; return received.some(r => r.message.case === 'join'); }, 'wire join');
    let requestId = 100;
    return {socket, received, update: async payload => {
      const id = ++requestId;
      socket.send(new p.SignalRequest({message: {case: 'updateMetadata', value: {...payload, requestId: id}}}).toBinary());
      await eventually(() => received.some(r => r.message.case === 'requestResponse' && r.message.value.requestId === id), 'metadata response');
      const response = received.find(r => r.message.case === 'requestResponse' && r.message.value.requestId === id).message.value;
      return {reason: response.reason, message: response.message};
    }};
  }
  try {
    await eventually(async () => {
      if (spawnError || child.exitCode !== null) throw spawnError || Error('Server exited: ' + child.exitCode);
      try { return (await api('ListRooms')).status === 200; } catch { return false; }
    }, 'ready', 20000);
    const sizes = profile === 'unlimited' ? [258, 258, 258, 524289, 65537]
      : profile === 'legacy' ? [6, 9, 8, 24, 16] : [12, 16, 8, 32, 16];
    const [nameSize, identitySize, participantSize, metadataSize, attrSize] = sizes;
    if (profile === 'negative') {
      for (const wire of [false, true]) await check(`negative name limit even rejects empty/${wire}`, async () => {
        const result = await api('UpdateParticipant', {room, identity: 'missing'}, wire);
        assert.equal(result.status, 400); return result;
      });
      record.completed = true; return;
    }
    for (const wire of [false, true]) {
      const suffix = wire ? '/protobuf' : '/json';
      await check('room name byte boundary' + suffix, async () => {
        const name = '界'.repeat(Math.floor(nameSize / 3)) + 'n'.repeat(nameSize % 3);
        const ok = await api('CreateRoom', {name}, wire); assert.equal(ok.status, 200); assert.equal(ok.body.name, name);
        if (profile === 'unlimited') return {status: ok.status, bytes: Buffer.byteLength(name)};
        const error = await api('CreateRoom', {name: name + 'x'}, wire); assert.equal(error.status, 400); return error;
      });
      await check('room metadata byte boundary and atomic rejection' + suffix, async () => {
        const metadata = '界'.repeat(Math.floor(metadataSize / 3)) + 'm'.repeat(metadataSize % 3);
        assert.equal((await api('CreateRoom', {name: room, metadata}, wire)).status, 200);
        assert.equal((await api('UpdateRoomMetadata', {room, metadata}, wire)).status, 200);
        if (profile === 'unlimited') return {status: 200, bytes: Buffer.byteLength(metadata)};
        const error = await api('UpdateRoomMetadata', {room, metadata: metadata + 'x'}, wire); assert.equal(error.status, 400);
        const state = await api('ListRooms', {names: [room]}); assert.equal(state.body.rooms[0].metadata, metadata); return error;
      });
      if (profile !== 'unlimited') {
        await check('create permission precedes name limit' + suffix, async () => {
          const result = await api('CreateRoom', {name: 'n'.repeat(nameSize + 1)}, wire, denied); assert.equal(result.status, 401); return result;
        });
        for (const [field, value] of [['name', 'n'.repeat(participantSize + 1)], ['metadata', 'm'.repeat(metadataSize + 1)], ['attributes', {a: 'x'.repeat(attrSize)}]]) {
          await check(`${field} limit precedes update permission` + suffix, async () => {
            const result = await api('UpdateParticipant', {room, identity: 'missing', [field]: value}, wire, denied); assert.equal(result.status, 400); return result;
          });
        }
      }
    }
    for (const path of ['/rtc/validate', '/rtc/v1/validate']) {
      for (const [identity, name, status] of [
        ['', room, 400], ['valid', '', 400],
        ['i'.repeat(identitySize), room, 200], ['i'.repeat(identitySize + 1), room, profile === 'unlimited' ? 200 : 400],
        ['valid', 'n'.repeat(nameSize + 1), profile === 'unlimited' ? 200 : 400],
        ['i'.repeat(identitySize + 1), 'n'.repeat(nameSize + 1), profile === 'unlimited' ? 200 : 400],
      ]) await check(`${path}/${Buffer.byteLength(identity)}/${Buffer.byteLength(name)}`, async () => {
        const query = new URLSearchParams({access_token: await token(identity, {roomJoin: true, room: name})});
        // Empty wrapped proto is a valid empty JoinRequest, but the query cannot be empty.
        if (path.includes('/v1/')) query.set('join_request', 'CgA=');
        const response = await fetch(`${base}${path}?${query}`); const body = await response.text();
        assert.equal(response.status, status); return {status: response.status, body};
      });
    }
    // The Go join path limits token metadata/attributes and identity, but does
    // not apply the update-name limit to the initial JWT name.
    const identity = 'i'.repeat(identitySize), c = await connect(identity, {
      name: 'n'.repeat(participantSize + 1), metadata: 'm'.repeat(metadataSize), attributes: {a: 'x'.repeat(attrSize - 1)},
    });
    await check('join applies configured bounds and preserves JWT name', async () => {
      const state = await api('GetParticipant', {room, identity});
      assert.equal(state.status, 200); assert.equal(state.body.identity.length, identitySize);
      assert.equal(state.body.name.length, participantSize + 1); assert.equal(state.body.metadata.length, metadataSize);
      assert.equal(state.body.attributes.a.length, attrSize - 1);
      return {identityBytes: identitySize, nameBytes: participantSize + 1, metadataBytes: metadataSize, attributeBytes: attrSize};
    });
    for (const wire of [false, true]) await check('participant configured boundaries/' + wire, async () => {
      const result = await api('UpdateParticipant', {room, identity, name: 'n'.repeat(participantSize), metadata: 'm'.repeat(metadataSize), attributes: {a: 'x'.repeat(attrSize - 1)}}, wire);
      assert.equal(result.status, 200); assert.equal(result.body.name.length, participantSize); assert.equal(result.body.metadata.length, metadataSize);
      assert.equal(result.body.attributes.a.length, attrSize - 1); return {nameBytes: participantSize, metadataBytes: metadataSize, attributeBytes: attrSize};
    });
    for (const [field, payload, message] of [
      ['name', {name: 'n'.repeat(participantSize + 1), metadata: 'new'}, 'exceeds name length limit'],
      ['metadata', {name: 'valid', metadata: 'm'.repeat(metadataSize + 1)}, 'exceeds metadata size limit'],
      ['attributes', {name: 'valid', attributes: {a: 'x'.repeat(attrSize)}}, 'exceeds attributes size limit'],
    ]) await check('self update ' + field, async () => {
      const before = await api('GetParticipant', {room, identity});
      const response = await c.update(payload);
      if (profile === 'unlimited') assert.equal(response.reason, 0);
      else {
        assert.equal(response.reason, p.RequestResponse_Reason.LIMIT_EXCEEDED); assert.equal(response.message, message);
        assert.deepEqual(await api('GetParticipant', {room, identity}), before);
      }
      assert.equal(c.socket.readyState, WebSocket.OPEN); return response;
    });
    await check('valid self update after rejection', async () => {
      const response = await c.update({name: 'ok', metadata: 'ok', attributes: {a: ''}}); assert.equal(response.reason, 0);
      const state = await api('GetParticipant', {room, identity}); assert.equal(state.body.name, 'ok'); assert.equal(state.body.metadata, 'ok');
      assert.equal(state.body.attributes?.a, undefined); return response;
    });
    record.completed = true;
  } catch (e) { record.infrastructureError = e.stack; }
  finally {
    for (const ws of sockets) ws.close();
    if (!spawnError && child.exitCode === null && child.signalCode === null) { const exited = once(child, 'exit'); child.kill(); await exited; }
    writeFileSync(resolve(out, kind + '-' + profile + '.log'), log.replaceAll(secret, '[test-secret]')); unlinkSync(path);
  }
}
try {
  for (const [profile, extra] of Object.entries(profiles)) {
    for (const [kind, binary] of [['go', opts['go-server']], ['cpp', opts['cpp-server']]]) await exercise(kind, binary, profile, extra);
    for (const expected of report.servers['go/' + profile].cases) {
      const actual = report.servers['cpp/' + profile].cases.find(c => c.name === expected.name); let failure;
      try { assert.ok(expected.passed && actual?.passed); assert.deepEqual(actual.evidence, expected.evidence); } catch (e) { failure = e.message; }
      report.comparisons.push({name: profile + '/' + expected.name, passed: !failure, ...(failure ? {failure} : {})});
    }
  }
  process.exitCode = Object.values(report.servers).some(s => !s.completed) ? 2
    : report.comparisons.length > 0 && report.comparisons.every(c => c.passed) ? 0 : 1;
} catch (e) { report.infrastructureError = e.stack; process.exitCode = 2; }
finally {
  report.exitCode = process.exitCode || 0; report.passed = report.comparisons.filter(c => c.passed).length; report.total = report.comparisons.length;
  writeFileSync(resolve(out, 'report.json'), JSON.stringify(report, null, 2)); console.log(`${report.passed}/${report.total}; ${out}`);
}
