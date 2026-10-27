// Real browser decoding with exact codec checks; fallback/transcoding cannot pass.
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
import { captureRtcDiagnostics } from './rtc_diagnostics.mjs';
const here=dirname(fileURLToPath(import.meta.url));
const opts=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{if(v.startsWith('--'))a.push([v.slice(2),all[i+1]]);return a;},[]));
assert.ok(opts['go-server']&&opts['cpp-server'],'Pass --go-server and --cpp-server');
assert.ok(!opts.only || ['go','cpp'].includes(opts.only),'--only must be go or cpp (diagnostics, not differential acceptance)');
const deps=resolve(opts['node-modules']||'D:/workspace/livekit-cpp/tests/node_modules');
const modernDeps=resolve(opts['modern-node-modules']||'D:/workspace/livekit-cli-cpp/web/node_modules');
const require=createRequire(resolve(deps,'../package.json'));
const {chromium}=require('playwright');const {AccessToken,RoomServiceClient}=require('livekit-server-sdk');
const out=resolve(opts.output||resolve(here,'results/go-codec-parity'),new Date().toISOString().replaceAll(/[:.]/g,'-')+'-'+randomBytes(3).toString('hex'));
mkdirSync(out,{recursive:true});
const report={fullReplacementVerified:false,differential:!opts.only,scope:'H264 mode 1 and VP8 actual decoding, mixed subscriptions, single/dual PC',sdkVersion:JSON.parse(readFileSync(resolve(modernDeps,'livekit-client/package.json'))).version,servers:{},comparisons:[]};
report.testSources={};
for(const name of ['go_codec_parity.mjs','rtc_diagnostics.mjs']){
  const bytes=readFileSync(resolve(here,name));writeFileSync(resolve(out,name),bytes);
  report.testSources[name]=createHash('sha256').update(bytes).digest('hex');
}
const sustainMs=Number(opts['sustain-ms']||0);assert.ok(Number.isInteger(sustainMs)&&sustainMs>=0&&sustainMs<=60000);
report.sustainedObservationMs=sustainMs;
const resubscribeRounds=Number(opts['resubscribe-rounds']||1);
assert.ok(Number.isInteger(resubscribeRounds)&&resubscribeRounds>=1&&resubscribeRounds<=20,'--resubscribe-rounds must be 1..20');
report.resubscribeRounds=resubscribeRounds;
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function eventually(fn,label,timeout=25000){const end=Date.now()+timeout;while(Date.now()<end){if(await fn())return;await delay(100);}throw Error('Timed out: '+label);}
async function port(){const s=tcpServer();s.listen(0,'127.0.0.1');await once(s,'listening');const p=s.address().port;await new Promise(r=>s.close(r));return p;}
const sdk=readFileSync(resolve(modernDeps,'livekit-client/dist/livekit-client.umd.js'));
const assets=createServer((req,res)=>{if(req.url==='/sdk.js'){res.setHeader('Content-Type','application/javascript');res.end(sdk);}else res.end('<!doctype html><script src="/sdk.js"></script>');});
let browser;
async function exercise(kind,binary){
  const key='codectest',secret=randomBytes(32).toString('hex'),p=await port(),base=`http://127.0.0.1:${p}`;
  const config=resolve(out,`${kind}.${kind==='go'?'yaml':'json'}`);
  writeFileSync(config,kind==='go'?`port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 61400\n  port_range_end: 61580\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`:
    JSON.stringify({bind:'127.0.0.1',port:p,keys:{[key]:secret},rtc:{port_range_start:61600,port_range_end:61780}}));
  const record=report.servers[kind]={binary:resolve(binary),sha256:createHash('sha256').update(readFileSync(binary)).digest('hex'),cases:[]};
  const child=spawn(resolve(binary),['--config',config],{windowsHide:true});let log='',spawnError;
  child.stdout.on('data',b=>log+=b);child.stderr.on('data',b=>log+=b);child.on('error',e=>spawnError=e);
  const contexts=[],pages=[];const api=new RoomServiceClient(base,key,secret);
  async function client(roomName,identity,single){
    const token=new AccessToken(key,secret,{identity,ttl:'15m'});token.addGrant({roomJoin:true,room:roomName});
    const context=await browser.newContext();contexts.push(context);const page=await context.newPage();pages.push(page);
    page.on('pageerror',e=>log+=`[${identity}] ${e.stack}\n`);
    page.on('console',m=>{if(['warning','error'].includes(m.type()))log+=`[${identity}] ${m.text()}\n`;});
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async({base,token,single})=>{
      window.room=new LivekitClient.Room({singlePeerConnection:single,adaptiveStream:false,dynacast:false});window.tracks=new Map();window.published=new Map();
      room.on(LivekitClient.RoomEvent.TrackSubscribed,(track,pub)=>{tracks.set(pub.trackSid,track);const el=track.attach();el.autoplay=true;document.body.appendChild(el);});
      room.on(LivekitClient.RoomEvent.TrackUnsubscribed,(track,pub)=>{tracks.delete(pub.trackSid);track.detach().forEach(el=>el.remove());});
      window.publish=async codec=>{
        const canvas=document.createElement('canvas');canvas.width=640;canvas.height=360;document.body.appendChild(canvas);
        const ctx=canvas.getContext('2d');let frame=0;
        setInterval(()=>{for(let x=0;x<640;x+=40){ctx.fillStyle=`hsl(${(frame+x)%360},75%,45%)`;ctx.fillRect(x,0,40,360);}ctx.fillStyle='white';ctx.font='32px sans-serif';ctx.fillText(`${codec} ${frame++}`,10,60);},66);
        const pub=await room.localParticipant.publishTrack(canvas.captureStream(15).getVideoTracks()[0],{source:LivekitClient.Track.Source.Camera,videoCodec:codec,backupCodec:false,simulcast:false,videoEncoding:{maxBitrate:700000,maxFramerate:15}});
        published.set(pub.trackSid,pub);return pub.trackSid;
      };
      window.stats=async(sid,outbound=false)=>{
        const track=outbound?published.get(sid)?.track:tracks.get(sid);const report=await track?.getRTCStatsReport();const result=[];
        report?.forEach(s=>{if(s.type===(outbound?'outbound-rtp':'inbound-rtp')&&s.kind==='video')result.push({id:s.id,ssrc:s.ssrc,mid:s.mid,frames:s.framesDecoded||s.framesEncoded||0,width:s.frameWidth,height:s.frameHeight,codec:report.get(s.codecId)?.mimeType,fmtp:report.get(s.codecId)?.sdpFmtpLine,
          packets:s.packetsReceived??s.packetsSent,bytes:s.bytesReceived??s.bytesSent,lost:s.packetsLost,jitter:s.jitter,keyFrames:s.keyFramesDecoded??s.keyFramesEncoded,nacks:s.nackCount,plis:s.pliCount,targetBitrate:s.targetBitrate,qualityLimitationReason:s.qualityLimitationReason,lastPacket:s.lastPacketReceivedTimestamp});});return result;
      };
      await room.connect(base.replace('http:','ws:'),token,{rtcConfig:{iceServers:[]}});
    },{base,token:await token.toJwt(),single});
    assert.equal(await page.evaluate(()=>room.engine.pcManager.mode==='publisher-only'),single);return page;
  }
  async function media(page,sid,codec){
    const before=await page.evaluate(sid=>stats(sid),sid);let evidence;
    const observation=record.lastDecodeWait={sid,codec,startedAt:Date.now(),samples:[]};let sampledAt=0;
    await eventually(async()=>{
      evidence=await page.evaluate(sid=>stats(sid),sid);
      if(Date.now()-sampledAt>=500){sampledAt=Date.now();observation.samples.push({elapsedMs:sampledAt-observation.startedAt,stats:evidence});}
      return evidence.some(s=>s.codec?.toLowerCase()===`video/${codec}`&&s.width===640&&s.height===360&&s.frames>Math.max(5,(before.find(b=>b.id===s.id)?.frames||0)+2));
    },`${codec} advances actual decoded frames`);
    return evidence;
  }
  async function sustained(page,sids){
    if(!sustainMs)return [];
    const samples=[];const start=Date.now();let previous,previousAt=start;
    while(Date.now()-start<sustainMs){
      const current=await page.evaluate(async sids=>Promise.all(sids.map(async sid=>({sid,stats:await stats(sid)}))),sids);
      samples.push({elapsedMs:Date.now()-start,tracks:current});
      record.lastMediaObservation=samples;
      if(previous && Date.now()-previousAt>=2000){
        for(const track of current){const old=previous.find(t=>t.sid===track.sid);
          assert.ok(track.stats.some(s=>s.frames>(old?.stats.find(o=>o.id===s.id)?.frames||0)),`Track ${track.sid} stopped decoding during sustained observation`);
        }
        previous=current;previousAt=Date.now();
      }else if(!previous)previous=current;
      await delay(500);
    }
    return samples;
  }
  async function check(name,fn){try{const evidence=await fn();record.cases.push({name,passed:true,evidence});console.log(`${kind}: PASS ${name}`);}catch(e){record.cases.push({name,passed:false,failure:e.stack});throw e;}}
  try{
    await eventually(async()=>{if(spawnError)throw spawnError;if(child.exitCode!==null)throw Error('Server exited '+child.exitCode);return(await fetch(kind==='go'?base:`${base}/healthz`).catch(()=>null))?.ok;},'startup',60000);
    for(const single of [false,true]){
      const mode=single?'single':'dual',roomName=`codecs-${mode}`;await api.createRoom({name:roomName});
      const alice=await client(roomName,'alice',single),bob=await client(roomName,'bob',single);let h264,vp8,reverse;
      await check(`${mode}: H264 decodes without VP8 fallback`,async()=>{
        h264=await alice.evaluate(()=>publish('h264'));const decoded=await media(bob,h264,'h264');
        const sent=await alice.evaluate(sid=>stats(sid,true),h264);assert.ok(sent.some(s=>s.codec?.toLowerCase()==='video/h264'&&/packetization-mode=1/.test(s.fmtp)));return {sent,decoded};
      });
      await check(`${mode}: publication metadata reports negotiated H264`,async()=>{
        const p=await api.getParticipant(roomName,'alice'),t=p.tracks.find(t=>t.sid===h264);assert.equal(t.mimeType.toLowerCase(),'video/h264');assert.ok(t.codecs.some(c=>c.mimeType.toLowerCase()==='video/h264'));return t;
      });
      await check(`${mode}: mixed VP8 and H264 select per-track downstream codec`,async()=>{
        vp8=await alice.evaluate(()=>publish('vp8'));return [await media(bob,vp8,'vp8'),await media(bob,h264,'h264'),await sustained(bob,[h264,vp8])];
      });
      await check(`${mode}: simultaneous reverse H264 publication`,async()=>{reverse=await bob.evaluate(()=>publish('h264'));return [await media(alice,reverse,'h264'),await media(bob,h264,'h264')];});
      await check(`${mode}: H264 mute and resume restores decoding`,async()=>{
        await alice.evaluate(sid=>published.get(sid).track.mute(),h264);await delay(600);
        await alice.evaluate(sid=>published.get(sid).track.unmute(),h264);return media(bob,h264,'h264');
      });
      await check(`${mode}: H264 resubscription retains encoding`,async()=>{
        const rounds=[];
        record.lastResubscription={mode,rounds};
        for(let round=1;round<=resubscribeRounds;++round){
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setSubscribed(false),h264);
          await eventually(()=>bob.evaluate(sid=>!tracks.has(sid),h264),'unsubscribed');
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setSubscribed(true),h264);
          rounds.push({round,h264:await media(bob,h264,'h264'),vp8:await media(bob,vp8,'vp8'),reverse:await media(alice,reverse,'h264')});
        }
        return rounds;
      });
      await check(`${mode}: late subscriber decodes both codecs`,async()=>{const late=await client(roomName,'late',single);return [await media(late,h264,'h264'),await media(late,vp8,'vp8'),await media(late,reverse,'h264'),await sustained(late,[h264,vp8,reverse])];});
      (record.transportCheckpoints??=[]).push({mode,clients:await Promise.all(pages.map(page=>page.evaluate(captureRtcDiagnostics)))});
      for(const context of contexts.splice(0))await context.close();pages.length=0;await api.deleteRoom(roomName);
    }
    if(kind==='cpp'){
      const token=new AccessToken(key,secret);token.addGrant({roomList:true});
      const body=await(await fetch(`${base}/metrics`,{headers:{Authorization:`Bearer ${await token.toJwt()}`}})).text();
      const metrics=Object.fromEntries(body.trim().split('\n').map(line=>{const [name,value]=line.split(' ');return[name,Number(value)];}));
      assert.equal(metrics.livekit_cpp_video_encode_calls,0);assert.equal(metrics.livekit_cpp_video_decode_calls,0);assert.ok(metrics.livekit_cpp_sfu_video_frames_forwarded>100);record.audit=metrics;
    }
    record.completed=true;
  }catch(e){record.failure=e.stack;console.error(`${kind}: ${e.stack}`);
    if(kind==='cpp')try{
      const token=new AccessToken(key,secret);token.addGrant({roomList:true});
      record.failureMetrics=await(await fetch(`${base}/metrics`,{headers:{Authorization:`Bearer ${await token.toJwt()}`}})).text();
    }catch(error){record.failureMetricsError=String(error);}
    record.diagnostics=await Promise.all(pages.map(async page=>{try{
      const transport=await page.evaluate(captureRtcDiagnostics);
      return {...await page.evaluate(async()=>({mode:room.engine.pcManager?.mode,localSdp:room.engine.pcManager?.publisher.pc.localDescription?.sdp,remoteSdp:room.engine.pcManager?.subscriber?.pc.remoteDescription?.sdp,published:await Promise.all([...published.keys()].map(async sid=>({sid,stats:await stats(sid,true)}))),received:await Promise.all([...tracks.keys()].map(async sid=>({sid,stats:await stats(sid)})))})),transport};
    }catch(e){return{error:String(e)};}}));
  }finally{for(const c of contexts)await c.close();if(child.exitCode===null&&child.signalCode===null){const exited=once(child,'exit');child.kill();await exited;}writeFileSync(resolve(out,`${kind}.log`),log);unlinkSync(config);}
}
try{assets.listen(0,'127.0.0.1');await once(assets,'listening');browser=await chromium.launch({channel:'chrome',headless:true,args:['--autoplay-policy=no-user-gesture-required','--disable-background-timer-throttling']});report.browser=browser.version();
  if(!opts.only || opts.only==='go')await exercise('go',opts['go-server']);
  if(!opts.only || opts.only==='cpp')await exercise('cpp',opts['cpp-server']);
  if(opts.only)process.exitCode=report.servers[opts.only].completed?0:1;
  else {
    for(const expected of report.servers.go.cases){const actual=report.servers.cpp.cases.find(c=>c.name===expected.name);report.comparisons.push({name:expected.name,passed:expected.passed&&actual?.passed===true});}
    process.exitCode=report.servers.go.completed&&report.servers.cpp.completed&&report.comparisons.length===14&&report.comparisons.every(c=>c.passed)?0:1;
  }
}catch(e){report.infrastructureError=e.stack;process.exitCode=2;}
finally{await browser?.close();await new Promise(r=>assets.close(r));report.exitCode=process.exitCode||0;writeFileSync(resolve(out,'report.json'),JSON.stringify(report,null,2));console.log(`${report.comparisons.filter(c=>c.passed).length}/${report.comparisons.length} matched; ${out}`);}
