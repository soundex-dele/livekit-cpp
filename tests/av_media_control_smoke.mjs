// Real Go control-plane admission and C++ SFU media, using synthetic sources only.
// See av_media_control_smoke.md for the process/environment contract.
import assert from 'node:assert/strict';
import {once} from 'node:events';
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {createServer} from 'node:http';
import {dirname, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {chromium} from 'playwright';
import {captureRtcDiagnostics} from './rtc_diagnostics.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const api = process.env.AV_SMOKE_API?.replace(/\/$/, '');
const api2 = (process.env.AV_SMOKE_API2 || api)?.replace(/\/$/, '');
const bearer = process.env.AV_SMOKE_TOKEN;
const memberBearer = process.env.AV_SMOKE_MEMBER_TOKEN;
const organization = process.env.AV_SMOKE_ORGANIZATION;
assert.ok(api && bearer && organization,
  'AV_SMOKE_API, AV_SMOKE_TOKEN and AV_SMOKE_ORGANIZATION are required');
const output = resolve(process.env.AV_SMOKE_OUTPUT || resolve(here, 'results/av-control-media.json'));
const secrets = new Set([bearer, memberBearer]);
const report = {test: 'av-control-synthetic-media', started_at: new Date().toISOString(),
  source: 'synthetic canvas VP8 and oscillator Opus; no capture devices',
  checks: [], scenarios: {}, diagnostics: [], cleanup: [], phase: 'testing', passed: false};
const pages = [];
const delay = ms => new Promise(r => setTimeout(r, ms));
let browser;
let browserServer;
let session;
let guest;
let stopped = false;
let heartbeatTimer;
let heartbeatPending = Promise.resolve();
let heartbeatFailure;
let activeCleanup;
const sdk = readFileSync(resolve(here, 'node_modules/livekit-client/dist/livekit-client.umd.js'));
const assets = createServer((req, res) => {
  if (req.url === '/sdk.js') {
    res.writeHead(200, {'Content-Type': 'application/javascript'});
    res.end(sdk);
  } else {
    res.writeHead(200, {'Content-Type': 'text/html'});
    res.end('<!doctype html><html><head><title>AV synthetic media smoke</title></head>' +
      '<body><script src="/sdk.js"></script></body></html>');
  }
});

function safe(value) {
  let result = JSON.stringify(value);
  for (const secret of secrets) {
    if (secret) result = result.split(secret).join('[redacted]');
  }
  return JSON.parse(result.replace(/eyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+/g, '[redacted-jwt]'));
}
function pass(check) {
  report.checks.push(check);
  writeReport();
  console.log(`PASS: ${check}`);
}
function writeReport() {
  mkdirSync(dirname(output), {recursive: true});
  writeFileSync(output, JSON.stringify(safe(report), null, 2) + '\n');
}
async function bounded(work, label, milliseconds) {
  let timer;
  try {
    return await Promise.race([work, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(`${label} exceeded ${milliseconds} ms`)), milliseconds);
    })]);
  } finally {
    clearTimeout(timer);
  }
}
async function cleanupStep(label, work, milliseconds = 5000) {
  const entry = {step: label, state: 'running', started_at: new Date().toISOString()};
  report.cleanup.push(entry);
  writeReport();
  console.log(`CLEANUP: ${label}`);
  const started = Date.now();
  try {
    await bounded(Promise.resolve().then(work), label, milliseconds);
    entry.state = 'succeeded';
  } catch (error) {
    entry.state = 'failed';
    entry.error = safe(String(error));
  }
  entry.elapsed_ms = Date.now() - started;
  writeReport();
  return entry.state === 'succeeded';
}
function exited(child) {
  return child.exitCode !== null || child.signalCode !== null;
}
async function waitForExit(child, milliseconds) {
  if (exited(child)) return true;
  return new Promise(resolveExit => {
    const finish = result => {
      clearTimeout(timer);
      child.off('exit', onExit);
      resolveExit(result);
    };
    const onExit = () => finish(true);
    const timer = setTimeout(() => finish(exited(child)), milliseconds);
    child.once('exit', onExit);
  });
}
async function closeOwnedBrowser() {
  const child = browserServer.process();
  const evidence = report.owned_browser = {pid: child.pid, method: 'graceful', actual_exit: false};
  // Playwright waits for ChildProcess.close, which can lag exit on Windows if
  // inherited stdio remains open. Observe the actual owned process independently.
  const close = browserServer.close().then(() => true, error => {
    evidence.driver_error = safe(String(error));
    return false;
  });
  if (!await waitForExit(child, 15000)) {
    evidence.method = 'native_kill_after_graceful_timeout';
    child.kill('SIGKILL');
    if (!await waitForExit(child, 15000)) throw new Error(`Owned browser PID ${child.pid} did not exit`);
  }
  evidence.actual_exit = exited(child);
  evidence.exit_code = child.exitCode;
  evidence.exit_signal = child.signalCode;
  let driverClosed = await bounded(close, 'browser driver close', 1000).catch(() => false);
  if (!driverClosed) {
    // Safe only after actual process exit; never break pipes of a running app.
    evidence.exited_process_stdio_released = true;
    for (const stream of child.stdio) stream?.destroy?.();
    driverClosed = await bounded(close, 'browser driver close after stdio release', 10000).catch(() => false);
  }
  evidence.driver_cleanup_completed = driverClosed;
  assert.ok(evidence.actual_exit, 'owned browser must actually exit');
  assert.ok(driverClosed, 'browser driver cleanup must complete after actual process exit');
}
async function request(path, {principal = 'user', base = api, body = {}, method = 'POST'} = {}) {
  const headers = {'Content-Type': 'application/json', 'X-Foundation-Request': '1'};
  if (principal === 'user' || principal === 'member') {
    headers.Authorization = `Bearer ${principal === 'member' ? memberBearer : bearer}`;
    headers['X-Organization-ID'] = organization;
  } else if (principal === 'guest') {
    headers['X-AV-Resource-Credential'] = guest;
  }
  const response = await fetch(`${base}/api/v1/av/${path}`, {
    method, headers, body: method === 'GET' ? undefined : JSON.stringify(body),
    signal: AbortSignal.timeout(8000),
  });
  const data = await response.json();
  return {status: response.status, data};
}
async function ok(path, options) {
  const result = await request(path, options);
  assert.equal(result.status, 200, `${path}: HTTP ${result.status}, ${result.data.error?.code || 'unknown error'}`);
  return result.data;
}
async function eventually(fn, description, timeout = 20000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    if (heartbeatFailure) throw heartbeatFailure;
    const result = await bounded(Promise.resolve().then(fn), `${description} probe`, 8000);
    if (result) return result;
    await delay(150);
  }
  throw new Error(`Timed out waiting for ${description}`);
}
async function credentials(grantID, principal, base) {
  return eventually(async () => {
    const result = await request(`rtc-grants/${grantID}/credentials`, {principal, base});
    if (result.status === 409 && result.data.error?.code === 'media_preparing') return false;
    assert.equal(result.status, 200, `grant credentials: HTTP ${result.status}, ${result.data.error?.code}`);
    secrets.add(result.data.media_token);
    assert.equal(result.data.grant_id, grantID);
    assert.ok(result.data.media_url && result.data.media_token);
    return result.data;
  }, 'worker provisioning media room', 30000);
}
async function connect(label, credentials, origin) {
  const context = await browser.newContext();
  const page = await context.newPage();
  pages.push({label, page});
  page.on('pageerror', error => report.diagnostics.push({label, error: safe(String(error))}));
  await page.goto(origin);
  await page.evaluate(async ({url, token}) => {
    const {Room, RoomEvent} = LivekitClient;
    window.events = [];
    window.received = [];
    window.tracks = new Map();
    window.sourceTracks = [];
    window.timers = [];
    window.room = new Room({adaptiveStream: false, dynacast: false,
      publishDefaults: {simulcast: false, videoCodec: 'vp8', red: false}});
    room.on(RoomEvent.TrackSubscribed, (track, publication) => {
      tracks.set(publication.trackSid, track);
      const element = track.attach();
      element.autoplay = true;
      document.body.appendChild(element);
      events.push({type: 'subscribed', kind: track.kind, source: publication.source});
    });
    room.on(RoomEvent.TrackUnsubscribed, (_track, publication) => tracks.delete(publication.trackSid));
    room.on(RoomEvent.Disconnected, reason => events.push({type: 'disconnected', reason}));
    room.on(RoomEvent.DataReceived, (bytes, participant, _kind, topic) => {
      received.push({text: new TextDecoder().decode(bytes), identity: participant?.identity, topic});
    });
    await room.connect(url, token);
  }, {url: credentials.media_url, token: credentials.media_token});
  assert.equal(await page.evaluate(() => room.localParticipant.identity), credentials.participant_identity);
  return page;
}
async function publishSynthetic(page, screen = true, withAudio = true) {
  await page.evaluate(async ({screen, withAudio}) => {
    const canvas = document.createElement('canvas');
    canvas.width = 320;
    canvas.height = 240;
    document.body.appendChild(canvas);
    const ctx = canvas.getContext('2d');
    let frame = 0;
    timers.push(setInterval(() => {
      ctx.fillStyle = `hsl(${frame++ % 360},80%,50%)`;
      ctx.fillRect(0, 0, 320, 240);
      ctx.fillStyle = 'white';
      ctx.font = '30px sans-serif';
      ctx.fillText(`AV ${frame}`, 40, 80);
    }, 66));
    const video = canvas.captureStream(15).getVideoTracks()[0];
    sourceTracks.push(video);
    await room.localParticipant.publishTrack(video, {
      source: screen ? LivekitClient.Track.Source.ScreenShare : LivekitClient.Track.Source.Camera,
      simulcast: false, videoCodec: 'vp8',
    });
    if (!withAudio) return;
    window.audioContext = new AudioContext();
    const oscillator = audioContext.createOscillator();
    oscillator.frequency.value = 440;
    const gain = audioContext.createGain();
    gain.gain.value = 0.1;
    const destination = audioContext.createMediaStreamDestination();
    oscillator.connect(gain).connect(destination);
    oscillator.start();
    await audioContext.resume();
    const audio = destination.stream.getAudioTracks()[0];
    sourceTracks.push(audio);
    await room.localParticipant.publishTrack(audio, {
      source: screen ? LivekitClient.Track.Source.ScreenShareAudio : LivekitClient.Track.Source.Microphone,
      red: false,
    });
  }, {screen, withAudio});
}
async function mediaStats(page) {
  return page.evaluate(async () => {
    const result = [];
    for (const track of tracks.values()) {
      const stats = await track.getRTCStatsReport();
      stats?.forEach(stat => {
        if (stat.type === 'inbound-rtp') result.push({id: stat.id, kind: stat.kind,
          bytes: stat.bytesReceived || 0, frames: stat.framesDecoded || 0,
          energy: stat.totalAudioEnergy || 0, samples: stat.totalSamplesReceived || 0});
      });
    }
    return result;
  });
}
async function deniedOldToken(value) {
  const url = new URL('/rtc/validate', value.media_url.replace(/^ws/, 'http'));
  const response = await fetch(url, {
    headers: {Authorization: `Bearer ${value.media_token}`}, signal: AbortSignal.timeout(8000),
  });
  assert.ok([401, 403].includes(response.status), `old media token must be rejected: HTTP ${response.status}`);
  return response.status;
}

function useGuest(value) {
  guest = value.credential;
  assert.ok(guest, 'public join must issue a scoped guest credential');
  secrets.add(guest);
}
async function startHeartbeat(path, extra = async () => {}, principals = ['user', 'guest']) {
  heartbeatFailure = undefined;
  const beat = async () => {
    for (const principal of principals) await ok(`${path}/heartbeat`, {principal});
    await extra();
  };
  await beat();
  heartbeatPending = Promise.resolve();
  heartbeatTimer = setInterval(() => {
    heartbeatPending = heartbeatPending.then(beat).catch(error => { heartbeatFailure = error; });
  }, 3000);
}
async function stopHeartbeat() {
  clearInterval(heartbeatTimer);
  heartbeatTimer = undefined;
  await heartbeatPending;
  if (heartbeatFailure) throw heartbeatFailure;
}
async function assertMedia(page, description, withAudio = true) {
  const before = await eventually(async () => {
    const stats = await mediaStats(page);
    return stats.some(s => s.kind === 'video' && s.frames > 5 && s.bytes > 500) &&
      (!withAudio || stats.some(s => s.kind === 'audio' && s.bytes > 500 && s.energy > 0.001 && s.samples > 0))
      ? stats : false;
  }, `${description}: decoded video${withAudio ? ' and non-silent audio' : ''}`, 45000);
  const after = await eventually(async () => {
    const stats = await mediaStats(page);
    return before.every(previous => stats.some(current => current.id === previous.id &&
      current.bytes > previous.bytes && (current.kind === 'video'
        ? current.frames >= previous.frames + 5 : current.energy > previous.energy && current.samples > previous.samples)))
      ? stats : false;
  }, `${description}: continued media progression`);
  return {before, after};
}
async function assertRevocation(peers, revoke) {
  const fresh = [];
  for (const peer of peers) fresh.push(await credentials(peer.grant_id, peer.principal, api));
  await stopHeartbeat();
  const started = Date.now();
  await revoke();
  const results = [];
  for (let index = 0; index < peers.length; ++index) {
    const peer = peers[index];
    const denied = await request(`rtc-grants/${peer.grant_id}/credentials`, {principal: peer.principal, base: api2});
    assert.equal(denied.status, 403, 'revoked grant cannot issue RTC credentials');
    results.push({credential_status: denied.status, validation_status: await deniedOldToken(fresh[index])});
  }
  await eventually(async () => {
    const disconnected = await Promise.all(peers.map(({page}) => page.evaluate(() =>
      room.state === 'disconnected' && events.some(event => event.type === 'disconnected'))));
    return disconnected.every(Boolean);
  }, 'all revoked clients disconnected', 25000);
  assert.ok(fresh.every(value => Date.parse(value.expires_at) > Date.now()), 'revocation precedes JWT expiry');
  return {elapsed_ms: Date.now() - started, peers: results, all_disconnected: true, before_token_expiry: true};
}
async function closeClients(clients) {
  for (const page of clients) {
    const label = pages.find(entry => entry.page === page)?.label || 'client';
    if (page.isClosed()) continue;
    await cleanupStep(`${label}: stop synthetic sources`, () => page.evaluate(() => {
      for (const timer of window.timers || []) clearInterval(timer);
      for (const track of window.sourceTracks || []) track.stop();
      // Closing a context releases WebRTC. Do not await SDK disconnect again
      // after the revocation assertion already observed Disconnected.
      if (window.audioContext?.state !== 'closed') void window.audioContext?.close();
    }), 3000);
    await cleanupStep(`${label}: close browser context`, () => page.context().close(), 8000);
  }
}

async function conferenceScenario(origin) {
  const meeting = await ok('conference/meetings', {body: {title: 'synthetic conference', waiting_room: true}});
  const path = `conference/meetings/${meeting.id}`;
  activeCleanup = `${path}/end`;
  await ok(`${path}/heartbeat`);
  const joined = await ok('conference/join', {
    principal: 'public', base: api2, body: {number: meeting.number, name: 'Synthetic conference guest'},
  });
  useGuest(joined);
  assert.equal(joined.state, 'waiting');
  assert.equal((await request(`${path}/connect`, {principal: 'guest'})).status, 403);
  await startHeartbeat(path);
  await ok(`${path}/admit`, {body: {target: joined.subject_id}});
  const hostGrant = await ok(`${path}/connect`);
  const guestGrant = await ok(`${path}/connect`, {principal: 'guest', base: api2});
  const host = await connect('conference-host', await credentials(hostGrant.grant_id, 'user', api2), origin);
  const attendee = await connect('conference-guest', await credentials(guestGrant.grant_id, 'guest', api), origin);
  await publishSynthetic(host, false);
  await publishSynthetic(attendee, false);
  const hostMedia = await assertMedia(host, 'conference host receives guest');
  const guestMedia = await assertMedia(attendee, 'conference guest receives host');
  const revocation = await assertRevocation([
    {page: host, grant_id: hostGrant.grant_id, principal: 'user'},
    {page: attendee, grant_id: guestGrant.grant_id, principal: 'guest'},
  ], () => ok(`${path}/end`, {base: api2}));
  activeCleanup = undefined;
  report.scenarios.conference = {hostMedia, guestMedia, revocation, waiting_room_enforced: true};
  pass('conference waiting-room admission, bidirectional decoded audio/video and end revocation');
  await closeClients([host, attendee]);
}

async function liveScenario(origin) {
  const liveRoom = await ok('live-studio/rooms', {body: {title: 'synthetic live studio'}});
  const broadcast = await ok(`live-studio/rooms/${liveRoom.id}/start`);
  const path = `live-studio/broadcasts/${broadcast.id}`;
  activeCleanup = `${path}/end`;
  await ok(`${path}/heartbeat`);
  const watched = await ok(`${path}/watch`, {principal: 'public', base: api2});
  useGuest(watched);
  await startHeartbeat(path);
  const publisherCredentials = await credentials(broadcast.grant_id, 'user', api2);
  const viewerCredentials = await credentials(watched.grant_id, 'guest', api);
  const publisher = await connect('live-program-publisher', publisherCredentials, origin);
  const viewer = await connect('live-program-viewer', viewerCredentials, origin);
  await publishSynthetic(publisher, false);
  const program = await assertMedia(viewer, 'live program viewer');
  const peers = [
    {page: publisher, grant_id: broadcast.grant_id, principal: 'user'},
    {page: viewer, grant_id: watched.grant_id, principal: 'guest'},
  ];
  let stage = {covered: false, reason: 'AV_SMOKE_MEMBER_TOKEN not provided'};
  if (memberBearer) {
    const member = await ok(`${path}/watch`, {principal: 'member'});
    await ok(`${path}/stage-request`, {principal: 'member'});
    await ok(`${path}/stage-approve`, {body: {target: member.subject_id}});
    await stopHeartbeat();
    await startHeartbeat(path, async () => {}, ['user', 'guest', 'member']);
    const access = await ok(`${path}/connect`, {principal: 'member', body: {stage: true}, base: api2});
    const hostStageCredentials = await credentials(broadcast.stage_grant_id, 'user', api);
    const memberCredentials = await credentials(access.grant_id, 'member', api2);
    assert.notEqual(hostStageCredentials.room, publisherCredentials.room, 'interactive stage and program have separate bindings');
    assert.equal(hostStageCredentials.room, memberCredentials.room);
    const host = await connect('live-stage-host', hostStageCredentials, origin);
    const memberPage = await connect('live-stage-member', memberCredentials, origin);
    await publishSynthetic(host, false);
    await publishSynthetic(memberPage, false);
    stage = {covered: true, hostMedia: await assertMedia(host, 'stage host receives member'),
      memberMedia: await assertMedia(memberPage, 'stage member receives host')};
    assert.deepEqual(await viewer.evaluate(() => [...room.remoteParticipants.keys()]),
      [publisherCredentials.participant_identity], 'program viewer cannot see private stage participants');
    peers.push({page: host, grant_id: broadcast.stage_grant_id, principal: 'user'},
      {page: memberPage, grant_id: access.grant_id, principal: 'member'});
  }
  const revocation = await assertRevocation(peers, () => ok(`${path}/end`, {base: api2}));
  activeCleanup = undefined;
  report.scenarios.live = {program, stage, revocation};
  pass(`live program audio/video${stage.covered ? ', isolated bidirectional stage' : ''} and broadcast end revocation`);
  await closeClients(peers.map(peer => peer.page));
}

async function assistScenario(origin) {
  const value = await ok('remote-control/sessions', {body: {title: 'synthetic remote assist'}});
  secrets.add(value.pairing_code);
  const path = `remote-control/sessions/${value.id}`;
  activeCleanup = `${path}/stop`;
  useGuest(await ok('remote-control/redeem', {
    principal: 'public', base: api2, body: {code: value.pairing_code, name: 'Synthetic controller'},
  }));
  let lease;
  const renew = async () => {
    if (lease) await ok(`control-grants/${lease.id}/renew`, {body: {epoch: lease.epoch}});
  };
  await startHeartbeat(path, renew);
  const requested = await ok(`${path}/request`, {principal: 'guest'});
  await ok(`control-grants/${requested.id}/approve`, {body: {local_consent: true, epoch: requested.epoch}});
  lease = requested;
  await renew();
  const hostGrant = await ok(`${path}/connect`, {body: {epoch: lease.epoch}});
  const controllerGrant = await ok(`${path}/connect`, {principal: 'guest', body: {epoch: lease.epoch}});
  const hostCredentials = await credentials(hostGrant.grant_id, 'user', api2);
  const controllerCredentials = await credentials(controllerGrant.grant_id, 'guest', api);
  const host = await connect('assist-recording-host', hostCredentials, origin);
  const controller = await connect('assist-controller', controllerCredentials, origin);
  await publishSynthetic(host, true, false);
  const screen = await assertMedia(controller, 'assist controller screen', false);
  const input = {v: 1, type: 'key', grant_id: lease.id, epoch: lease.epoch, seq: 1,
    layout_version: 1, code: 'KeyA', down: true};
  await controller.evaluate(async ({input, destination}) => {
    await room.localParticipant.publishData(new TextEncoder().encode(JSON.stringify(input)), {
      reliable: true, topic: 'av.remote-control.v1', destinationIdentities: [destination],
    });
  }, {input, destination: hostCredentials.participant_identity});
  const recorded = await eventually(() => host.evaluate(({input, identity}) => {
    return received.find(message => message.topic === 'av.remote-control.v1' && message.identity === identity &&
      message.text === JSON.stringify(input));
  }, {input, identity: controllerCredentials.participant_identity}), 'authenticated synthetic input record');
  // The fixture records and echoes bytes. It deliberately has no OS input sink.
  await host.evaluate(async ({text, destination}) => {
    await room.localParticipant.publishData(new TextEncoder().encode(text), {
      reliable: true, topic: 'av.remote-control.smoke-ack', destinationIdentities: [destination],
    });
  }, {text: recorded.text, destination: controllerCredentials.participant_identity});
  await eventually(() => controller.evaluate(({text, identity}) => received.some(message =>
    message.topic === 'av.remote-control.smoke-ack' && message.identity === identity && message.text === text),
  {text: recorded.text, identity: hostCredentials.participant_identity}), 'synthetic input acknowledgement');
  const revocation = await assertRevocation([
    {page: host, grant_id: hostGrant.grant_id, principal: 'user'},
    {page: controller, grant_id: controllerGrant.grant_id, principal: 'guest'},
  ], () => ok(`control-grants/${lease.id}/revoke`, {body: {epoch: lease.epoch}, base: api2}));
  await ok(`${path}/stop`);
  activeCleanup = undefined;
  report.scenarios.assist = {screen, input_recorded_and_echoed: true, os_input_injected: false, revocation};
  pass('approved assist screen video, authenticated synthetic input record/echo and control-grant revocation');
  await closeClients([host, controller]);
}

try {
  assets.listen(0, '127.0.0.1');
  await once(assets, 'listening');
  const origin = `http://127.0.0.1:${assets.address().port}`;
  browserServer = await chromium.launchServer({headless: true, host: '127.0.0.1',
    ...(process.env.LK_CHROME ? {executablePath: process.env.LK_CHROME} : {channel: 'chrome'}),
    args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling'],
  });
  browser = await chromium.connect(browserServer.wsEndpoint());
  report.owned_browser_pid = browserServer.process().pid;
  report.browser = browser.version();
  session = await ok('screen-cast/sessions', {body: {title: 'synthetic-media-smoke'}});
  secrets.add(session.pairing_code);
  const sender = await ok('screen-cast/redeem', {
    base: api2, principal: 'public', body: {code: session.pairing_code, name: 'Synthetic sender'},
  });
  guest = sender.credential;
  secrets.add(guest);
  assert.ok(guest, 'pairing must issue a scoped resource guest credential');
  assert.equal(sender.id, session.id);
  const path = `screen-cast/sessions/${session.id}`;
  const heartbeat = async () => {
    await ok(`${path}/heartbeat`);
    await ok(`${path}/heartbeat`, {principal: 'guest', base: api2});
  };
  await heartbeat();
  heartbeatTimer = setInterval(() => {
    heartbeatPending = heartbeatPending.then(heartbeat).catch(error => { heartbeatFailure = error; });
  }, 3000);
  const receiverGrant = await ok(`${path}/connect`);
  const senderGrant = await ok(`${path}/connect`, {principal: 'guest', base: api2});
  assert.equal(receiverGrant.role, 'receiver');
  assert.equal(senderGrant.role, 'sender');
  const receiverCredentials = await credentials(receiverGrant.grant_id, 'user', api2);
  const senderCredentials = await credentials(senderGrant.grant_id, 'guest', api);
  assert.equal(receiverCredentials.room, senderCredentials.room);
  assert.notEqual(receiverCredentials.participant_identity, senderCredentials.participant_identity);
  const receiver = await connect('receiver', receiverCredentials, origin);
  const publisher = await connect('sender', senderCredentials, origin);
  pass('two clients joined using only real Go-issued grants and SFU admission across API replicas');
  await publishSynthetic(publisher);
  const before = await eventually(async () => {
    const stats = await mediaStats(receiver);
    return stats.some(s => s.kind === 'video' && s.frames > 5 && s.bytes > 500) &&
      stats.some(s => s.kind === 'audio' && s.bytes > 500 && s.energy > 0.001 && s.samples > 0)
      ? stats : false;
  }, 'decoded video and non-silent Opus audio', 45000);
  const after = await eventually(async () => {
    const stats = await mediaStats(receiver);
    return before.every(previous => stats.some(current => current.id === previous.id &&
      current.bytes > previous.bytes && (current.kind === 'video'
        ? current.frames >= previous.frames + 5 : current.energy > previous.energy && current.samples > previous.samples)))
      ? stats : false;
  }, 'continued video frame and audio sample progression');
  report.media = {before, after};
  pass('synthetic screen video decoded and non-silent screen audio received with progressing RTP statistics');

  // Renew through the control API immediately before revocation: expiry cannot explain denial.
  const currentReceiver = await credentials(receiverGrant.grant_id, 'user', api);
  const currentSender = await credentials(senderGrant.grant_id, 'guest', api2);
  clearInterval(heartbeatTimer);
  heartbeatTimer = undefined;
  await heartbeatPending;
  if (heartbeatFailure) throw heartbeatFailure;
  const revokeStart = Date.now();
  await ok(`${path}/stop`, {base: api2});
  stopped = true;
  const denied = await request(`rtc-grants/${senderGrant.grant_id}/credentials`, {principal: 'guest'});
  assert.equal(denied.status, 403, 'stopped session cannot issue a new RTC token');
  const validate = await Promise.all([deniedOldToken(currentReceiver), deniedOldToken(currentSender)]);
  await eventually(async () => {
    const states = await Promise.all([receiver, publisher].map(page => page.evaluate(() =>
      room.state === 'disconnected' && events.some(event => event.type === 'disconnected'))));
    return states.every(Boolean);
  }, 'both media clients disconnected after business stop', 25000);
  assert.ok(Date.parse(currentSender.expires_at) > Date.now(), 'revocation must precede JWT expiry');
  report.revocation = {elapsed_ms: Date.now() - revokeStart, credential_status: denied.status,
    validation_statuses: validate, both_disconnected: true, before_token_expiry: true};
  pass('business stop denied still-valid media tokens and disconnected both clients');
  report.scenarios.cast = {media: report.media, revocation: report.revocation};
  await closeClients([receiver, publisher]);
  await conferenceScenario(origin);
  await liveScenario(origin);
  await assistScenario(origin);
  report.media_assertions_passed = true;
  writeReport();
} catch (error) {
  report.error = safe(String(error?.stack || error));
  writeReport();
  await Promise.all(pages.filter(({page}) => !page.isClosed()).map(async ({label, page}) => {
    try {
      const diagnostic = await bounded(page.evaluate(async () => ({events: window.events || [],
        state: window.room?.state})), `${label}: diagnostic state`, 2000);
      const rtc = await bounded(page.evaluate(captureRtcDiagnostics), `${label}: RTC diagnostics`, 2000);
      report.diagnostics.push({label, rtc: safe(rtc), ...diagnostic});
    } catch (diagnosticError) { report.diagnostics.push({label, error: safe(String(diagnosticError))}); }
  }));
  process.exitCode = 1;
} finally {
  report.phase = 'cleanup';
  writeReport();
  clearInterval(heartbeatTimer);
  await cleanupStep('finish pending heartbeats', () => heartbeatPending, 10000);
  if (session && !stopped) {
    await cleanupStep('stop unfinished cast', () => ok(`screen-cast/sessions/${session.id}/stop`), 10000);
  }
  if (activeCleanup) {
    await cleanupStep('stop unfinished business resource', () => ok(activeCleanup), 10000);
  }
  await closeClients(pages.map(({page}) => page).filter(page => !page.isClosed()));
  if (browser) await cleanupStep('disconnect browser client', () => browser.close(), 8000);
  if (browserServer) {
    await cleanupStep('close owned browser process and verify exit', closeOwnedBrowser, 45000);
  }
  await cleanupStep('close local SDK HTTP server', () => new Promise(resolveClose => {
    assets.close(resolveClose);
    assets.closeAllConnections();
  }), 3000);
  report.phase = 'finished';
  report.passed = report.media_assertions_passed === true && report.cleanup.every(entry => entry.state === 'succeeded');
  if (!report.passed) process.exitCode = 1;
  report.finished_at = new Date().toISOString();
  writeReport();
  console.log(JSON.stringify({passed: report.passed, checks: report.checks.length, output}));
  if (!report.passed) console.error(report.error || 'One or more fixture cleanup steps failed; see report.cleanup');
  // Every owned resource has now either closed or exhausted an explicit bound.
  // Do not let a stuck Playwright transport suppress the persisted failure report.
  process.exit(process.exitCode || 0);
}
