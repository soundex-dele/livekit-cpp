// Raw protocol sessions exercise AV join/resume/refresh without a browser or
// capture devices. The admission stub and SFU are isolated child processes.
import assert from 'node:assert/strict';
import {createServer} from 'node:http';
import {createServer as tcpServer} from 'node:net';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {createHmac} from 'node:crypto';
import {mkdtemp, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve, sep} from 'node:path';
import {SignalRequest, SignalResponse} from '@livekit/protocol';

const binary = process.argv[process.argv.indexOf('--binary') + 1];
assert.ok(binary && binary !== process.argv[0], 'Pass --binary path/to/livekit-server');
const delay = ms => new Promise(done => setTimeout(done, ms));
async function eventually(predicate, message, timeout = 5000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) { if (await predicate()) return; await delay(25); }
  throw new Error(message);
}
async function port() {
  const server = tcpServer(); server.listen(0, '127.0.0.1'); await once(server, 'listening');
  const value = server.address().port; await new Promise(done => server.close(done)); return value;
}
const rtcSecret = 'rtc-secret-' + 'r'.repeat(32), managementSecret = 'management-secret-' + 'm'.repeat(32);
const admissionSecret = 'admission-secret-' + 'a'.repeat(32);
function token(issuer, secret, fields) {
  const encode = value => Buffer.from(JSON.stringify(value)).toString('base64url');
  const now = Math.floor(Date.now() / 1000);
  const body = encode({alg:'HS256',typ:'JWT'}) + '.' + encode({iss:issuer,nbf:now-1,exp:now+60,...fields});
  return body + '.' + createHmac('sha256',secret).update(body).digest('base64url');
}
let allowed = true, calls = 0, processLog = '';
const grantExpires = Math.floor(Date.now()/1000)+300;
const admission = createServer(async(req,res) => {
  let raw=''; for await(const part of req) raw+=part;
  const value=JSON.parse(raw); calls++;
  res.setHeader('Content-Type','application/json');
  res.end(JSON.stringify({allowed:allowed && req.headers.authorization==='Bearer '+admissionSecret,
    policy_version:value.policy_version,binding_generation:value.binding_generation,expires_at:grantExpires}));
});
admission.listen(0,'127.0.0.1'); await once(admission,'listening');
const mediaPort=await port(), base=`http://127.0.0.1:${mediaPort}`;
const directory=await mkdtemp(join(tmpdir(),'av-signal-'));
await writeFile(join(directory,'config.json'),JSON.stringify({bind:'127.0.0.1',port:mediaPort,
  keys:{'a-management':managementSecret,'z-rtc':rtcSecret}}));
const env={...process.env,AV_MEDIA_MODE:'true',AV_NODE_ID:'signal-node',AV_RTC_KEY:'z-rtc',
  AV_MANAGEMENT_KEY:'a-management',AV_ADMISSION_SECRET:admissionSecret,AV_ALLOW_INSECURE_ADMISSION:'true',
  AV_ADMISSION_URL:`http://127.0.0.1:${admission.address().port}/check`};
delete env.LIVEKIT_KEYS; delete env.LIVEKIT_CONFIG;
let child;
const sockets=[];
async function request(path,credential,body) {
  const response=await fetch(base+path,{method:body?'POST':'GET',headers:{Authorization:'Bearer '+credential,
    'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});
  const result=await response.json(); assert.equal(response.status,200,JSON.stringify(result)); return result;
}
function connect(jwt,reconnect=false) {
  const socket=new WebSocket(`${base.replace('http:','ws:')}/rtc?protocol=16&sdk=js&version=2.15.6&access_token=${jwt}${reconnect?'&reconnect=1':''}`);
  socket.binaryType='arraybuffer';
  const value={socket,messages:[],closed:false,error:null}; sockets.push(value);
  socket.addEventListener('message',event=>{try{value.messages.push(SignalResponse.fromBinary(new Uint8Array(event.data)));}catch(e){value.error=e;}});
  socket.addEventListener('error',event=>{value.error=event.message||'socket error';});
  socket.addEventListener('close',()=>value.closed=true);
  value.heartbeat=setInterval(()=>{if(socket.readyState===WebSocket.OPEN){socket.send(new SignalRequest({message:{case:'ping',value:BigInt(Date.now())}}).toBinary());}},1000);
  return value;
}
const messages=(client,type)=>client.messages.filter(m=>m.message.case===type).map(m=>m.message.value);
try {
  child=spawn(resolve(binary),['--config',join(directory,'config.json')],{env,windowsHide:true});
  child.stdout.on('data',b=>processLog+=b); child.stderr.on('data',b=>processLog+=b);
  await eventually(async()=>{try{return (await fetch(base+'/healthz')).ok;}catch{return false;}},'SFU not ready: '+processLog,15000);
  const admin=token('a-management',managementSecret,{video:{roomList:true,roomCreate:true,roomAdmin:true,room:'signal-room'}});
  const node=await request('/internal/v1/media/node',admin);
  await request('/internal/v1/media/operations',admin,{operation_id:'create-signal',node_id:node.node_id,
    node_incarnation:node.node_incarnation,room:'signal-room',binding_generation:1,target_policy_revision:1,
    method:'CreateRoom',payload:{name:'signal-room'}});
  const fields={sub:'signal-participant',av_grant:'signal-grant',av_policy_version:1,av_binding_generation:1,
    av_node_id:node.node_id,av_node_incarnation:node.node_incarnation,av_grant_expires_at:grantExpires,
    video:{roomJoin:true,room:'signal-room',canPublish:false,canPublishData:false}};
  const initial=connect(token('z-rtc',rtcSecret,fields));
  await eventually(()=>messages(initial,'join').length && messages(initial,'refreshToken').length,'join/refresh response missing');
  const refreshed=messages(initial,'refreshToken').at(-1), payload=JSON.parse(Buffer.from(refreshed.split('.')[1],'base64url'));
  assert.equal(payload.iss,'z-rtc'); assert.equal(payload.av_grant,fields.av_grant);
  assert.equal(payload.av_policy_version,1); assert.equal(payload.av_node_incarnation,node.node_incarnation);
  assert.ok(payload.exp<=Math.floor(Date.now()/1000)+60 && payload.exp<=grantExpires);
  const mismatch=connect(token('z-rtc',rtcSecret,{...fields,av_policy_version:2}),true);
  await eventually(()=>mismatch.closed || messages(mismatch,'leave').length,'resume accepted changed grant generation');
  assert.equal(messages(mismatch,'reconnect').length,0);
  assert.equal(initial.closed,false,'rejected replacement disconnected authorized participant');
  const resumed=connect(refreshed,true);
  await eventually(()=>messages(resumed,'reconnect').length,'same authorization failed to resume');
  await eventually(()=>messages(resumed,'refreshToken').length,'resumed connection did not refresh credentials');
  const before=calls; allowed=false;
  await eventually(()=>resumed.closed || messages(resumed,'leave').length,'periodic admission denial did not remove participant',25000);
  assert.ok(calls>before,'refresh did not query current authorization');
  console.log('AV join, refresh signer/TTL, resume generation and periodic revocation contracts passed');
} catch(error) {
  console.error(processLog); throw error;
} finally {
  for(const value of sockets){clearInterval(value.heartbeat);if(value.socket.readyState===WebSocket.OPEN)value.socket.close();}
  if(child){child.kill();await Promise.race([once(child,'exit'),delay(5000)]);}
  await new Promise(done=>admission.close(done));
  assert.ok(resolve(directory).startsWith(resolve(tmpdir())+sep),'temporary cleanup escaped its root');
  await rm(directory,{recursive:true,force:true});
}
