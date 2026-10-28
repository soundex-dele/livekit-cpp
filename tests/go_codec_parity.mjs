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
const testCodec=opts.codec||'h264';assert.ok(['h264','vp8','vp9','av1','h265'].includes(testCodec));
const scalable=['vp9','av1'].includes(testCodec);
const encrypted=opts.e2ee==='true';
const adaptive=opts.adaptive==='true',backup=opts.backup==='true';
const backupPolicy=Number(opts['backup-policy']??1);assert.ok([0,1,2].includes(backupPolicy));
const h264Mode=opts['h264-mode']??'1',h264Profile=opts['h264-profile'];
assert.ok(['0','1'].includes(h264Mode));
assert.ok(!backup || scalable,'--backup requires VP9/AV1 primary');
const scalabilityMode=opts['scalability-mode']||(scalable?'L1T1':undefined);
const spatialLayers=Number(scalabilityMode?.match(/^[LS](\d)/)?.[1]||1);
const svcOnly=opts['svc-only']==='true';assert.ok(!svcOnly || (scalable && spatialLayers===3),'--svc-only requires three spatial layers');
const videoWidth=spatialLayers>=3?1280:640,videoHeight=spatialLayers>=3?720:360;
assert.ok(!opts.only || ['go','cpp'].includes(opts.only),'--only must be go or cpp (diagnostics, not differential acceptance)');
const deps=resolve(opts['node-modules']||'D:/workspace/livekit-cpp/tests/node_modules');
const modernDeps=resolve(opts['modern-node-modules']||'D:/workspace/livekit-cli-cpp/web/node_modules');
const require=createRequire(resolve(deps,'../package.json'));
const {chromium}=require('playwright');const {AccessToken,RoomServiceClient}=require('livekit-server-sdk');
const out=resolve(opts.output||resolve(here,'results/go-codec-parity'),new Date().toISOString().replaceAll(/[:.]/g,'-')+'-'+randomBytes(3).toString('hex'));
mkdirSync(out,{recursive:true});
const report={fullReplacementVerified:false,differential:!opts.only,scope:`${testCodec} and VP8 actual decoding, mixed subscriptions, single/dual PC`,codec:testCodec,scalabilityMode,svcOnly,sdkVersion:JSON.parse(readFileSync(resolve(modernDeps,'livekit-client/package.json'))).version,servers:{},comparisons:[]};
report.encrypted=encrypted;
Object.assign(report,{adaptive,backup,backupPolicy});
Object.assign(report,{h264Mode,h264Profile});
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
const worker=readFileSync(resolve(modernDeps,'livekit-client/dist/livekit-client.e2ee.worker.js'));
const assets=createServer((req,res)=>{if(req.url==='/sdk.js'||req.url==='/worker.js'){res.setHeader('Content-Type','application/javascript');res.end(req.url==='/sdk.js'?sdk:worker);}else res.end('<!doctype html><script src="/sdk.js"></script>');});
let browser;
async function exercise(kind,binary){
  const key='codectest',secret=randomBytes(32).toString('hex'),p=await port(),base=`http://127.0.0.1:${p}`;
  const config=resolve(out,`${kind}.${kind==='go'?'yaml':'json'}`);
  writeFileSync(config,kind==='go'?`port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 61400\n  port_range_end: 61580\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\n  ips:\n    includes: [127.0.0.1/32]\nkeys:\n  ${key}: ${secret}\n`:
    JSON.stringify({bind:'127.0.0.1',port:p,keys:{[key]:secret},rtc:{port_range_start:61600,port_range_end:61780}}));
  const record=report.servers[kind]={binary:resolve(binary),sha256:createHash('sha256').update(readFileSync(binary)).digest('hex'),cases:[]};
  const child=spawn(resolve(binary),['--config',config],{windowsHide:true});let log='',spawnError;
  child.stdout.on('data',b=>log+=b);child.stderr.on('data',b=>log+=b);child.on('error',e=>spawnError=e);
  const contexts=[],pages=[];const api=new RoomServiceClient(base,key,secret);
  async function client(roomName,identity,single,encryptionKey='media-compatibility-test-key',receiveCodec){
    const token=new AccessToken(key,secret,{identity,ttl:'15m'});token.addGrant({roomJoin:true,room:roomName});
    const context=await browser.newContext();contexts.push(context);const page=await context.newPage();pages.push(page);
    page.on('pageerror',e=>log+=`[${identity}] ${e.stack}\n`);
    page.on('console',m=>{if(['warning','error'].includes(m.type()))log+=`[${identity}] ${m.text()}\n`;});
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async({base,token,single,scalabilityMode,videoWidth,videoHeight,encrypted,encryptionKey,adaptive,backup,backupPolicy,receiveCodec,h264Mode,h264Profile})=>{
      if(receiveCodec)for(const method of ['createOffer','createAnswer']){
        const original=RTCPeerConnection.prototype[method];
        RTCPeerConnection.prototype[method]=function(...args){
          for(const t of this.getTransceivers())if(t.receiver.track.kind==='video' && !t.sender.track){
            const caps=RTCRtpReceiver.getCapabilities('video').codecs.filter(c=>c.mimeType.toLowerCase()===`video/${receiveCodec}` || c.mimeType.toLowerCase()==='video/rtx');
            t.setCodecPreferences(caps);
          }
          return original.apply(this,args);
        };
      }
      let e2ee;
      if(encrypted){window.keyProvider=new LivekitClient.ExternalE2EEKeyProvider();await keyProvider.setKey(encryptionKey);e2ee={keyProvider,worker:new Worker('/worker.js')};}
      window.room=new LivekitClient.Room({singlePeerConnection:single,adaptiveStream:false,dynacast:adaptive||backup,...(e2ee?{e2ee}:{})});window.tracks=new Map();window.published=new Map();
      window.qualityUpdates=[];window.streamUpdates=[];
      room.on(LivekitClient.RoomEvent.TrackStreamStateChanged,(pub,state,participant)=>streamUpdates.push({sid:pub.trackSid,state,identity:participant.identity}));
      if(encrypted)await room.setE2EEEnabled(true);
      room.on(LivekitClient.RoomEvent.TrackSubscribed,(track,pub)=>{tracks.set(pub.trackSid,track);const el=track.attach();el.autoplay=true;document.body.appendChild(el);});
      room.on(LivekitClient.RoomEvent.TrackUnsubscribed,(track,pub)=>{tracks.delete(pub.trackSid);track.detach().forEach(el=>el.remove());});
      window.publish=async codec=>{
        if(codec==='h264')room.localParticipant.once(LivekitClient.ParticipantEvent.LocalSenderCreated,sender=>{
          const t=room.engine.pcManager.publisher.pc.getTransceivers().find(t=>t.sender===sender);
          const caps=RTCRtpSender.getCapabilities('video').codecs.filter(c=>c.mimeType.toLowerCase()==='video/h264' && c.sdpFmtpLine?.includes(`packetization-mode=${h264Mode}`) && (!h264Profile || c.sdpFmtpLine?.includes(`profile-level-id=${h264Profile}`)));
          if(!t || !caps.length)throw Error('Requested H264 mode/profile not available in browser');
          t.setCodecPreferences([...caps,...RTCRtpSender.getCapabilities('video').codecs.filter(c=>c.mimeType.toLowerCase()==='video/rtx')]);
        });
        const canvas=document.createElement('canvas');canvas.width=videoWidth;canvas.height=videoHeight;document.body.appendChild(canvas);
        const ctx=canvas.getContext('2d');let frame=0;
        setInterval(()=>{for(let x=0;x<canvas.width;x+=40){ctx.fillStyle=`hsl(${(frame+x)%360},75%,45%)`;ctx.fillRect(x,0,40,canvas.height);}ctx.fillStyle='white';ctx.font='32px sans-serif';ctx.fillText(`${codec} ${frame++}`,10,60);},66);
        const pub=await room.localParticipant.publishTrack(canvas.captureStream(15).getVideoTracks()[0],{source:LivekitClient.Track.Source.Camera,videoCodec:codec,backupCodec:backup && ['vp9','av1'].includes(codec)?{codec:'vp8'}:false,backupCodecPolicy:backupPolicy,simulcast:false,degradationPreference:'maintain-resolution',...(['vp9','av1'].includes(codec)?{scalabilityMode}:{}),videoEncoding:{maxBitrate:videoWidth>=1280?1400000:700000,maxFramerate:15}});
        published.set(pub.trackSid,pub);return pub.trackSid;
      };
      window.stats=async(sid,outbound=false)=>{
        const track=outbound?published.get(sid)?.track:tracks.get(sid);const report=await track?.getRTCStatsReport();const result=[];
        report?.forEach(s=>{if(s.type===(outbound?'outbound-rtp':'inbound-rtp')&&s.kind==='video')result.push({id:s.id,ssrc:s.ssrc,mid:s.mid,frames:s.framesDecoded||s.framesEncoded||0,scalabilityMode:s.scalabilityMode,width:s.frameWidth,height:s.frameHeight,codec:report.get(s.codecId)?.mimeType,fmtp:report.get(s.codecId)?.sdpFmtpLine,
          packets:s.packetsReceived??s.packetsSent,bytes:s.bytesReceived??s.bytesSent,lost:s.packetsLost,jitter:s.jitter,keyFrames:s.keyFramesDecoded??s.keyFramesEncoded,nacks:s.nackCount,plis:s.pliCount,targetBitrate:s.targetBitrate,qualityLimitationReason:s.qualityLimitationReason,lastPacket:s.lastPacketReceivedTimestamp});});return result;
      };
      window.connectionTimeline=[];
      const observed=new Map();
      const monitor=setInterval(async()=>{
        const manager=room.engine.pcManager;
        for(const role of ['publisher','subscriber']){
          const pc=manager?.[role]?.pc;if(pc)observed.set(pc,role);
        }
        const connections=[];
        for(const [pc,role] of observed){
          const item={role,state:pc.connectionState,ice:pc.iceConnectionState,gathering:pc.iceGatheringState};
          try{item.stats=[...(await pc.getStats()).values()].filter(s=>['candidate-pair','local-candidate','remote-candidate','transport'].includes(s.type));}catch{}
          connections.push(item);
        }
        if(connectionTimeline.length<160)connectionTimeline.push({at:Date.now(),connections});
      },250);
      try{
        await room.connect(base.replace('http:','ws:'),token,{rtcConfig:{iceServers:[]}});
        const original=room.engine.client.onSubscribedQualityUpdate;
        room.engine.client.onSubscribedQualityUpdate=update=>{qualityUpdates.push(JSON.parse(JSON.stringify(update)));original?.(update);};
      }
      finally{clearInterval(monitor);}
    },{base,token:await token.toJwt(),single,scalabilityMode,videoWidth,videoHeight,encrypted,encryptionKey,adaptive,backup,backupPolicy,receiveCodec,h264Mode,h264Profile});
    assert.equal(await page.evaluate(()=>room.engine.pcManager.mode==='publisher-only'),single);return page;
  }
  async function media(page,sid,codec,width=videoWidth,height=videoHeight){
    const before=await page.evaluate(sid=>stats(sid),sid);let evidence;
    const observation=record.lastDecodeWait={sid,codec,startedAt:Date.now(),samples:[]};let sampledAt=0;
    await eventually(async()=>{
      evidence=await page.evaluate(sid=>stats(sid),sid);
      if(Date.now()-sampledAt>=500){sampledAt=Date.now();observation.samples.push({elapsedMs:sampledAt-observation.startedAt,stats:evidence});}
      return evidence.some(s=>s.codec?.toLowerCase()===`video/${codec}`&&s.width===width&&s.height===height&&s.frames>Math.max(5,(before.find(b=>b.id===s.id)?.frames||0)+2));
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
      const alice=await client(roomName,'alice',single),bob=await client(roomName,'bob',single);let h264,vp8,reverse,late;
      await check(`${mode}: ${testCodec.toUpperCase()} decodes without VP8 fallback`,async()=>{
        h264=await alice.evaluate(codec=>publish(codec),testCodec);const decoded=await media(bob,h264,testCodec);
        // SDP may negotiate a lower H264 level while retaining the requested
        // profile/constraints. Compare those bytes, and record the full fmtp.
        const sent=await alice.evaluate(sid=>stats(sid,true),h264);assert.ok(sent.some(s=>s.codec?.toLowerCase()===`video/${testCodec}`&&(testCodec!=='h264'||(s.fmtp?.includes(`packetization-mode=${h264Mode}`)&&(!h264Profile||s.fmtp?.includes(`profile-level-id=${h264Profile.slice(0,4)}`))))));if(scalable)assert.ok(sent.some(s=>s.scalabilityMode===scalabilityMode),'Publisher must use requested SVC mode');return {sent,decoded};
      });
      await check(`${mode}: publication metadata reports negotiated ${testCodec.toUpperCase()}`,async()=>{
        const p=await api.getParticipant(roomName,'alice'),t=p.tracks.find(t=>t.sid===h264);assert.equal(t.mimeType.toLowerCase(),`video/${testCodec}`);assert.ok(t.codecs.some(c=>c.mimeType.toLowerCase()===`video/${testCodec}`));if(scalable){assert.equal(t.simulcast,false,'SVC spatial layers must not be announced as simulcast');if(spatialLayers===3){for(const layers of [t.layers,t.codecs[0].layers])assert.deepEqual(layers.map(l=>({width:l.width,height:l.height,spatialLayer:l.spatialLayer})),[{width:320,height:180,spatialLayer:0},{width:640,height:360,spatialLayer:1},{width:1280,height:720,spatialLayer:2}]);}}return t;
      });
      if(!svcOnly){
      await check(`${mode}: mixed VP8 and ${testCodec.toUpperCase()} select per-track downstream codec`,async()=>{
        vp8=await alice.evaluate(()=>publish('vp8'));return [await media(bob,vp8,'vp8'),await media(bob,h264,testCodec),await sustained(bob,[h264,vp8])];
      });
      await check(`${mode}: simultaneous reverse ${testCodec.toUpperCase()} publication`,async()=>{reverse=await bob.evaluate(codec=>publish(codec),testCodec);return [await media(alice,reverse,testCodec),await media(bob,h264,testCodec)];});
      await check(`${mode}: ${testCodec.toUpperCase()} mute and resume restores decoding`,async()=>{
        await alice.evaluate(sid=>published.get(sid).track.mute(),h264);await delay(600);
        await alice.evaluate(sid=>published.get(sid).track.unmute(),h264);return media(bob,h264,testCodec);
      });
      await check(`${mode}: ${testCodec.toUpperCase()} resubscription retains encoding`,async()=>{
        const rounds=[];
        record.lastResubscription={mode,rounds};
        for(let round=1;round<=resubscribeRounds;++round){
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setSubscribed(false),h264);
          await eventually(()=>bob.evaluate(sid=>!tracks.has(sid),h264),'unsubscribed');
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setSubscribed(true),h264);
          rounds.push({round,h264:await media(bob,h264,testCodec),vp8:await media(bob,vp8,'vp8'),reverse:await media(alice,reverse,testCodec)});
        }
        return rounds;
      });
      await check(`${mode}: late subscriber decodes both codecs`,async()=>{late=await client(roomName,'late',single);return [await media(late,h264,testCodec),await media(late,vp8,'vp8'),await media(late,reverse,testCodec),await sustained(late,[h264,vp8,reverse])];});
      }else await check(`${mode}: second independent subscriber decodes VP9`,async()=>{late=await client(roomName,'late',single);return media(late,h264,testCodec);});
      if(scalable && spatialLayers===3){
        await check(`${mode}: independent VP9 LOW and HIGH spatial subscriptions`,async()=>{
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setVideoQuality(LivekitClient.VideoQuality.LOW),h264);
          return {low:await media(bob,h264,testCodec,320,180),high:await media(late,h264,testCodec)};
        });
        await check(`${mode}: VP9 dimensions select middle spatial layer`,async()=>{
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setVideoDimensions({width:640,height:360}),h264);
          return {middle:await media(bob,h264,testCodec,640,360),high:await media(late,h264,testCodec)};
        });
        await check(`${mode}: VP9 spatial upgrade restores full resolution`,async()=>{
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setVideoQuality(LivekitClient.VideoQuality.HIGH),h264);
          return [await media(bob,h264,testCodec),await media(late,h264,testCodec),...(svcOnly?[]:[await media(alice,reverse,testCodec)])];
        });
      }
      if(adaptive){
        await check(`${mode}: temporal FPS selection preserves decoding`,async()=>{
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setVideoFPS(4),h264);
          await delay(2500);
          const before=await bob.evaluate(sid=>stats(sid),h264);await delay(4000);
          const after=await bob.evaluate(sid=>stats(sid),h264);
          const frames=after.reduce((n,s)=>n+s.frames-(before.find(b=>b.id===s.id)?.frames||0),0);
          assert.ok(frames>3,`Temporal request stopped decoding (${frames} frames in 4 seconds)`);
          await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setVideoFPS(30),h264);
          return {before,after,framesInFourSeconds:frames,fpsEffective:frames<32,resumed:await media(bob,h264,testCodec)};
        });
        await check(`${mode}: dynacast pauses unused publication and resumes`,async()=>{
          for(const page of [bob,late])await page.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setEnabled(false),h264);
          await eventually(()=>alice.evaluate(sid=>qualityUpdates.some(u=>u.trackSid===sid && u.subscribedQualities.length && u.subscribedQualities.every(q=>!q.enabled)),h264),'dynacast all layers disabled');
          const disabled=await alice.evaluate(()=>qualityUpdates);
          for(const page of [bob,late])await page.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setEnabled(true),h264);
          return {disabled,resumed:await media(bob,h264,testCodec),streamUpdates:await bob.evaluate(()=>streamUpdates)};
        });
      }
      if(backup)await check(`${mode}: VP8-only subscriber receives backup codec under original SID`,async()=>{
        const fallback=await client(roomName,'fallback',single,'media-compatibility-test-key','vp8');
        const decoded=await media(fallback,h264,'vp8');
        const primary=await media(bob,h264,backupPolicy===1?testCodec:'vp8');
        const participant=await api.getParticipant(roomName,'alice');
        const track=participant.tracks.find(t=>t.sid===h264);
        assert.ok(track.codecs.some(c=>c.mimeType.toLowerCase()==='video/vp8' && c.mid),'Backup codec metadata absent');
        return {decoded,primary,track};
      });
      if(encrypted)await check(`${mode}: wrong E2EE key cannot decode, corrected key recovers`,async()=>{
        const wrong=await client(roomName,'wrong-key',single,'incorrect-test-key');
        await eventually(()=>wrong.evaluate(sid=>tracks.has(sid),h264),'encrypted subscription');
        // Subscription events precede bandwidth allocation and the first media
        // packet. Padding-only traffic cannot establish encryption rejection.
        await eventually(async()=>{
          const stats=await wrong.evaluate(sid=>window.stats(sid),h264);
          record.wrongKeyBeforeObservation=stats;
          // Chrome does not expose codecId until a frame passes the encoded
          // transform. The publication SID identifies this negative-test track.
          return stats.some(s=>s.bytes>0);
        },'wrong-key receiver gets encrypted payload');
        await delay(2500);
        const rejected=await wrong.evaluate(sid=>stats(sid),h264);
        record.wrongKeyRejected=rejected;
        assert.ok(rejected.length>0,'Wrong-key receiver lost inbound media statistics');
        assert.ok(rejected.every(s=>s.frames===0),'Wrong key decoded encrypted video');
        await wrong.evaluate(()=>keyProvider.setKey('media-compatibility-test-key'));
        const recovered=await media(wrong,h264,testCodec);
        const info=await api.getParticipant(roomName,'alice');
        assert.notEqual(info.tracks.find(t=>t.sid===h264).encryption,0,'Publication encryption flag lost');
        return {rejected,recovered};
      });
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
      return {...await page.evaluate(async()=>({connectionTimeline:window.connectionTimeline,mode:room.engine.pcManager?.mode,localSdp:room.engine.pcManager?.publisher.pc.localDescription?.sdp,remoteSdp:room.engine.pcManager?.subscriber?.pc.remoteDescription?.sdp,published:await Promise.all([...published.keys()].map(async sid=>({sid,stats:await stats(sid,true)}))),received:await Promise.all([...tracks.keys()].map(async sid=>({sid,stats:await stats(sid)})))})),transport};
    }catch(e){return{error:String(e)};}}));
  }finally{for(const c of contexts)await c.close();if(child.exitCode===null&&child.signalCode===null){const exited=once(child,'exit');child.kill();await exited;}writeFileSync(resolve(out,`${kind}.log`),log);unlinkSync(config);}
}
try{assets.listen(0,'127.0.0.1');await once(assets,'listening');browser=await chromium.launch({channel:'chrome',headless:true,args:['--autoplay-policy=no-user-gesture-required','--disable-background-timer-throttling']});report.browser=browser.version();
  if(!opts.only || opts.only==='go')await exercise('go',opts['go-server']);
  if(!opts.only || opts.only==='cpp')await exercise('cpp',opts['cpp-server']);
  if(opts.only)process.exitCode=report.servers[opts.only].completed?0:1;
  else {
    for(const expected of report.servers.go.cases){const actual=report.servers.cpp.cases.find(c=>c.name===expected.name);report.comparisons.push({name:expected.name,passed:expected.passed&&actual?.passed===true&&expected.evidence?.fpsEffective===actual?.evidence?.fpsEffective,...(expected.evidence?.fpsEffective!==undefined?{goFpsEffective:expected.evidence.fpsEffective,cppFpsEffective:actual?.evidence?.fpsEffective}:{})});}
    process.exitCode=report.servers.go.completed&&report.servers.cpp.completed&&report.comparisons.length===((svcOnly?12:scalable && spatialLayers===3?20:14)+(encrypted?2:0)+(adaptive?4:0)+(backup?2:0))&&report.comparisons.every(c=>c.passed)?0:1;
  }
}catch(e){report.infrastructureError=e.stack;process.exitCode=2;}
finally{await browser?.close();await new Promise(r=>assets.close(r));report.exitCode=process.exitCode||0;writeFileSync(resolve(out,'report.json'),JSON.stringify(report,null,2));console.log(`${report.comparisons.filter(c=>c.passed).length}/${report.comparisons.length} matched; ${out}`);}
