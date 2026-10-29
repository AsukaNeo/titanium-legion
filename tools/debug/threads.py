"""threads.py <pid> <exe path> <map file> -- every thread's RIP and the exe
return addresses on its stack, named from the link map. For our own hung
build: suspends each thread briefly, reads its context, resumes it."""
import ctypes, ctypes.wintypes as W, struct, sys, bisect, re

pid, exe, mapf = int(sys.argv[1]), sys.argv[2], sys.argv[3]
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

# Symbols from the MSVC map: "0001:00012345  name  0000000140013345 f obj"
syms = []
for line in open(mapf, errors='replace'):
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', line)
    if m:
        syms.append((int(m.group(2), 16), m.group(1)))
syms.sort()
base_pref = None
for line in open(mapf, errors='replace'):
    m = re.search(r'Preferred load address is ([0-9a-f]+)', line)
    if m: base_pref = int(m.group(1), 16); break
addrs = [s[0] for s in syms]

def name(rva):
    va = base_pref + rva
    i = bisect.bisect_right(addrs, va) - 1
    return '%s+0x%X' % (syms[i][1], va - syms[i][0]) if i >= 0 else '?'

class ME(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD),
                ('GlblcntUsage', W.DWORD), ('ProccntUsage', W.DWORD), ('modBaseAddr', ctypes.c_void_p),
                ('modBaseSize', W.DWORD), ('hModule', W.HMODULE), ('szModule', ctypes.c_char * 256),
                ('szExePath', ctypes.c_char * 260)]
class TE(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD),
                ('th32OwnerProcessID', W.DWORD), ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG), ('dwFlags', W.DWORD)]

k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.OpenThread.restype = W.HANDLE
k32.OpenProcess.restype = W.HANDLE
snap = k32.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
me = ME(); me.dwSize = ctypes.sizeof(ME)
base = size = None
ok = k32.Module32First(snap, ctypes.byref(me))
while ok:
    if me.szModule.decode().lower() == exe.lower():
        base, size = me.modBaseAddr, me.modBaseSize
    ok = k32.Module32Next(snap, ctypes.byref(me))
k32.CloseHandle(snap)
print('image base %X size %X' % (base, size))

CONTEXT_SIZE = 1232
proc = k32.OpenProcess(0x0010 | 0x0400, False, pid)
snap = k32.CreateToolhelp32Snapshot(0x4, 0)
te = TE(); te.dwSize = ctypes.sizeof(TE)
ok = k32.Thread32First(snap, ctypes.byref(te))
while ok:
    if te.th32OwnerProcessID == pid:
        h = k32.OpenThread(0x0002 | 0x0008 | 0x0040, False, te.th32ThreadID)
        buf = ctypes.create_string_buffer(CONTEXT_SIZE + 16)
        addr = (ctypes.addressof(buf) + 15) & ~15
        ctx = (ctypes.c_char * CONTEXT_SIZE).from_address(addr)
        struct.pack_into('<I', ctx, 0x30, 0x100003)   # ContextFlags: CONTROL|INTEGER
        prev = k32.SuspendThread(h)
        got = k32.GetThreadContext(h, ctypes.c_void_p(addr))
        k32.ResumeThread(h)
        rip = struct.unpack_from('<Q', ctx, 0xF8)[0]; rsp = struct.unpack_from('<Q', ctx, 0x98)[0]
        out = []
        data = b''
        if got:
            page = ctypes.create_string_buffer(0x1000); n = ctypes.c_size_t()
            a0 = rsp
            while len(data) < 0x10000:
                ln = 0x1000 - (a0 & 0xFFF)
                if not k32.ReadProcessMemory(proc, ctypes.c_void_p(a0), page, ln, ctypes.byref(n)):
                    break
                data += page.raw[:n.value]; a0 += ln
        class _N: pass
        n = _N(); n.value = len(data)
        stack = _N(); stack.raw = data
        if data:
            for i in range(0, n.value - 7, 8):
                v = struct.unpack_from('<Q', stack.raw, i)[0]
                if base <= v < base + size:
                    nm = name(v - base)
                    if nm.startswith('??_C') or nm.startswith('s_') or nm.startswith('g_') or nm.startswith('__ImageBase'):
                        continue
                    if not out or out[-1] != nm:
                        out.append(nm)
                    if len(out) >= 18: break
        where = name(rip - base) if base <= rip < base + size else 'outside exe %X' % rip
        print("tid %6d pri %2d susp %d  rip %s" % (te.th32ThreadID, te.tpBasePri, prev, where))
        for o in out: print('        ', o)
        k32.CloseHandle(h)
    ok = k32.Thread32Next(snap, ctypes.byref(te))
