import io,sys
sys.stdout=io.TextIOWrapper(sys.stdout.buffer,encoding="utf-8")
import json, sys, subprocess, os
tool = sys.argv[1]
args = json.load(open(sys.argv[2], encoding='utf-8')) if len(sys.argv) > 2 else {}
req = {"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":tool,"arguments":args}}
open('C:/mlt/_req.json','w',encoding='utf-8').write(json.dumps(req))
tok = open('C:/mlt/.tok').read().strip()
out = subprocess.run(['curl.exe','-s','https://melty.gg/api/mcp','-H','Authorization: Bearer '+tok,'-H','content-type: application/json','-H','accept: application/json, text/event-stream','--data-binary','@C:/mlt/_req.json'],capture_output=True)
txt = out.stdout.decode('utf-8','replace')
if txt.startswith('event:') or 'data:' in txt[:20]:
    txt = '\n'.join(l[5:] for l in txt.splitlines() if l.startswith('data:'))
try:
    d = json.loads(txt)
except Exception:
    print(txt[:4000]); sys.exit(1)
if 'error' in d: print('RPC ERROR', json.dumps(d['error'])); sys.exit(1)
r = d['result']
if r.get('isError'): print('TOOL ERROR')
for c in r.get('content', []):
    if c.get('type') == 'text': print(c['text'])
