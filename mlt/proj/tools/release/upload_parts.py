import json, subprocess, sys, os
up = sys.argv[1]
sha = sys.argv[2]
os.chdir('C:/mlt/parts')
for f in os.listdir('.'):
    os.remove(f)
subprocess.run(['split', '-b', '67108864', '-d', '-a', '4', 'C:/mlt/BotStrike-0.2.0.zip', 'part-'], check=True)
n = len(os.listdir('.'))
json.dump({"uploadId": up, "partNumbers": list(range(1, n + 1))}, open('C:/mlt/a.json', 'w'))
out = subprocess.run([sys.executable, 'C:/mlt/proj/tools/release/melty.py', 'upload_part_urls', 'C:/mlt/a.json'], capture_output=True, text=True, encoding='utf-8').stdout
j = json.loads(out[out.index('{"uploadId"'):].splitlines()[0])
for p in j['parts']:
    f = 'C:/mlt/parts/part-%04d' % (p['partNumber'] - 1)
    r = subprocess.run(['curl.exe', '-sS', '-f', '-X', 'PUT', '--upload-file', f, '-w', '%{http_code}', p['url']], capture_output=True, text=True)
    print(p['partNumber'], r.stdout[-5:], r.stderr[:300])
json.dump({"uploadId": up, "sha256": sha}, open('C:/mlt/a.json', 'w'))
print(subprocess.run([sys.executable, 'C:/mlt/proj/tools/release/melty.py', 'finish_upload', 'C:/mlt/a.json'], capture_output=True, text=True, encoding='utf-8').stdout[:400])
