// RoomService mute policy, JSON/protobuf errors and real SDK side effects.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {createServer as tcpServer} from 'node:net';
import {createRequire} from 'node:module';
import {once} from 'node:events';
import {randomBytes,createHash} from 'node:crypto';
import {mkdirSync,readFileSync,writeFileSync,unlinkSync} from 'node:fs';
import {resolve,dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
const here=dirname(fileURLToPath(import.meta.url));
const opts=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(v.startsWith('--'))a.push([v.slice(2),all[i+1]]);return a;},[]));
assert.ok(opts['go-server']&&opts['cpp-server'],'Pass --go-server and --cpp-server');
const policy=opts['remote-unmute']||'default';assert.ok(['default','true','false'].includes(policy));
const enabled=policy==='true';
const deps=resolve(opts['node-modules']||resolve(here,'node_modules'));
const modern=resolve(opts['modern-node-modules']||'D:/workspace/livekit-cli-cpp/web/node_modules');
const require=createRequire(resolve(deps,'../package.json'));
const {chromium}=require('playwright');const {AccessToken,RoomServiceClient}=require('livekit-server-sdk');
const {MuteRoomTrackRequest,MuteRoomTrackResponse}=require('@livekit/protocol');
const out=resolve(opts.output||resolve(here,'results/go-mute-parity'),new Date().toISOString().replaceAll(/[:.]/g,'-')+'-'+randomBytes(3).toString('hex'));
mkdirSync(out,{recursive:true});
const report={fullReplacementVerified:false,scope:'remote mute policy, JSON/protobuf and SDK mute/media side effects',policy,servers:{},comparisons:[]};
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function eventually(fn,label,timeout=20000){const end=Date.now()+timeout;while(Date.now()<end){if(await fn())return;await delay(100);}throw Error('Timed out: '+label);}
async function port(){const s=tcpServer();s.listen(0,'127.0.0.1');await once(s,'listening');const p=s.address().port;await new Promise(r=>s.close(r));return p;}
const sdk=readFileSync(resolve(modern,'livekit-client/dist/livekit-client.umd.js'));
const assets=createServer((req,res)=>{if(req.url==='/sdk.js'){res.setHeader('Content-Type','application/javascript');res.end(sdk);}else res.end('<!doctype html><script src="/sdk.js"></script>');});
let browser;
async function exercise(kind,binary){
  const key='mutetest',secret=randomBytes(32).toString('hex'),p=await port(),base=`http://127.0.0.1:${p}`,roomName='mute-policy';
  const path=resolve(out,`${kind}.${kind==='go'?'yaml':'json'}`);
  const room=policy==='default'?{}:{room:{enable_remote_unmute:enabled}};
  writeFileSync(path,kind==='go'?`port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 62200\n  port_range_end: 62380\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n${policy==='default'?'':`room:\n  enable_remote_unmute: ${enabled}\n`}`:
    JSON.stringify({bind:'127.0.0.1',port:p,keys:{[key]:secret},rtc:{port_range_start:62400,port_range_end:62580},...room}));
  const record=report.servers[kind]={binary:resolve(binary),sha256:createHash('sha256').update(readFileSync(binary)).digest('hex'),cases:[]};
  const child=spawn(resolve(binary),['--config',path],{windowsHide:true});let log='',spawnError;
  child.stdout.on('data',b=>log+=b);child.stderr.on('data',b=>log+=b);child.on('error',e=>spawnError=e);
  const contexts=[];const api=new RoomServiceClient(base,key,secret);
  async function jwt(identity,grants){const t=new AccessToken(key,secret,{identity,ttl:'10m'});t.addGrant(grants);return t.toJwt();}
  const admin=await jwt('admin',{room:roomName,roomAdmin:true});
  const nonAdmin=await jwt('visitor',{room:roomName,roomJoin:true});
  async function request(payload,binary,token=admin){
    const r=await fetch(`${base}/twirp/livekit.RoomService/MutePublishedTrack`,{method:'POST',headers:{Authorization:`Bearer ${token}`,'Content-Type':binary?'application/protobuf':'application/json'},
      body:binary?new MuteRoomTrackRequest(payload).toBinary():JSON.stringify(payload),signal:AbortSignal.timeout(15000)});
    const body=r.ok&&binary?MuteRoomTrackResponse.fromBinary(new Uint8Array(await r.arrayBuffer())).toJson():await r.json();
    // Keep the full wire response as evidence; compare the method's relevant
    // track contract with nondeterministic SID mapped to the requested track.
    const semantic=r.ok?{track:body.track?{sid:body.track.sid===payload.trackSid?'requested':body.track.sid,muted:body.track.muted??false,type:body.track.type??'AUDIO',mimeType:body.track.mimeType??body.track.mime_type}:null}:body;
    return {status:r.status,body,semantic};
  }
  async function check(name,fn){try{const result=await fn();record.cases.push({name,result});console.log(`${kind}: ${name}`);}catch(e){record.cases.push({name,failure:e.stack});console.error(`${kind}: FAIL ${name}: ${e.message}`);}}
  async function client(identity){
    const context=await browser.newContext();contexts.push(context);const page=await context.newPage();
    page.on('pageerror',e=>log+=e.stack+'\n');await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async({base,token})=>{
      window.room=new LivekitClient.Room();window.remote=new Map();window.muteEvents=[];
      room.on(LivekitClient.RoomEvent.TrackSubscribed,(track,pub)=>{remote.set(pub.trackSid,track);document.body.appendChild(track.attach());});
      room.on(LivekitClient.RoomEvent.TrackMuted,pub=>muteEvents.push({sid:pub.trackSid,muted:true}));
      room.on(LivekitClient.RoomEvent.TrackUnmuted,pub=>muteEvents.push({sid:pub.trackSid,muted:false}));
      await room.connect(base.replace('http:','ws:'),token,{rtcConfig:{iceServers:[]}});
    },{base,token:await jwt(identity,{room:roomName,roomJoin:true})});return page;
  }
  try{
    await eventually(async()=>{if(spawnError)throw spawnError;if(child.exitCode!==null)throw Error('Server exited '+child.exitCode);return(await fetch(kind==='go'?base:`${base}/healthz`).catch(()=>null))?.ok;},'startup',60000);
    await api.createRoom({name:roomName});const alice=await client('alice'),bob=await client('bob');
    const sid=await alice.evaluate(async()=>{
      window.audio=new AudioContext();await audio.resume();window.osc=audio.createOscillator();window.gain=audio.createGain();gain.gain.value=0.2;
      window.destination=audio.createMediaStreamDestination();osc.connect(gain);gain.connect(destination);osc.start();
      window.pub=await room.localParticipant.publishTrack(destination.stream.getAudioTracks()[0],{dtx:false,red:false});return pub.trackSid;
    });
    async function decoded(){let sample;await eventually(async()=>{
      sample=await bob.evaluate(async sid=>{const s=await remote.get(sid)?.getRTCStatsReport();return [...(s?.values()||[])].find(v=>v.type==='inbound-rtp'&&v.kind==='audio');},sid);
      return sample?.totalAudioEnergy>0.00001&&sample.totalSamplesReceived>1000;
    },'actual audio decode');return true;}
    await check('initial Opus media decodes',decoded);
    const payload={room:roomName,identity:'alice',trackSid:sid};
    for(const binary of [false,true]){
      const mode=binary?'protobuf':'json';
      const call=(muted,extra={},auth=admin)=>request({...payload,muted,...extra},binary,auth);
      await check(`${mode}: remote mute returns muted track`,async()=>{
        const result=await call(true);assert.equal(result.status,200);
        await eventually(()=>bob.evaluate(sid=>room.remoteParticipants.get('alice')?.trackPublications.get(sid)?.isMuted===true,sid),'subscriber muted');
        await eventually(()=>alice.evaluate(()=>pub.isMuted),'publisher muted');return result;
      });
      await check(`${mode}: repeated mute is idempotent`,()=>call(true));
      await check(`${mode}: remote unmute follows server policy`,()=>call(false));
      await check(`${mode}: mute state matches policy without publisher override`,async()=>{
        await eventually(async()=>await alice.evaluate(()=>pub.isMuted)===!enabled && await bob.evaluate(sid=>room.remoteParticipants.get('alice')?.trackPublications.get(sid)?.isMuted,sid)===!enabled,'policy side effects');return {muted:!enabled};
      });
      await check(`${mode}: publisher can always unmute and resume media`,async()=>{
        await alice.evaluate(()=>pub.track.unmute());
        await eventually(()=>bob.evaluate(sid=>room.remoteParticipants.get('alice')?.trackPublications.get(sid)?.isMuted===false,sid),'local unmute');
        const before=await bob.evaluate(async sid=>{const s=await remote.get(sid).getRTCStatsReport();return [...s.values()].find(v=>v.type==='inbound-rtp'&&v.kind==='audio').totalAudioEnergy;},sid);
        await eventually(()=>bob.evaluate(async({sid,before})=>{const s=await remote.get(sid).getRTCStatsReport();return [...s.values()].some(v=>v.type==='inbound-rtp'&&v.kind==='audio'&&v.totalAudioEnergy>before+0.00001);},{sid,before}),'audio resumes');return {decoded:true};
      });
      await check(`${mode}: mute missing track returns no track`,()=>call(true,{trackSid:'TR_missing'}));
      await check(`${mode}: unmute missing track checks policy first`,()=>call(false,{trackSid:'TR_missing'}));
      await check(`${mode}: mute empty track returns no track`,()=>call(true,{trackSid:''}));
      await check(`${mode}: missing participant precedes mute policy`,()=>call(false,{identity:'missing'}));
      await check(`${mode}: non-admin cannot change track`,()=>call(true,{},nonAdmin));
    }
    record.completed=true;
  }catch(e){record.failure=e.stack;console.error(`${kind}: ${e.stack}`);}
  finally{for(const c of contexts)await c.close();if(child.exitCode===null&&child.signalCode===null){const done=once(child,'exit');child.kill();await done;}writeFileSync(resolve(out,`${kind}.log`),log);unlinkSync(path);}
}
try{
  assets.listen(0,'127.0.0.1');await once(assets,'listening');browser=await chromium.launch({channel:'chrome',headless:true,args:['--autoplay-policy=no-user-gesture-required']});report.browser=browser.version();
  await exercise('go',opts['go-server']);await exercise('cpp',opts['cpp-server']);
  for(const expected of report.servers.go.cases){const actual=report.servers.cpp.cases.find(c=>c.name===expected.name);let error;
    try{assert.ok(!expected.failure&&!actual?.failure&&actual);const semantic=v=>v.result?.semantic?{status:v.result.status,body:v.result.semantic}:v.result;assert.deepEqual(semantic(actual),semantic(expected));}catch(e){error=e.message;}
    report.comparisons.push({name:expected.name,passed:!error,...(error?{difference:error}:{})});
  }
  process.exitCode=report.servers.go.completed&&report.servers.cpp.completed&&report.comparisons.length===21&&report.comparisons.every(c=>c.passed)?0:1;
}catch(e){report.infrastructureError=e.stack;process.exitCode=2;}
finally{await browser?.close();await new Promise(r=>assets.close(r));report.exitCode=process.exitCode||0;writeFileSync(resolve(out,'report.json'),JSON.stringify(report,null,2));console.log(`${report.comparisons.filter(c=>c.passed).length}/21 matched; ${out}`);}
