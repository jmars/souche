# preserved from /tmp/flagref (compares make selftest CASE lines with the reference output)
import re, subprocess
st = subprocess.run(['make','selftest'],capture_output=True,text=True,cwd='/home/jaye/futaie/souche')
lines = st.stdout + st.stderr
port = {}
for m in re.finditer(r'CASE\s+(\d+) .*-> flags=0x([0-9a-f]+) table=0x([0-9a-f]+)', lines):
    port[int(m.group(1))] = (m.group(2), m.group(3))
ref = {}
r = subprocess.run(['node','run.mjs'],capture_output=True,text=True,cwd='/tmp/flagref')
for m in re.finditer(r'CASE\s+(\d+) ref=0x([0-9a-f]+)', r.stdout):
    ref[int(m.group(1))] = m.group(2)
assert len(port)==24 and len(ref)==24, (len(port), len(ref))
bad=[]
for i in range(24):
    p, t = port[i]
    if p != ref[i] or t != ref[i]:
        bad.append((i, p, t, ref[i]))
print("cases compared: 24")
print("port-vs-reference mismatches: %d" % len(bad))
for b in bad: print("  case %d port=0x%s table=0x%s ref=0x%s" % b)
print("reference instruction count line: %s" % [l for l in r.stdout.splitlines() if 'instruction count' in l][0])
print("VERDICT: %s" % ("all 24 flag cases agree (port == table == npm v86 reference)" if not bad else "MISMATCH"))
