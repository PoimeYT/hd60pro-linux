#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract (registry name, default, device offset) triples from an objdump
listing of CDevice::OnDeviceStart in e60MZ0380.X64.SYS."""
import re, struct, sys

def load_pe(path):
    d = open(path, 'rb').read()
    pe = struct.unpack('<I', d[0x3c:0x40])[0]
    nsec = struct.unpack('<H', d[pe+6:pe+8])[0]
    opt = struct.unpack('<H', d[pe+20:pe+22])[0]
    base = struct.unpack('<Q', d[pe+48:pe+56])[0]
    secs = []
    so = pe + 24 + opt
    for _ in range(nsec):
        vsz, va, rsz, rptr = struct.unpack('<IIII', d[so+8:so+24])
        secs.append((va, vsz, rptr))
        so += 40
    return d, base, secs

def read_str(pe, va, wide):
    d, base, secs = pe
    rva = va - base
    for sva, vsz, rptr in secs:
        if sva <= rva < sva + vsz:
            off = rptr + rva - sva
            if wide:
                e = off
                while d[e:e+2] != b'\0\0':
                    e += 2
                return d[off:e].decode('utf-16le', 'replace')
            e = d.index(b'\0', off)
            return d[off:e].decode('latin1')
    return None

def main(binary, listing):
    pe = load_pe(binary)
    lines = [l.rstrip() for l in open(listing)]
    ins = []
    for l in lines:
        m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', l)
        if m:
            ins.append((int(m.group(1), 16), m.group(2), m.group(3)))
    regs = {}
    def regval(r):
        r = r.replace('r', '').replace('d', '') if False else r
        return regs.get(r)
    canon = lambda r: {'eax':'rax','ecx':'rcx','edx':'rdx','ebx':'rbx','esi':'rsi','edi':'rdi','ebp':'rbp',
                       'r8d':'r8','r9d':'r9','r10d':'r10','r11d':'r11','r12d':'r12','r13d':'r13','r14d':'r14','r15d':'r15'}.get(r, r)
    last_name = None
    last_ansi = None
    for i, (addr, op, args) in enumerate(ins):
        m = re.match(r'(\w+),0x([0-9a-f]+)$', args)
        if op == 'mov' and m:
            regs[canon(m.group(1))] = int(m.group(2), 16)
        elif op == 'xor' and re.match(r'(\w+),\1$', args):
            regs[canon(args.split(',')[0])] = 0
        elif op == 'lea':
            m2 = re.match(r'(\w+),\[(\w+)([+-]0x[0-9a-f]+)\]$', args)
            if m2 and canon(m2.group(2)) in regs:
                regs[canon(m2.group(1))] = regs[canon(m2.group(2))] + int(m2.group(3), 16)
            m3 = re.search(r'# 0x([0-9a-f]+)', args)
            if m3 and args.startswith('rdx'):
                last_name = read_str(pe, int(m3.group(1), 16), True)
            if m3 and args.startswith('r8'):
                last_ansi = read_str(pe, int(m3.group(1), 16), False)
        if op == 'call' and args.startswith('0xbb53a'):
            last_name = (last_ansi or '?') + '<X>'
        if op == 'call' and args.startswith('0xbb570'):
            name = last_name
            default = None
            off = None
            local = dict(regs)
            for addr2, op2, args2 in ins[i+1:i+14]:
                m4 = re.match(r'(\w+),0x([0-9a-f]+)$', args2)
                if op2 == 'mov' and m4:
                    local[canon(m4.group(1))] = int(m4.group(2), 16)
                if op2 == 'xor' and re.match(r'(\w+),\1$', args2):
                    local[canon(args2.split(',')[0])] = 0
                if op2 == 'cmovne' and args2.startswith('ecx,'):
                    default = local.get(canon(args2.split(',')[1]))
                mm = re.match(r'(?:DWORD|WORD) PTR \[([^\]]*?)\+0x([0-9a-f]+)\],c?x?(?:ecx|cx)$', args2)
                if op2 == 'mov' and mm:
                    off = (mm.group(1) + '+' if mm.group(1) != 'rbx' else '') + hex(int(mm.group(2), 16))
                    break
                if op2 == 'call':
                    break
            print(f'{addr:#x}\t{name}\tdefault={default if default is None else hex(default)}\toff={off}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
