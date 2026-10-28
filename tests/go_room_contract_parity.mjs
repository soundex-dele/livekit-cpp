// Real Go/C++ RoomService wire/validation differential. No browser or desktop.
import assert from 'node:assert/strict';
import {spawn, execFileSync} from 'node:child_process';
import {createServer} from 'node:net';
import {once} from 'node:events';
import {createHash, randomBytes} from 'node:crypto';
import {mkdirSync, readFileSync, writeFileSync, unlinkSync} from 'node:fs';
import {resolve, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';

const here = dirname(fileURLToPath(import.meta.url));
const opts = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => {
  if (v.startsWith('--')) a.push([v.slice(2), all[i + 1]]); return a;
}, []));
assert.ok(opts['go-server'] && opts['cpp-server'], 'Pass --go-server and --cpp-server');
const require = createRequire(resolve(opts['node-modules'] || resolve(here, 'node_modules'), '../package.json'));
const p = require('@livekit/protocol');
const {AccessToken} = require('livekit-server-sdk');
const {proto3, ScalarType} = require('@bufbuild/protobuf');
const RpcRequest = proto3.makeMessageType('livekit.PerformRpcRequest', [
  ...['room', 'destination_identity', 'method', 'payload'].map((name, i) => ({no: i + 1, name, kind: 'scalar', T: ScalarType.STRING})),
  {no: 5, name: 'response_timeout_ms', kind: 'scalar', T: ScalarType.UINT32},
]);
const RpcResponse = proto3.makeMessageType('livekit.PerformRpcResponse', [{no: 1, name: 'payload', kind: 'scalar', T: ScalarType.STRING}]);
const methods = {
  CreateRoom: [p.CreateRoomRequest, p.Room], ListRooms: [p.ListRoomsRequest, p.ListRoomsResponse],
  DeleteRoom: [p.DeleteRoomRequest, p.DeleteRoomResponse], ListParticipants: [p.ListParticipantsRequest, p.ListParticipantsResponse],
  GetParticipant: [p.RoomParticipantIdentity, p.ParticipantInfo], RemoveParticipant: [p.RoomParticipantIdentity, p.RemoveParticipantResponse],
  MutePublishedTrack: [p.MuteRoomTrackRequest, p.MuteRoomTrackResponse], UpdateParticipant: [p.UpdateParticipantRequest, p.ParticipantInfo],
  UpdateSubscriptions: [p.UpdateSubscriptionsRequest, p.UpdateSubscriptionsResponse], SendData: [p.SendDataRequest, p.SendDataResponse],
  UpdateRoomMetadata: [p.UpdateRoomMetadataRequest, p.Room], ForwardParticipant: [p.ForwardParticipantRequest, p.ForwardParticipantResponse],
  MoveParticipant: [p.MoveParticipantRequest, p.MoveParticipantResponse], PerformRpc: [RpcRequest, RpcResponse],
};
const out = resolve(opts.output || resolve(here, 'results/go-room-contract'), new Date().toISOString().replaceAll(/[:.]/g, '-') + '-' + randomBytes(3).toString('hex'));
mkdirSync(out, {recursive: true});
const digest = path => createHash('sha256').update(readFileSync(path)).digest('hex');
const report = {fullReplacementVerified: false, scope: 'RoomService JSON/protobuf permissions, validation order, limits and wire decoding',
  excludedRoomFields: ['sid (format checked)', 'creation_time', 'creation_time_ms', 'enabled_codecs (R04)', 'turn_password (R08)'],
  source: {testSha256: digest(fileURLToPath(import.meta.url)), protocol: 'f09f87aab29b865850a967924726bf76b7db4755',
    goBaseline: 'a390d03d28c895742d9b13c0894880ee67733801'}, servers: {}, comparisons: []};
try { report.source.cppHead = execFileSync('git', ['rev-parse', 'HEAD'], {cwd: resolve(here, '..'), encoding: 'utf8'}).trim(); } catch {}
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function port() {
  const s = createServer(); s.listen(0, '127.0.0.1'); await once(s, 'listening');
  const result = s.address().port; await new Promise(r => s.close(r)); return result;
}
function compact(v) {
  if (typeof v === 'string' && v.length > 1024) return {bytes: Buffer.byteLength(v), sha256: createHash('sha256').update(v).digest('hex')};
  if (Array.isArray(v)) return v.map(compact);
  if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).map(([k, value]) => [k, compact(value)]));
  return v;
}
async function exercise(kind, binary) {
  const record = report.servers[kind] = {binary: resolve(binary), sha256: digest(binary), cases: []};
  const key = 'contractkey', secret = randomBytes(32).toString('hex'), room = 'contract-room', missing = 'missing-room';
  const config = {port: await port(), keys: {[key]: secret}};
  if (kind === 'go') Object.assign(config, {bind_addresses: ['127.0.0.1'], rtc: {tcp_port: 0, port_range_start: 59400, port_range_end: 59440, use_external_ip: false, node_ip: '127.0.0.1'}});
  else Object.assign(config, {bind: '127.0.0.1', rtc: {port_range_start: 59450, port_range_end: 59490}});
  record.config = {...config, keys: '[ephemeral, redacted]'};
  const configPath = resolve(out, kind + '.json'); writeFileSync(configPath, JSON.stringify(config));
  const child = spawn(resolve(binary), ['--config', configPath], {windowsHide: true});
  let log = '', spawnError;
  child.stdout.on('data', b => log += b); child.stderr.on('data', b => log += b); child.on('error', e => spawnError = e);
  const base = `http://127.0.0.1:${config.port}`;
  const auth = {};
  for (const [name, grant] of Object.entries({admin: {room, roomAdmin: true, roomCreate: true, roomList: true, destinationRoom: 'destination'},
    none: {}, join: {room, roomJoin: true}, other: {room: 'other', roomAdmin: true},
    list: {roomList: true}, create: {roomCreate: true}, roomAdmin: {room, roomAdmin: true}, missing: {room: missing, roomAdmin: true}})) {
    const t = new AccessToken(key, secret, {identity: 'contract', ttl: '1h'}); t.addGrant(grant); auth[name] = await t.toJwt();
  }
  async function api(method, data = {}, options = {}) {
    const {binary: wire = false, grant = 'admin', raw, type, verb = 'POST', codeOnly = false} = options;
    const headers = {'Content-Type': type || (wire ? 'application/protobuf' : 'application/json')};
    if (auth[grant]) headers.Authorization = `Bearer ${auth[grant]}`;
    const body = raw ?? (wire ? methods[method][0].fromJson(data).toBinary() : JSON.stringify(data));
    const response = await fetch(`${base}/twirp/livekit.RoomService/${method}`, {
      method: verb, headers, ...(verb === 'GET' ? {} : {body}), signal: AbortSignal.timeout(15000),
    });
    const bytes = new Uint8Array(await response.arrayBuffer());
    let result;
    if (response.ok) {
      const Message = methods[method]?.[1];
      if (wire) {
        assert.match(response.headers.get('content-type'), /^application\/protobuf(?:;|$)/);
        result = Message.fromBinary(bytes).toJson({emitDefaultValues: true, useProtoFieldName: true});
      } else {
        assert.match(response.headers.get('content-type'), /^application\/json(?:;|$)/);
        result = JSON.parse(Buffer.from(bytes).toString());
      }
    } else {
      const text = Buffer.from(bytes).toString();
      try { result = JSON.parse(text); } catch { result = {text}; }
    }
    const evidence = {status: response.status, body: result};
    // These non-media cases compare room state but do not claim codec parity.
    // Random IDs/timestamps are validated separately, never compared across servers.
    function normalize(value) {
      if (Array.isArray(value)) return value.map(normalize);
      if (!value || typeof value !== 'object') return value;
      return Object.fromEntries(Object.entries(value).filter(([k]) => !['sid', 'creation_time', 'creation_time_ms', 'enabled_codecs', 'turn_password'].includes(k)).map(([k, v]) => [k, normalize(v)]));
    }
    evidence.comparison = codeOnly && !response.ok ? {status: response.status, code: result.code} : {status: response.status, body: normalize(result)};
    if (response.ok && method === 'CreateRoom') assert.match(result.sid, /^RM_[A-Za-z0-9_-]+$/);
    return evidence;
  }
  async function check(name, method, data, options = {}) {
    assert.ok(!record.cases.some(c => c.name === name), 'Duplicate case name: ' + name);
    const start = Date.now();
    try {
      const result = await api(method, data, options);
      record.cases.push({name, method, expectedStatus: options.expectedStatus, result: compact(result), elapsedMs: Date.now() - start,
        ...(result.status === options.expectedStatus ? {} : {assertionFailure: `status ${result.status} != expected ${options.expectedStatus}`})});
    }
    catch (error) { record.cases.push({name, method, infrastructureError: error.stack}); }
  }
  try {
    let ready = false;
    for (let i = 0; i < 300; ++i) {
      if (spawnError) throw spawnError;
      if (child.exitCode !== null) throw Error('Server exited: ' + child.exitCode);
      try { if ((await api('ListRooms')).status === 200) { ready = true; break; } } catch {}
      await sleep(100);
    }
    assert.ok(ready, 'Server not ready');
    assert.equal((await api('CreateRoom', {name: room})).status, 200);
    for (const binary of [false, true]) {
      const mode = binary ? 'protobuf' : 'json';
      // These expectations were checked against the pinned reference, not
      // inferred from the C++ response. Equal server failures cannot pass.
      const errorStatuses = new Map([
        ...['room metadata limit before permission', 'room metadata limit before missing room',
          'name and metadata validation order', 'create name too long', 'create metadata too large',
          'create name validation with egress', 'invalid nonce before missing room', 'nonce exact error',
          'rejected metadata write', 'malformed before permission', 'malformed without token',
          'JSON duplicate', 'JSON null', 'JSON array', 'JSON empty', 'JSON badType', 'JSON field alias duplicate',
          'JSON nested known duplicate', 'JSON map duplicate', 'JSON null then field duplicate'].map(n => [n, 400]),
        ...['participant name exact byte limit', 'attributes exact key plus value limit', 'metadata missing room',
          'unknown method without token', 'wrong verb', 'invalid content type', 'mime prefix is not media type'].map(n => [n, 404]),
        ['create checks permission before limits', 401], ['data missing room', 503],
      ]);
      for (const label of ['name', 'utf8Name', 'metadata', 'attributes'])
        for (const suffix of ['limit before permission', 'limit before participant lookup']) errorStatuses.set(`${label} ${suffix}`, 400);
      for (const method of Object.keys(methods)) {
        for (const grant of ['none', 'join', 'absent']) errorStatuses.set(`${method} rejects ${grant}`, 401);
        errorStatuses.set(`${method} wrong room`, 401);
        for (const suffix of ['missing participant', 'missing room'])
          errorStatuses.set(`${method} ${suffix}`, ['UpdateSubscriptions', 'MutePublishedTrack'].includes(method) ? 503 : 404);
      }
      const run = (name, method, data, options = {}) => check(`${mode}: ${name}`, method, data, {binary, expectedStatus: errorStatuses.get(name) || 200, ...options});
      for (const method of Object.keys(methods)) {
        for (const grant of ['none', 'join', 'absent'])
          await run(`${method} rejects ${grant}`, method, {room, name: room, identity: 'nobody'}, {grant, raw: binary ? methods[method][0].fromJson({room, name: room, identity: 'nobody'}, {ignoreUnknownFields: true}).toBinary() : undefined});
      }
      for (const method of ['ListParticipants', 'GetParticipant', 'RemoveParticipant', 'MutePublishedTrack', 'UpdateParticipant', 'UpdateSubscriptions', 'SendData', 'UpdateRoomMetadata', 'ForwardParticipant', 'MoveParticipant', 'PerformRpc'])
        await run(`${method} wrong room`, method, {room}, {grant: 'other'});
      await run('list grant can list', 'ListRooms', {names: [missing]}, {grant: 'list'});
      await run('create grant cannot list', 'ListRooms', {}, {grant: 'create', expectedStatus: 401});
      await run('room admin cannot list all rooms', 'ListRooms', {}, {grant: 'roomAdmin', expectedStatus: 401});
      await run('room admin cannot create', 'CreateRoom', {name: room}, {grant: 'roomAdmin', expectedStatus: 401});
      await run('room admin cannot delete', 'DeleteRoom', {room}, {grant: 'roomAdmin', expectedStatus: 401});
      await run('room admin can list participants', 'ListParticipants', {room}, {grant: 'roomAdmin'});
      for (const grant of ['create', 'list'])
        await run(`${grant} cannot list participants`, 'ListParticipants', {room}, {grant, expectedStatus: 401});
      await run('create grant can create', 'CreateRoom', {name: 'created-by-grant'}, {grant: 'create'});
      await run('create grant can delete', 'DeleteRoom', {room: 'created-by-grant'}, {grant: 'create'});
      await run('create grant deletion took effect', 'ListRooms', {names: ['created-by-grant']}, {grant: 'list'});
      await run('unknown room preset', 'CreateRoom', {name: 'rejected-preset', roomPreset: 'missing'}, {expectedStatus: 400});
      await run('unknown preset creates no room', 'ListRooms', {names: ['rejected-preset']});
      for (const method of ['GetParticipant', 'RemoveParticipant', 'UpdateParticipant', 'UpdateSubscriptions', 'MutePublishedTrack']) {
        await run(`${method} missing participant`, method, {room, identity: 'nobody'});
        await run(`${method} missing room`, method, {room: missing, identity: 'nobody'}, {grant: 'missing'});
      }
      await run('room metadata limit before permission', 'UpdateRoomMetadata', {room, metadata: 'x'.repeat(524289)}, {grant: 'none'});
      await run('room metadata limit before missing room', 'UpdateRoomMetadata', {room: missing, metadata: 'x'.repeat(524289)}, {grant: 'missing'});
      for (const [label, extra] of Object.entries({name: {name: 'x'.repeat(257)}, utf8Name: {name: '中'.repeat(86)}, metadata: {metadata: 'x'.repeat(524289)}, attributes: {attributes: {x: 'x'.repeat(65536)}}})) {
        await run(`${label} limit before permission`, 'UpdateParticipant', {room, identity: 'nobody', ...extra}, {grant: 'none'});
        await run(`${label} limit before participant lookup`, 'UpdateParticipant', {room, identity: 'nobody', ...extra});
      }
      await run('name and metadata validation order', 'UpdateParticipant', {room, name: 'x'.repeat(257), metadata: 'x'.repeat(524289)});
      await run('participant name exact byte limit', 'UpdateParticipant', {room, identity: 'nobody', name: 'x'.repeat(256)});
      await run('attributes exact key plus value limit', 'UpdateParticipant', {room, identity: 'nobody', attributes: {x: 'x'.repeat(65535)}});
      await run('create checks permission before limits', 'CreateRoom', {name: 'x'.repeat(257)}, {grant: 'none'});
      await run('create name too long', 'CreateRoom', {name: 'x'.repeat(257)});
      await run('create metadata too large', 'CreateRoom', {name: 'rejected', metadata: 'x'.repeat(524289)});
      await run('no room created by rejection', 'ListRooms', {names: ['rejected']});
      await run('create name validation with egress', 'CreateRoom', {name: 'x'.repeat(257), egress: {}});
      await run('invalid nonce before missing room', 'SendData', {room: missing, nonce: Buffer.from('bad').toString('base64')}, {grant: 'missing'});
      await run('data missing room', 'SendData', {room: missing}, {grant: 'missing'});
      await run('metadata missing room', 'UpdateRoomMetadata', {room: missing}, {grant: 'missing'});
      await run('data 60001 bytes accepted by API', 'SendData', {room, data: Buffer.alloc(60001).toString('base64')});
      await run('nonce exact error', 'SendData', {room, nonce: 'AQ=='});
      await run('metadata write', 'UpdateRoomMetadata', {room, metadata: 'retained'});
      await run('rejected metadata write', 'UpdateRoomMetadata', {room, metadata: 'x'.repeat(524289)});
      await run('rejection preserves metadata', 'ListRooms', {names: [room]});
      await run('metadata clear', 'UpdateRoomMetadata', {room});
      await run('malformed before permission', 'ListRooms', {}, {grant: 'none', raw: binary ? Buffer.from([10, 255]) : '{', codeOnly: true});
      await run('malformed without token', 'ListRooms', {}, {grant: 'absent', raw: binary ? Buffer.from([10, 255]) : '{', codeOnly: true});
      await run('unknown method without token', 'MissingMethod', {}, {grant: 'absent', raw: '{}', codeOnly: true});
      await run('wrong verb', 'ListRooms', {}, {verb: 'GET', codeOnly: true});
      await run('invalid content type', 'ListRooms', {}, {type: 'text/plain', codeOnly: true});
      await run('mime prefix is not media type', 'ListRooms', {}, {type: binary ? 'application/protobuf-bad' : 'application/json-bad', codeOnly: true});
      await run('mime parameter accepted', 'ListRooms', {names: [missing]}, {type: binary ? 'application/protobuf; charset=utf-8' : 'application/json; charset=utf-8'});
      if (binary) await run('protobuf unknown field', 'ListRooms', {}, {raw: Buffer.from([0xa0, 0x06, 1])});
      else {
        for (const [label, raw] of Object.entries({duplicate: '{"names":[],"names":[]}', trailing: '{} {}', null: 'null', array: '[]', empty: '', badType: '{"names":42}'}))
          await run(`JSON ${label}`, 'ListRooms', {}, {raw, codeOnly: true});
        await run('JSON unknown fields', 'ListRooms', {names: [missing], future: {x: [1, 2]}});
        await run('JSON duplicate unknown fields ignored', 'ListRooms', {}, {raw: '{"names":["missing-room"],"future":1,"future":2}'});
        await run('JSON field alias duplicate', 'CreateRoom', {}, {raw: '{"name":"rejected-alias","empty_timeout":30,"emptyTimeout":60}', codeOnly: true});
        await run('JSON nested known duplicate', 'UpdateParticipant', {}, {raw: '{"room":"contract-room","permission":{"canPublish":true,"can_publish":false}}', codeOnly: true});
        await run('JSON map duplicate', 'UpdateParticipant', {}, {raw: '{"room":"contract-room","attributes":{"key":"a","key":"b"}}', codeOnly: true});
        await run('JSON null then field duplicate', 'ListRooms', {}, {raw: '{"names":null,"names":[]}', codeOnly: true});
        await run('JSON null field', 'ListRooms', {names: null});
      }
    }
    record.completed = true;
  } catch (error) { record.infrastructureError = error.stack; }
  finally {
    if (child.exitCode === null && child.signalCode === null) { const exited = once(child, 'exit'); child.kill(); await exited; }
    writeFileSync(resolve(out, kind + '.log'), log); unlinkSync(configPath);
  }
  console.log(`${kind}: ${record.cases.length} cases recorded`);
}
try {
  await exercise('go', opts['go-server']); await exercise('cpp', opts['cpp-server']);
  for (const expected of report.servers.go.cases) {
    const actual = report.servers.cpp.cases.find(c => c.name === expected.name);
    let difference;
    try {
      assert.ok(!expected.infrastructureError && actual && !actual.infrastructureError, 'Request infrastructure failure');
      assert.ok(!expected.assertionFailure, 'Reference contract failure: ' + expected.assertionFailure);
      assert.ok(!actual.assertionFailure, 'C++ contract failure: ' + actual.assertionFailure);
      assert.deepEqual(actual.result.comparison, expected.result.comparison);
    } catch (error) { difference = error.message; }
    report.comparisons.push({name: expected.name, passed: !difference, ...(difference ? {difference} : {})});
    if (difference) console.log('FAIL ' + expected.name);
  }
  const infra = Object.values(report.servers).some(s => !s.completed || s.infrastructureError || s.cases.some(c => c.infrastructureError));
  process.exitCode = infra ? 2 : report.comparisons.length && report.comparisons.every(c => c.passed) ? 0 : 1;
} catch (error) { report.infrastructureError = error.stack; process.exitCode = 2; }
finally {
  report.exitCode = process.exitCode || 0;
  report.passed = report.comparisons.filter(c => c.passed).length;
  report.total = report.comparisons.length;
  writeFileSync(resolve(out, 'report.json'), JSON.stringify(report, null, 2));
  console.log(`${report.passed}/${report.total} matched; ${out}`);
}
