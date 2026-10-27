"""Launch the pinned servers; verify configuration precedence by authenticated API calls."""
import argparse, base64, hashlib, hmac, json, os, secrets, socket, subprocess, time
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.error import HTTPError, URLError

parser=argparse.ArgumentParser()
parser.add_argument('--go-server', required=True)
parser.add_argument('--cpp-server', required=True)
parser.add_argument('--output', default='tests/results/go-config-parity')
args=parser.parse_args()
out=Path(args.output).resolve()/f'{time.strftime("%Y%m%dT%H%M%SZ",time.gmtime())}-{secrets.token_hex(3)}'
out.mkdir(parents=True)
source=Path(__file__).read_bytes(); (out/'go_config_parity.py').write_bytes(source)
report={'fullReplacementVerified':False,'scope':'YAML/JSON, config-body/LIVEKIT_CONFIG and keys precedence',
        'testSha256':hashlib.sha256(source).hexdigest(),'servers':{},'comparisons':[]}
secret=secrets.token_hex(24)

def token(key,signing_secret):
    def encode(value): return base64.urlsafe_b64encode(json.dumps(value,separators=(',',':')).encode()).rstrip(b'=')
    body=encode({'alg':'HS256','typ':'JWT'})+b'.'+encode({'iss':key,'exp':int(time.time())+120,'video':{'roomList':True}})
    return (body+b'.'+base64.urlsafe_b64encode(hmac.new(signing_secret.encode(),body,hashlib.sha256).digest()).rstrip(b'=')).decode()

def request(port,key,signing_secret=secret):
    req=Request(f'http://127.0.0.1:{port}/twirp/livekit.RoomService/ListRooms',data=b'{}',
                headers={'Authorization':'Bearer '+token(key,signing_secret),'Content-Type':'application/json'})
    try:
        with urlopen(req,timeout=1) as response: return response.status
    except HTTPError as error: return error.code
    except (URLError,TimeoutError,OSError): return None

def exercise(kind,binary):
    record=report['servers'][kind]={'sha256':hashlib.sha256(Path(binary).read_bytes()).hexdigest(),'cases':[]}
    names=['yaml-file','json-file','inline-body','environment-body','body-over-file','explicit-body-over-env',
           'environment-keys','cli-keys-over-env','keys-replace-file-map','dev-preserves-config-keys',
           'empty-keys-rejected','unknown-field-rejected','invalid-room-boolean','duplicate-keys-rejected',
           'cli-yaml-word-secret','cli-key-named-keys-numeric-rejected']
    for name in names:
        with socket.socket() as sock:
            sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
        config={'port':port,'bind_addresses':['127.0.0.1'],'keys':{'filekey':secret},'room':{'enable_remote_unmute':True}}
        body=f'port: {port}\nbind_addresses: [127.0.0.1]\nkeys:\n  filekey: {secret}\nroom:\n  enable_remote_unmute: true\n'
        env={k:v for k,v in os.environ.items() if not k.startswith('LIVEKIT_') and k not in ['NODE_IP','UDP_PORT','REDIS_HOST','REDIS_PASSWORD']}
        if kind=='go': env['LIVEKIT_RTC_TCP_PORT']='0'
        expected_key='filekey'; rejected_key=None; expect_start=True
        signing_secret=secret
        rejected_secret=secret
        extra=[]; path=out/f'{kind}-{name}.yaml'; text=body
        if name=='json-file': text=json.dumps(config)
        command=['--config',str(path)]
        if name=='inline-body': command=['--config-body',body]
        if name=='environment-body': command=[]; env['LIVEKIT_CONFIG']=body
        if name=='body-over-file':
            command=['--config',str(out/'does-not-exist.yaml')]; env['LIVEKIT_CONFIG']=body
        if name=='explicit-body-over-env':
            command=['--config-body',body]; env['LIVEKIT_CONFIG']='not_a_config: true'
        if name in ['environment-keys','cli-keys-over-env','keys-replace-file-map']:
            expected_key='overridekey'; rejected_key='filekey'
            env['LIVEKIT_KEYS']=f'overridekey: {secret}'
            if name=='environment-keys': text=f'port: {port}\nbind_addresses: [127.0.0.1]\n'
            if name=='cli-keys-over-env':
                env['LIVEKIT_KEYS']=f'wrongkey: {secret}'; extra=['--keys',f'overridekey: {secret}']; rejected_key='wrongkey'
        if name=='dev-preserves-config-keys': extra=['--dev']; rejected_key='devkey'; rejected_secret='secret'
        if name=='empty-keys-rejected': text=f'port: {port}\nkeys: {{}}\n'; expect_start=False
        if name=='unknown-field-rejected': text=body+'unimplemented_option: true\n'; expect_start=False
        if name=='invalid-room-boolean': text=body.replace('enable_remote_unmute: true','enable_remote_unmute: [true]'); expect_start=False
        if name=='duplicate-keys-rejected': text=body+'port: 1\n'; expect_start=False
        if name=='cli-yaml-word-secret': expected_key='overridekey'; signing_secret='on'; extra=['--keys','overridekey: on']
        if name=='cli-key-named-keys-numeric-rejected': extra=['--keys','keys: 1234']; expect_start=False
        path.write_text(text,encoding='utf-8')
        log_path=out/f'{kind}-{name}.log'
        with log_path.open('wb') as log:
            child=subprocess.Popen([str(Path(binary).resolve()),*command,*extra],env=env,stdout=log,stderr=subprocess.STDOUT,
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            status=None; rejected=None
            try:
                end=time.monotonic()+15
                while time.monotonic()<end and child.poll() is None:
                    status=request(port,expected_key,signing_secret)
                    if status is not None: break
                    time.sleep(.05)
                if status is not None and rejected_key: rejected=request(port,rejected_key,rejected_secret)
                passed=(status==200 and (not rejected_key or rejected==401)) if expect_start else child.poll() is not None and child.returncode!=0 and status is None
                row={'name':name,'passed':passed,'expectedStartup':expect_start,'status':status,'rejectedKeyStatus':rejected,'exitCode':child.poll()}
                record['cases'].append(row); print(kind,name,'PASS' if passed else 'FAIL',flush=True)
            finally:
                if child.poll() is None:
                    child.terminate()
                    try: child.wait(timeout=10)
                    except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=5)
                path.unlink()
        # Never retain generated credentials, including errors that echo input.
        log_path.write_text(log_path.read_text(encoding='utf-8',errors='replace').replace(secret,'[test-secret]'),encoding='utf-8')

try:
    for kind,binary in [('go',args.go_server),('cpp',args.cpp_server)]: exercise(kind,binary)
    for expected,actual in zip(report['servers']['go']['cases'],report['servers']['cpp']['cases']):
        report['comparisons'].append({'name':expected['name'],'passed':expected['passed'] and actual['passed']})
    report['exitCode']=0 if len(report['comparisons'])==16 and all(c['passed'] for c in report['comparisons']) else 1
except Exception as error:
    report['infrastructureError']=str(error).replace(secret,'[test-secret]'); report['exitCode']=2
finally:
    (out/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(out)
raise SystemExit(report['exitCode'])
