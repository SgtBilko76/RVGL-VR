"""Recon helpers for the closed RVGL engine (libmain.so). Usage from execute_code:
    import sys; sys.path.insert(0, '/home/bilko/workspace/rvgl-quest/tools'); from recon import *
"""
import json
import os
import re
import subprocess

T = '/home/bilko/workspace/android-ndk-r30/toolchains/llvm/prebuilt/linux-x86_64/bin/'
LIB = '/home/bilko/workspace/rvgl-quest/src/main/jniLibs/arm64-v8a/libmain.so'
W = '/home/bilko/workspace/rvgl-quest/build/recon'
os.makedirs(W, exist_ok=True)


def _run(*a, inp=None):
    return subprocess.run(list(a), capture_output=True, text=True, input=inp).stdout


def _load():
    out = _run(T + 'llvm-readelf', '--dyn-syms', '-W', LIB)
    raw = []
    for l in out.splitlines():
        f = l.split()
        if len(f) >= 8 and f[0].endswith(':') and f[0][:-1].isdigit() and f[6] != 'UND':
            raw.append((int(f[1], 16), int(f[2], 0), f[3], f[7].split('@')[0]))
    dem = _run(T + 'llvm-cxxfilt', inp='\n'.join(r[3] for r in raw)).splitlines()
    tab = [dict(addr=a, size=s, type=t, name=n, dem=d) for (a, s, t, n), d in zip(raw, dem)]
    dpath = W + '/dis.txt'
    if not os.path.exists(dpath):
        open(dpath, 'w').write(_run(T + 'llvm-objdump', '-d', '--no-show-raw-insn', LIB))
    lines = open(dpath).read().splitlines()
    idx = {}
    for i, l in enumerate(lines):
        m = re.match(r'\s*([0-9a-f]+):\s', l)
        if m:
            idx[int(m.group(1), 16)] = i
    got = {}
    for l in _run(T + 'llvm-readelf', '-r', '-W', LIB).splitlines():
        p = l.split()
        if len(p) >= 4 and re.match(r'^[0-9a-f]{16}$', p[0]):
            if 'RELATIVE' in p[2]:
                got[int(p[0], 16)] = int(p[3], 16)
            elif len(p) >= 5:
                got[int(p[0], 16)] = p[4].split('@')[0]
    return tab, lines, idx, got


tab, lines, addr_idx, got = _load()
funcs = [x for x in tab if x['type'] == 'FUNC']
objs = [x for x in tab if x['type'] == 'OBJECT']
byaddr = {x['addr']: x['dem'] for x in funcs}
byname = {x['name']: x['dem'] for x in tab}


def sym_at(a):
    for x in tab:
        if x['addr'] <= a < x['addr'] + max(x['size'], 1):
            off = a - x['addr']
            return x['dem'] + (f'+{off:#x}' if off else '')
    return '?'


def fn(name):
    return [x for x in funcs if x['dem'].split('(')[0] == name][0]


def rng(a, b):
    return [lines[addr_idx[x]] for x in range(a, b, 4) if x in addr_idx]


def annotate(bl):
    """Add '; &symbol' to adrp+ldr GOT loads."""
    out, pages = [], {}
    for l in bl:
        m = re.match(r'\s*([0-9a-f]+):\s+(\w+)\s+(.*)', l)
        if not m:
            out.append(l)
            continue
        _, op, args = m.groups()
        mm = re.match(r'(x\d+), 0x([0-9a-f]+)', args)
        if op == 'adrp' and mm:
            pages[mm.group(1)] = int(mm.group(2), 16)
        mm = re.match(r'(x\d+), \[(x\d+), #0x([0-9a-f]+)\]', args)
        note = ''
        if op == 'ldr' and mm and mm.group(2) in pages:
            t = got.get(pages[mm.group(2)] + int(mm.group(3), 16))
            if isinstance(t, int):
                note = '   ; &' + sym_at(t)
            elif t:
                note = '   ; &' + byname.get(t, t)
        out.append(l + note)
    return out


def body(name):
    f = fn(name)
    return f, annotate(rng(f['addr'], f['addr'] + f['size']))


def calls(name):
    f, b = body(name)
    res = []
    for l in b:
        m = re.search(r'\bbl\s+0x([0-9a-f]+)', l)
        if m:
            t = int(m.group(1), 16)
            mm = re.search(r'<([^>]+)>', l)
            res.append((l.split(':')[0].strip(), byaddr.get(t, mm.group(1) if mm else hex(t))))
    return f, res


def callers(name):
    f = fn(name)
    pat = re.compile(r'\bbl?\s+0x%x\b' % f['addr'])
    return sorted({sym_at(int(l.split(':')[0], 16)) .split('+')[0] for l in lines if pat.search(l)})
