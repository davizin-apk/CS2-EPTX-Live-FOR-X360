import zipfile, os, json, hashlib, sys
src = 'C:/mlt/pkg'
out = sys.argv[1]
entries = []
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for root, _, files in os.walk(src):
        for f in files:
            p = os.path.join(root, f)
            rel = os.path.relpath(p, src).replace(os.sep, '/')
            z.write(p, rel)
            entries.append({"path": rel, "size": os.path.getsize(p)})
h = hashlib.sha256(open(out, 'rb').read()).hexdigest()
info = {"entries": entries, "sha": h, "size": os.path.getsize(out)}
json.dump(info, open('C:/mlt/pkginfo.json', 'w'))
print(info)
