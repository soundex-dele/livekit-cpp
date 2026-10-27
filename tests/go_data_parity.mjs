// Official SDK streams plus raw data-channel envelopes against pinned Go/C++.
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

const here=dirname(fileURLToPath(import.meta.url));
const opts=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>{
  if(v.startsWith('--'))a.push([v.slice(2),all[i+1]]); return a;
},[]));
assert.ok(opts['go-server'] && opts['cpp-server'],'Pass --go-server and --cpp-server');
const deps=resolve(opts['node-modules']||'D:/workspace/livekit-cpp/tests/node_modules');
const modernDeps=resolve(opts['modern-node-modules']||'D:/workspace/livekit-cli-cpp/web/node_modules');
const require=createRequire(resolve(deps,'../package.json'));
const modernRequire=createRequire(resolve(modernDeps,'../package.json'));
const {chromium}=require('playwright');
const {AccessToken,RoomServiceClient}=require('livekit-server-sdk');
const {DataPacket}=modernRequire('@livekit/protocol');
const out=resolve(opts.output||resolve(here,'results/go-data-parity'),new Date().toISOString().replaceAll(/[:.]/g,'-')+'-'+randomBytes(3).toString('hex'));
mkdirSync(out,{recursive:true});
const report={fullReplacementVerified:false,scope:'SDK text/byte streams and data-envelope routing, permissions, identity, deduplication',
  mode:opts.mode||'single',sdkVersion:JSON.parse(readFileSync(resolve(modernDeps,'livekit-client/package.json'))).version,servers:{},comparisons:[]};
assert.ok(['single','dual'].includes(report.mode),'--mode must be single or dual');
const delay=ms=>new Promise(r=>setTimeout(r,ms));
async function eventually(fn,label,timeout=15000){const end=Date.now()+timeout;while(Date.now()<end){if(await fn())return;await delay(50);}throw Error('Timed out: '+label);}
async function port(){const s=tcpServer();s.listen(0,'127.0.0.1');await once(s,'listening');const p=s.address().port;await new Promise(r=>s.close(r));return p;}
const sdk=readFileSync(resolve(modernDeps,'livekit-client/dist/livekit-client.umd.js'));
const assets=createServer((req,res)=>{if(req.url==='/sdk.js'){res.setHeader('Content-Type','application/javascript');res.end(sdk);}else res.end('<!doctype html><script src="/sdk.js"></script>');});
let browser;
async function exercise(kind,binary){
  const key='datatest',secret=randomBytes(32).toString('hex'),roomName='data-parity';
  const p=await port(),base=`http://127.0.0.1:${p}`;
  const config=resolve(out,`${kind}.${kind==='go'?'yaml':'json'}`);
  writeFileSync(config,kind==='go'?`port: ${p}\nbind_addresses: [127.0.0.1]\nrtc:\n  tcp_port: 0\n  port_range_start: 61000\n  port_range_end: 61180\n  use_external_ip: false\n  node_ip: 127.0.0.1\n  enable_loopback_candidate: true\nkeys:\n  ${key}: ${secret}\n`:
    JSON.stringify({bind:'127.0.0.1',port:p,keys:{[key]:secret},rtc:{port_range_start:61200,port_range_end:61380}}));
  const record=report.servers[kind]={binary:resolve(binary),sha256:createHash('sha256').update(readFileSync(binary)).digest('hex'),cases:[]};
  const child=spawn(resolve(binary),['--config',config],{windowsHide:true});
  let log='',spawnError;child.stdout.on('data',b=>log+=b);child.stderr.on('data',b=>log+=b);child.on('error',e=>spawnError=e);
  const contexts=[],clients=[];const api=new RoomServiceClient(base,key,secret);
  async function client(identity,grant={},participantKind){
    const token=new AccessToken(key,secret,{identity,ttl:'15m'});if(participantKind)token.kind=participantKind;
    token.addGrant({roomJoin:true,room:roomName,...grant});
    const context=await browser.newContext();contexts.push(context);const page=await context.newPage();
    page.on('pageerror',e=>log+=`[${identity}] ${e.stack}\n`);
    await page.goto(`http://127.0.0.1:${assets.address().port}`);
    await page.evaluate(async({base,token,single})=>{
      window.wire=[];window.channels=[];window.texts=[];window.blobs=[];window.streamErrors=[];
      const NativePC=window.RTCPeerConnection;
      function tap(channel,local){channels.push({channel,local});channel.addEventListener('message',async e=>{
        const bytes=e.data instanceof Blob?await e.data.arrayBuffer():e.data;
        if(typeof bytes!=='string')wire.push(Array.from(new Uint8Array(bytes)));
      });}
      window.RTCPeerConnection=class extends NativePC{
        constructor(...args){super(...args);this.addEventListener('datachannel',e=>tap(e.channel,false));}
        createDataChannel(...args){const c=super.createDataChannel(...args);tap(c,true);return c;}
      };
      window.room=new LivekitClient.Room({singlePeerConnection:single});
      room.registerTextStreamHandler('text',async(reader,sender)=>{
        try{texts.push({info:reader.info,sender,text:await reader.readAll()});}catch(e){streamErrors.push(String(e));}
      });
      room.registerByteStreamHandler('bytes',async(reader,sender)=>{
        try{const chunks=await reader.readAll();blobs.push({info:reader.info,sender,bytes:chunks.flatMap(c=>Array.from(c))});}catch(e){streamErrors.push(String(e));}
      });
      await room.connect(base.replace('http:','ws:'),token,{rtcConfig:{iceServers:[]}});
    },{base,token:await token.toJwt(),single:report.mode==='single'});
    assert.equal(await page.evaluate(()=>room.engine.pcManager.mode==='publisher-only'),report.mode==='single');
    assert.equal(await page.evaluate(()=>!!room.engine.pcManager.subscriber),report.mode==='dual');
    const c={page,identity,sid:await page.evaluate(()=>room.localParticipant.sid)};clients.push(c);return c;
  }
  async function check(name,fn){const start=Date.now();try{await fn();record.cases.push({name,passed:true,elapsedMs:Date.now()-start});console.log(`${kind}: PASS ${name}`);}catch(e){record.cases.push({name,passed:false,failure:e.stack});console.error(`${kind}: FAIL ${name}: ${e.message}`);}}
  async function raw(c,json,reliable=true){const bytes=Array.from(DataPacket.fromJson(json).toBinary());await c.page.evaluate(async({bytes,reliable})=>{
    await room.engine.ensurePublisherConnected(reliable?0:1);
    const ch=channels.find(c=>c.local&&c.channel.label===(reliable?'_reliable':'_lossy')&&c.channel.readyState==='open');
    if(!ch)throw Error('No open publisher data channel');ch.channel.send(new Uint8Array(bytes));
  },{bytes,reliable});}
  async function packets(c){return(await c.page.evaluate(()=>wire)).map(bytes=>DataPacket.fromBinary(new Uint8Array(bytes)).toJson());}
  async function clear(){for(const c of clients)await c.page.evaluate(()=>{wire.length=0;});}
  const user=(topic,extra={})=>({user:{topic,payload:Buffer.from(topic).toString('base64')},...extra});
  async function has(c,topic){return(await packets(c)).some(p=>p.user?.topic===topic);}
  async function count(c,topic){return(await packets(c)).filter(p=>p.user?.topic===topic).length;}
  async function wait(c,topic){await eventually(()=>has(c,topic),`${c.identity} receives ${topic}`);return(await packets(c)).find(p=>p.user?.topic===topic);}
  try{
    await eventually(async()=>{if(spawnError)throw spawnError;if(child.exitCode!==null)throw Error('Server exited '+child.exitCode);return(await fetch(kind==='go'?base:`${base}/healthz`).catch(()=>null))?.ok;},'startup',60000);
    await api.createRoom({name:roomName});
    const alice=await client('alice'),bob=await client('bob'),charlie=await client('charlie');
    await check('official SDK Unicode text stream preserves content and attributes',async()=>{
      const text='你好🌍 streamed text\n'.repeat(5000);
      const info=await alice.page.evaluate(text=>room.localParticipant.sendText(text,{topic:'text',attributes:{source:'parity'}}),text);
      for(const c of [bob,charlie]){await eventually(()=>c.page.evaluate(id=>texts.some(x=>x.info.id===id),info.id),'text complete');
        const got=await c.page.evaluate(id=>texts.find(x=>x.info.id===id),info.id);assert.equal(got.text,text);assert.equal(got.sender.identity,'alice');assert.equal(got.info.attributes.source,'parity');}
    });
    await check('official SDK directed incremental byte stream preserves 256 KiB',async()=>{
      const info=await alice.page.evaluate(async()=>{
        const writer=await room.localParticipant.streamBytes({topic:'bytes',name:'binary.bin',mimeType:'application/octet-stream',totalSize:262144,destinationIdentities:['bob']});
        for(let i=0;i<32;i++)await writer.write(Uint8Array.from({length:8192},(_,j)=>(i*8192+j)%251));
        await writer.close();return writer.info;
      });
      await eventually(()=>bob.page.evaluate(id=>blobs.some(x=>x.info.id===id),info.id),'byte stream complete');
      const got=await bob.page.evaluate(id=>blobs.find(x=>x.info.id===id),info.id);
      assert.deepEqual(got.bytes,Array.from({length:262144},(_,i)=>i%251));assert.equal(got.info.name,'binary.bin');
      assert.equal(await charlie.page.evaluate(()=>blobs.length),0);
    });
    await check('top-level destinations override legacy identity list',async()=>{
      await clear();await raw(alice,{user:{topic:'override',destinationIdentities:['charlie']},destinationIdentities:['bob']});
      const p=await wait(bob,'override');assert.deepEqual(p.user.destinationIdentities,['bob']);await delay(350);assert.equal(await has(charlie,'override'),false);
    });
    await check('legacy destination identities are promoted to envelope',async()=>{
      await clear();await raw(alice,{user:{topic:'legacy',destinationIdentities:['bob']}});const p=await wait(bob,'legacy');assert.deepEqual(p.destinationIdentities,['bob']);
    });
    await check('destination identities and legacy SIDs form a union',async()=>{
      await clear();await raw(alice,{user:{topic:'union',destinationSids:[charlie.sid]},destinationIdentities:['bob']});await wait(bob,'union');await wait(charlie,'union');
    });
    await check('ordinary participants cannot spoof sender identity',async()=>{
      await clear();await raw(alice,user('identity',{participantIdentity:'forged',participantSid:'PA_forged'}));const p=await wait(bob,'identity');
      assert.equal(p.participantIdentity,'alice');assert.equal(p.participantSid,alice.sid);assert.equal(p.user.participantIdentity,'alice');assert.equal(p.user.participantSid,alice.sid);
    });
    const agent=await client('agent',{},'agent');
    await check('agent chat preserves represented identity and marks generated',async()=>{
      await clear();await raw(agent,{participantIdentity:'alice',chatMessage:{id:'chat',message:'generated'}});
      await eventually(async()=>(await packets(bob)).some(p=>p.chatMessage?.id==='chat'),'agent chat');const p=(await packets(bob)).find(p=>p.chatMessage?.id==='chat');
      assert.equal(p.participantIdentity,'alice');assert.equal(p.participantSid,agent.sid);assert.equal(p.chatMessage.generated,true);
    });
    await check('agent text header preserves represented identity and marks generated',async()=>{
      await clear();await raw(agent,{participantIdentity:'alice',streamHeader:{streamId:'agent-text',topic:'raw',textHeader:{}}});
      await eventually(async()=>(await packets(bob)).some(p=>p.streamHeader?.streamId==='agent-text'),'agent text');const p=(await packets(bob)).find(p=>p.streamHeader?.streamId==='agent-text');
      assert.equal(p.participantIdentity,'alice');assert.equal(p.streamHeader.textHeader.generated,true);
    });
    await check('agent byte header still asserts actual sender identity',async()=>{
      await clear();await raw(agent,{participantIdentity:'alice',streamHeader:{streamId:'agent-byte',topic:'raw',byteHeader:{name:'raw'}}});
      await eventually(async()=>(await packets(bob)).some(p=>p.streamHeader?.streamId==='agent-byte'),'agent byte');const p=(await packets(bob)).find(p=>p.streamHeader?.streamId==='agent-byte');assert.equal(p.participantIdentity,'agent');
    });
    await check('transcription is forwarded only from an agent',async()=>{
      await clear();await raw(alice,{transcription:{trackId:'ordinary'}});await raw(agent,{transcription:{trackId:'agent'}});
      await eventually(async()=>(await packets(bob)).some(p=>p.transcription?.trackId==='agent'),'agent transcription');await delay(350);
      assert.equal((await packets(bob)).some(p=>p.transcription?.trackId==='ordinary'),false);
    });
    await check('metrics are not broadcast to ordinary data recipients',async()=>{
      await clear();await raw(alice,{metrics:{}});await raw(alice,user('metrics-barrier'));await wait(bob,'metrics-barrier');assert.equal((await packets(bob)).some(p=>'metrics'in p),false);
    });
    await check('reliable publisher sequence is preserved and duplicates discarded',async()=>{
      await clear();await raw(alice,user('seq',{sequence:100000}));await raw(alice,user('seq',{sequence:100000}));await raw(alice,user('old-seq',{sequence:99999}));await raw(alice,user('seq-end',{sequence:100001}));
      const end=await wait(bob,'seq-end');assert.equal(end.sequence,100001);assert.equal(await count(bob,'seq'),1);assert.equal(await count(bob,'old-seq'),0);
    });
    await check('sequence zero is accepted without advancing replay cursor',async()=>{
      await clear();await raw(alice,user('zero'));const p=await wait(bob,'zero');assert.equal(p.sequence||0,0);
    });
    await check('reliable sequence cursors are independent for each sender',async()=>{
      await clear();await raw(charlie,user('other-seq',{sequence:100000}));const p=await wait(bob,'other-seq');assert.equal(p.sequence,100000);assert.equal(p.participantSid,charlie.sid);
    });
    await check('lossy channel overrides claimed kind and bypasses reliable dedup',async()=>{
      await clear();await raw(alice,user('lossy',{kind:'RELIABLE',sequence:7}),false);const p=await wait(bob,'lossy');assert.equal(p.kind,'LOSSY');assert.equal(p.sequence,7);
    });
    await check('room user nonces deduplicate across senders',async()=>{
      await clear();const nonce=Buffer.from('1234567890abcdef').toString('base64');await raw(alice,{user:{topic:'nonce',nonce}});await wait(bob,'nonce');await raw(charlie,{user:{topic:'nonce',nonce}});await raw(charlie,user('nonce-end'));await wait(bob,'nonce-end');assert.equal(await count(bob,'nonce'),1);
    });
    await check('invalid length nonce does not deduplicate',async()=>{
      await clear();for(let i=0;i<2;i++)await raw(alice,{user:{topic:'short-nonce',nonce:'AQID'}});await raw(alice,user('short-end'));await wait(bob,'short-end');assert.equal(await count(bob,'short-nonce'),2);
    });
    const nosub=await client('no-media-subscribe',{canSubscribe:false});
    await check('media subscribe permission does not block data reception',async()=>{
      await clear();await raw(alice,user('no-media-sub',{destinationIdentities:[nosub.identity]}));await wait(nosub,'no-media-sub');
    });
    const blocked=await client('blocked',{canPublishData:false});
    await check('canPublishData false rejects raw data streams',async()=>{
      await clear();await raw(blocked,{streamHeader:{streamId:'forbidden',topic:'raw',textHeader:{}}});await raw(blocked,user('forbidden'));await delay(500);
      assert.equal((await packets(bob)).some(p=>p.streamHeader?.streamId==='forbidden'||p.user?.topic==='forbidden'),false);
    });
    const hidden=await client('hidden',{hidden:true});
    await check('hidden user strips public identity while preserving envelope SID',async()=>{
      await clear();await raw(hidden,user('hidden',{participantIdentity:'forged'}));const p=await wait(bob,'hidden');
      assert.equal(p.participantIdentity||'','');assert.equal(p.participantSid,hidden.sid);assert.equal(p.user.participantIdentity||'','');assert.equal(p.user.participantSid||'','');
    });
    const legacyAgent=await client('legacy-agent',{agent:true});
    await check('legacy agent grant has represented text semantics',async()=>{
      await clear();await raw(legacyAgent,{participantIdentity:'alice',chatMessage:{id:'legacy-agent',message:'generated'}});
      await eventually(async()=>(await packets(bob)).some(p=>p.chatMessage?.id==='legacy-agent'),'legacy agent chat');const p=(await packets(bob)).find(p=>p.chatMessage?.id==='legacy-agent');assert.equal(p.participantIdentity,'alice');assert.equal(p.chatMessage.generated,true);
    });
    await check('signal resume returns publisher cursor and retains duplicate suppression',async()=>{
      await clear();await alice.page.evaluate(()=>{
        window.resumeResponses=[];window.originalPCs=[room.engine.pcManager.publisher.pc,room.engine.pcManager.subscriber?.pc];
        const reconnect=room.engine.client.reconnect.bind(room.engine.client);
        room.engine.client.reconnect=async(...args)=>{const res=await reconnect(...args);resumeResponses.push(res?.lastMessageSeq||0);return res;};
        room.engine.client.ws.close({closeCode:4000,reason:'data parity resume'});
      });
      await eventually(()=>alice.page.evaluate(()=>resumeResponses.length>0&&room.state==='connected'&&room.engine.client.currentState===1),'data signal resume',25000);
      assert.deepEqual(await alice.page.evaluate(()=>resumeResponses),[100001]);
      assert.equal(await alice.page.evaluate(()=>originalPCs[0]===room.engine.pcManager.publisher.pc&&originalPCs[1]===room.engine.pcManager.subscriber?.pc&&room.localParticipant.sid),alice.sid);
      await raw(alice,user('replayed',{sequence:100000}));await raw(alice,user('after-resume',{sequence:100002}));await wait(bob,'after-resume');assert.equal(await count(bob,'replayed'),0);
    });
    record.completed=true;
  }catch(e){record.failure=e.stack;console.error(`${kind}: ${e.stack}`);}
  finally{
    for(const c of contexts)await c.close();
    if(child.exitCode===null&&child.signalCode===null){const exited=once(child,'exit');child.kill();await exited;}
    writeFileSync(resolve(out,`${kind}.log`),log);unlinkSync(config);
  }
}
try{
  assets.listen(0,'127.0.0.1');await once(assets,'listening');browser=await chromium.launch({channel:'chrome',headless:true,args:['--disable-background-timer-throttling']});report.browser=browser.version();
  await exercise('go',opts['go-server']);await exercise('cpp',opts['cpp-server']);
  for(const expected of report.servers.go.cases){const actual=report.servers.cpp.cases.find(c=>c.name===expected.name);report.comparisons.push({name:expected.name,passed:expected.passed&&actual?.passed===true});}
  process.exitCode=report.servers.go.completed&&report.servers.cpp.completed&&report.comparisons.every(c=>c.passed)?0:1;
}catch(e){report.infrastructureError=e.stack;process.exitCode=2;}
finally{await browser?.close();await new Promise(r=>assets.close(r));report.exitCode=process.exitCode||0;writeFileSync(resolve(out,'report.json'),JSON.stringify(report,null,2));console.log(`${report.comparisons.filter(c=>c.passed).length}/${report.comparisons.length} matched; ${out}`);}
