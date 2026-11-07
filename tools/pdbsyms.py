#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal PDB (MSF7) symbol extractor: public + procedure symbols -> RVA list."""
import struct, sys, json

def msf_streams(d):
    bs, fpm, nblocks, dirsize, _, bmap = struct.unpack('<IIIIII', d[32:56])
    ndb = (dirsize + bs - 1) // bs
    dirblk = struct.unpack('<%dI' % ndb, d[bmap*bs:bmap*bs+4*ndb])
    dd = b''.join(d[b*bs:(b+1)*bs] for b in dirblk)[:dirsize]
    ns = struct.unpack('<I', dd[:4])[0]
    sizes = struct.unpack('<%dI' % ns, dd[4:4+4*ns])
    off = 4 + 4*ns
    out = []
    for s in sizes:
        if s == 0xffffffff:
            out.append(b''); continue
        n = (s + bs - 1)//bs
        blks = struct.unpack('<%dI' % n, dd[off:off+4*n]); off += 4*n
        out.append(b''.join(d[b*bs:(b+1)*bs] for b in blks)[:s])
    return out

def pe_sections(p):
    d = open(p, 'rb').read()
    pe = struct.unpack('<I', d[0x3c:0x40])[0]
    nsec = struct.unpack('<H', d[pe+6:pe+8])[0]
    opt = struct.unpack('<H', d[pe+20:pe+22])[0]
    base = struct.unpack('<Q', d[pe+24+24:pe+24+32])[0]
    secs = []
    so = pe + 24 + opt
    for i in range(nsec):
        name = d[so:so+8].rstrip(b'\0').decode()
        vsz, va = struct.unpack('<II', d[so+8:so+16])
        secs.append((name, va, vsz)); so += 40
    return base, secs

def cstr(b, o):
    e = b.index(b'\0', o)
    return b[o:e].decode('latin1')

def walk(buf, start, end, cb):
    o = start
    while o + 4 <= end:
        reclen, kind = struct.unpack('<HH', buf[o:o+4])
        if reclen < 2: break
        cb(kind, buf[o+4:o+2+reclen])
        o += 2 + reclen

def main(pdb, pe):
    d = open(pdb, 'rb').read()
    st = msf_streams(d)
    base, secs = pe_sections(pe)
    dbi = st[3]
    (sig, ver, age, gsi, bld, psi, dllver, symrec, rbld, modsz, sczsz, smsz,
     srcsz, tsmsz, mfc, dbgsz, ecsz, flags, mach) = struct.unpack('<iIIHHHHHHiiiiiIiiHH', dbi[:60])
    syms = {}
    def add(kind, seg, off, name):
        if 1 <= seg <= len(secs):
            rva = secs[seg-1][1] + off
            syms.setdefault(rva, (kind, name))
    def cb_pub(kind, r):
        if kind == 0x110E:
            fl, off, seg = struct.unpack('<IIH', r[:10]); add('pub' if not fl & 2 else 'pubfn', seg, off, cstr(r, 10))
    walk(st[symrec], 0, len(st[symrec]), cb_pub)
    # module streams for local procs
    o = 64; mend = 64 + modsz
    while o < mend:
        modsn = struct.unpack('<H', dbi[o+34:o+36])[0]
        symbytes = struct.unpack('<I', dbi[o+36:o+40])[0]
        o2 = o + 64
        mname = cstr(dbi, o2); o2 += len(mname) + 1
        oname = cstr(dbi, o2); o2 += len(oname) + 1
        o = (o2 + 3) & ~3
        if modsn == 0xffff or modsn >= len(st): continue
        ms = st[modsn]
        def cb_mod(kind, r):
            if kind in (0x110F, 0x1110, 0x1147, 0x1148):  # LPROC32, GPROC32, *_ID
                off, seg = struct.unpack('<IH', r[28:34]); add('proc', seg, off, cstr(r, 35))
            elif kind in (0x110C, 0x110D):  # LDATA32 / GDATA32
                ti, off, seg = struct.unpack('<IIH', r[:10]); add('data', seg, off, cstr(r, 10))
        walk(ms, 4, min(symbytes, len(ms)), cb_mod)
    rows = sorted((rva, k, n) for rva, (k, n) in syms.items())
    json.dump({'image_base': base, 'symbols': [{'rva': r, 'va': base + r, 'kind': k, 'name': n} for r, k, n in rows]},
              sys.stdout, indent=0)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
