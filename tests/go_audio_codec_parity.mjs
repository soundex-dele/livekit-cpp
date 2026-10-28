// Official SDK + browser codec preferences; verify actual G.711/Opus playout.
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
assert.ok(!opts.only || ['go','cpp'].includes(opts.only),'--only is diagnostic, not differential acceptance');
const red=opts.red==='true',encrypted=opts.e2ee==='true',events=opts.events==='true';
const deps=resolve(opts['node-modules']||'D:/workspace/livekit-cpp/tests/node_modules');
const modern=resolve(opts['modern-node-modules']||'D:/workspace/livekit-cli-cpp/web/node_modules');
const require=createRequire(resolve(deps,'../package.json'));
const {chromium}=require('playwright');const {AccessToken,RoomServiceClient}=require('livekit-server-sdk');
const out=resolve(opts.output||resolve(here,'results/go-audio-codec-parity'),new Date().toISOString().replaceAll(/[:.]/g,'-')+'-'+randomBytes(3).toString('hex'));
mkdirSync(out,{recursive:true});
const report={fullReplacementVerified:false,differential:!opts.only,scope:'PCMU/PCMA 8 kHz + Opus mixed audio, single/dual PC, real decoded tone checks',sdkVersion:JSON.parse(readFileSync(resolve(modern,'livekit-client/package.json'))).version,servers:{},comparisons:[]};
Object.assign(report,{red,encrypted,events});
report.testSources={};
const testSource=readFileSync(fileURLToPath(import.meta.url));
writeFileSync(resolve(out,'go_audio_codec_parity.mjs'),testSource);
report.testSources['go_audio_codec_parity.mjs']=createHash('sha256').update(testSource).digest('hex');
const expectedCases=18+(events?4:0);
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function eventually(fn,label,timeout=25000){const end=Date.now()+timeout;while(Date.now()<end){if(await fn())return;await delay(100);}throw Error('Timed out: '+label);}
async function port(){const s=tcpServer();s.listen(0,'127.0.0.1');await once(s,'listening');const p=s.address().port;await new Promise(r=>s.close(r));return p;}
const sdk=readFileSync(resolve(modern,'livekit-client/dist/livekit-client.umd.js'));
const worker=readFileSync(resolve(modern,'livekit-client/dist/livekit-client.e2ee.worker.js'));
const assets=createServer((req,res)=>{if(req.url==='/sdk.js'||req.url==='/worker.js'){res.setHeader('Content-Type','application/javascript');res.end(req.url==='/sdk.js'?sdk:worker);}else res.end('<!doctype html><script src="/sdk.js"></script>');});
let browser;
async function exercise(kind,binary){
  const key='audiotest',secret=randomBytes(32).toString('hex'),p=await port(),base=`http://127.0.0.1:${p}`;
  const config=resolve(out,`${kind}.${kind==='go'?'yaml':'json'}`);
  writeFileSync(config,kind==='go'?`port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 61800\n  port_range_end: 61980\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\n  ips:\n    includes: [127.0.0.1/32]\nkeys:\n  ${key}: ${secret}\n`:
    JSON.stringify({bind:'127.0.0.1',port:p,keys:{[key]:secret},rtc:{port_range_start:62000,port_range_end:62180}}));
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
    await page.evaluate(async({base,token,single,red,encrypted})=>{
      let e2ee;if(encrypted){const keyProvider=new LivekitClient.ExternalE2EEKeyProvider();await keyProvider.setKey('audio-compatibility-test-key');e2ee={keyProvider,worker:new Worker('/worker.js')};}
      window.room=new LivekitClient.Room({singlePeerConnection:single,adaptiveStream:false,dynacast:false,...(e2ee?{e2ee}:{})});
      if(encrypted)await room.setE2EEEnabled(true);
      window.speakerEvents=[];window.qualityEvents=[];
      room.on(LivekitClient.RoomEvent.ActiveSpeakersChanged,participants=>speakerEvents.push(participants.map(p=>({identity:p.identity,level:p.audioLevel}))));
      room.on(LivekitClient.RoomEvent.ConnectionQualityChanged,(quality,participant)=>qualityEvents.push({quality,identity:participant.identity}));
      window.tracks=new Map();window.published=new Map();window.audioSources=[];window.monitors=new Map();window.codecSelections=[];
      window.audio=new AudioContext({sampleRate:48000});await audio.resume();
      window.descriptionHistory=[];
      const setRemote=RTCPeerConnection.prototype.setRemoteDescription;
      RTCPeerConnection.prototype.setRemoteDescription=function(description){descriptionHistory.push(description);return setRemote.call(this,description);};
      room.on(LivekitClient.RoomEvent.TrackSubscribed,(track,pub)=>{
        tracks.set(pub.trackSid,track);const el=track.attach();el.autoplay=true;document.body.appendChild(el);
        const source=audio.createMediaStreamSource(new MediaStream([track.mediaStreamTrack]));
        const analyser=audio.createAnalyser();analyser.fftSize=2048;analyser.smoothingTimeConstant=0;
        const silent=audio.createGain();silent.gain.value=0;source.connect(analyser);analyser.connect(silent);silent.connect(audio.destination);
        monitors.set(pub.trackSid,{source,analyser,silent});
      });
      room.on(LivekitClient.RoomEvent.TrackUnsubscribed,(track,pub)=>{
        tracks.delete(pub.trackSid);track.detach().forEach(el=>el.remove());
        const m=monitors.get(pub.trackSid);if(m){m.source.disconnect();m.analyser.disconnect();m.silent.disconnect();monitors.delete(pub.trackSid);}
      });
      window.publish=async(codec,frequency)=>{
        const oscillator=audio.createOscillator(),gain=audio.createGain(),destination=audio.createMediaStreamDestination();
        destination.channelCount=1;
        oscillator.frequency.value=frequency;gain.gain.value=0.2;oscillator.connect(gain);gain.connect(destination);oscillator.start();
        audioSources.push({oscillator,gain,destination});
        // LiveKit's Web API has no audioCodec option. Select a standard browser
        // capability before SDK negotiation, identically against both servers.
        let chosen=false;
        const choose=(sender)=>{
          const t=room.engine.pcManager.publisher.pc.getTransceivers().find(t=>t.sender===sender);
          const caps=RTCRtpSender.getCapabilities('audio').codecs.filter(c=>c.mimeType.toLowerCase()===`audio/${codec}` || (red && codec==='opus' && c.mimeType.toLowerCase()==='audio/red'));
          if(red && codec==='opus'){caps.sort((a,b)=>Number(b.mimeType.toLowerCase()==='audio/red')-Number(a.mimeType.toLowerCase()==='audio/red'));if(!caps.some(c=>c.mimeType.toLowerCase()==='audio/red'))throw Error('Browser lacks RED capability');}
          if(!t || !caps.length)throw Error('Browser does not expose requested audio codec '+codec);
          t.setCodecPreferences(caps);
          chosen=true;codecSelections.push({codec,caps,trackId:sender.track?.id});
        };
        room.localParticipant.once(LivekitClient.ParticipantEvent.LocalSenderCreated,choose);
        try{
          // G.711 is fixed at 64 kbps. The SDK speech preset is lower and
          // Chrome rejects the answer with that sender maxBitrate cap.
          const pub=await room.localParticipant.publishTrack(destination.stream.getAudioTracks()[0],{name:codec,source:LivekitClient.Track.Source.Microphone,red:red && codec==='opus',dtx:false,audioPreset:{maxBitrate:64000}});
          if(!chosen)throw Error('Codec selection event did not fire');
          published.set(pub.trackSid,pub);return pub.trackSid;
        }finally{room.localParticipant.off(LivekitClient.ParticipantEvent.LocalSenderCreated,choose);}
      };
      window.stats=async(sid,outbound=false)=>{
        const track=outbound?published.get(sid)?.track:tracks.get(sid),stats=await track?.getRTCStatsReport(),result=[];
        stats?.forEach(s=>{if(s.type===(outbound?'outbound-rtp':'inbound-rtp')&&s.kind==='audio'){
          const c=stats.get(s.codecId);result.push({id:s.id,packets:s.packetsReceived??s.packetsSent,samples:s.totalSamplesReceived,energy:s.totalAudioEnergy,concealed:s.concealedSamples,codec:c?.mimeType,clockRate:c?.clockRate,channels:c?.channels});
        }});return result;
      };
      window.tone=sid=>{
        const m=monitors.get(sid);if(!m)return null;
        const samples=new Float32Array(m.analyser.fftSize);m.analyser.getFloatTimeDomainData(samples);
        const spectrum=new Float32Array(m.analyser.frequencyBinCount);m.analyser.getFloatFrequencyData(spectrum);
        let peak=1;for(let i=2;i<spectrum.length;i++)if(spectrum[i]>spectrum[peak])peak=i;
        return {rms:Math.sqrt(samples.reduce((sum,n)=>sum+n*n,0)/samples.length),frequency:peak*audio.sampleRate/m.analyser.fftSize};
      };
      await room.connect(base.replace('http:','ws:'),token,{rtcConfig:{iceServers:[]}});
    },{base,token:await token.toJwt(),single,red,encrypted});
    assert.equal(await page.evaluate(()=>room.engine.pcManager.mode==='publisher-only'),single);return page;
  }
  async function media(page,sid,codec,frequency){
    const before=await page.evaluate(sid=>stats(sid),sid);let evidence;
    await eventually(async()=>{
      evidence=await page.evaluate(async sid=>({stats:await stats(sid),tone:tone(sid)}),sid);
      return evidence.stats.some(s=>s.codec?.toLowerCase()===`audio/${codec}`&&s.clockRate===(codec==='opus'?48000:8000)&&s.packets>5&&s.samples>1000&&s.samples>(before.find(b=>b.id===s.id)?.samples||0)+500&&s.energy>(before.find(b=>b.id===s.id)?.energy||0))&&evidence.tone?.rms>0.03&&Math.abs(evidence.tone.frequency-frequency)<50;
    },`${codec} actual decoded ${frequency}Hz tone with correct RTP clock`);return evidence;
  }
  async function check(name,fn){try{const evidence=await fn();record.cases.push({name,passed:true,evidence});console.log(`${kind}: PASS ${name}`);}catch(e){record.cases.push({name,passed:false,failure:e.stack});throw e;}}
  try{
    await eventually(async()=>{if(spawnError)throw spawnError;if(child.exitCode!==null)throw Error('Server exited '+child.exitCode);return(await fetch(kind==='go'?base:`${base}/healthz`).catch(()=>null))?.ok;},'startup',60000);
    for(const single of [false,true]){
      const mode=single?'single':'dual',roomName=`audio-${mode}`;await api.createRoom({name:roomName});
      const alice=await client(roomName,'alice',single),bob=await client(roomName,'bob',single);let pcmu,pcma,opus,reverse;
      await check(`${mode}: PCMU actually decodes at 8 kHz RTP clock`,async()=>{
        pcmu=await alice.evaluate(()=>publish('pcmu',440));const decoded=await media(bob,pcmu,'pcmu',440);
        const sent=await alice.evaluate(sid=>stats(sid,true),pcmu);assert.ok(sent.some(s=>s.codec==='audio/PCMU'&&s.clockRate===8000));return {sent,decoded};
      });
      await check(`${mode}: PCMA and PCMU coexist without payload confusion`,async()=>{
        pcma=await alice.evaluate(()=>publish('pcma',880));return [await media(bob,pcma,'pcma',880),await media(bob,pcmu,'pcmu',440)];
      });
      await check(`${mode}: Opus and both G711 codecs coexist`,async()=>{
        opus=await alice.evaluate(()=>publish('opus',1320));return [await media(bob,opus,'opus',1320),await media(bob,pcma,'pcma',880),await media(bob,pcmu,'pcmu',440)];
      });
      if(events){
        await check(`${mode}: active speaker mute and restart`,async()=>{
          await eventually(()=>bob.evaluate(()=>room.activeSpeakers.some(p=>p.identity==='alice' && p.audioLevel>0)),'active speaker');
          await alice.evaluate(()=>Promise.all([...published.values()].map(p=>p.track.mute())));
          await eventually(()=>bob.evaluate(()=>!room.activeSpeakers.some(p=>p.identity==='alice')),'inactive speaker tombstone');
          await alice.evaluate(()=>Promise.all([...published.values()].map(p=>p.track.unmute())));
          await eventually(()=>bob.evaluate(()=>room.activeSpeakers.some(p=>p.identity==='alice')),'speaker restart');
          return bob.evaluate(()=>speakerEvents);
        });
        await check(`${mode}: connection quality for subscribed publisher`,async()=>{
          await eventually(()=>bob.evaluate(()=>qualityEvents.some(e=>e.identity==='alice')),'publisher connection quality');
          return bob.evaluate(()=>qualityEvents);
        });
      }
      await check(`${mode}: TrackInfo reports actual negotiated audio MIME`,async()=>{
        const p=await api.getParticipant(roomName,'alice');for(const [sid,codec] of [[pcmu,'pcmu'],[pcma,'pcma'],[opus,red?'red':'opus']])assert.equal(p.tracks.find(t=>t.sid===sid)?.mimeType.toLowerCase(),`audio/${codec}`);return p.tracks;
      });
      await check(`${mode}: reverse PCMA coexists with all forward tracks`,async()=>{
        reverse=await bob.evaluate(()=>publish('pcma',1760));return [await media(alice,reverse,'pcma',1760),await media(bob,pcmu,'pcmu',440),await media(bob,opus,'opus',1320)];
      });
      await check(`${mode}: PCMU publisher mute and resume restores tone`,async()=>{
        await alice.evaluate(sid=>published.get(sid).track.mute(),pcmu);await delay(600);
        await alice.evaluate(sid=>published.get(sid).track.unmute(),pcmu);return media(bob,pcmu,'pcmu',440);
      });
      await check(`${mode}: PCMA resubscription retains codec and tone`,async()=>{
        await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setSubscribed(false),pcma);
        await eventually(()=>bob.evaluate(sid=>!tracks.has(sid),pcma),'unsubscribed');
        await bob.evaluate(sid=>room.remoteParticipants.get('alice').trackPublications.get(sid).setSubscribed(true),pcma);return media(bob,pcma,'pcma',880);
      });
      await check(`${mode}: late subscriber decodes four distinct tones`,async()=>{
        const late=await client(roomName,'late',single);return [await media(late,pcmu,'pcmu',440),await media(late,pcma,'pcma',880),await media(late,opus,'opus',1320),await media(late,reverse,'pcma',1760)];
      });
      await check(`${mode}: signal resume preserves mixed audio and session`,async()=>{
        const sid=await alice.evaluate(()=>{window.resumeSeen=false;window.originalPC=room.engine.pcManager.publisher.pc;room.on(LivekitClient.RoomEvent.SignalReconnecting,()=>resumeSeen=true);room.engine.client.ws.close({closeCode:4000,reason:'audio parity resume'});return room.localParticipant.sid;});
        await eventually(()=>alice.evaluate(()=>resumeSeen&&room.state==='connected'&&room.engine.client.currentState===1),'audio signal resume');
        assert.equal(await alice.evaluate(()=>originalPC===room.engine.pcManager.publisher.pc&&room.localParticipant.sid),sid);
        return [await media(bob,pcmu,'pcmu',440),await media(bob,pcma,'pcma',880),await media(bob,opus,'opus',1320),await media(alice,reverse,'pcma',1760)];
      });
      for(const context of contexts.splice(0))await context.close();pages.length=0;await api.deleteRoom(roomName);
    }
    record.completed=true;
  }catch(e){record.failure=e.stack;console.error(`${kind}: ${e.stack}`);
    record.diagnostics=await Promise.all(pages.map(async page=>{try{return await page.evaluate(async()=>({mode:room.engine.pcManager?.mode,codecSelections,descriptionHistory,localSdp:room.engine.pcManager?.publisher.pc.localDescription?.sdp,answer:room.engine.pcManager?.publisher.pc.remoteDescription?.sdp,subscriberSdp:room.engine.pcManager?.subscriber?.pc.remoteDescription?.sdp,published:await Promise.all([...published.keys()].map(async sid=>({sid,stats:await stats(sid,true)}))),received:await Promise.all([...tracks.keys()].map(async sid=>({sid,stats:await stats(sid),tone:tone(sid)})))}));}catch(e){return{error:String(e)};}}));
  }finally{for(const c of contexts)await c.close();if(child.exitCode===null&&child.signalCode===null){const exited=once(child,'exit');child.kill();await exited;}writeFileSync(resolve(out,`${kind}.log`),log);unlinkSync(config);}
}
try{
  assets.listen(0,'127.0.0.1');await once(assets,'listening');browser=await chromium.launch({channel:'chrome',headless:true,args:['--autoplay-policy=no-user-gesture-required','--disable-background-timer-throttling']});report.browser=browser.version();
  if(!opts.only || opts.only==='go')await exercise('go',opts['go-server']);
  if(!opts.only || opts.only==='cpp')await exercise('cpp',opts['cpp-server']);
  if(opts.only)process.exitCode=report.servers[opts.only].completed?0:1;
  else{for(const expected of report.servers.go.cases){const actual=report.servers.cpp.cases.find(c=>c.name===expected.name);report.comparisons.push({name:expected.name,passed:expected.passed&&actual?.passed===true});}process.exitCode=report.servers.go.completed&&report.servers.cpp.completed&&report.comparisons.length===expectedCases&&report.comparisons.every(c=>c.passed)?0:1;}
}catch(e){report.infrastructureError=e.stack;process.exitCode=2;}
finally{await browser?.close();await new Promise(r=>assets.close(r));report.exitCode=process.exitCode||0;writeFileSync(resolve(out,'report.json'),JSON.stringify(report,null,2));console.log(opts.only?`${opts.only} diagnostic: ${report.servers[opts.only]?.cases.filter(c=>c.passed).length}/${expectedCases}; ${out}`:`${report.comparisons.filter(c=>c.passed).length}/${expectedCases} matched; ${out}`);}
