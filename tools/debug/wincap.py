"""wincap.py <title substring> <out.png> -- capture a window's client area
with PrintWindow(PW_RENDERFULLCONTENT), which includes D3D content."""
import ctypes, ctypes.wintypes as W, struct, zlib, sys

u32, g32 = ctypes.WinDLL('user32'), ctypes.WinDLL('gdi32')
u32.SetProcessDPIAware()
want, out = sys.argv[1].lower(), sys.argv[2]
found = []

@ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
def enum(h, _):
    n = u32.GetWindowTextLengthW(h)
    if n:
        b = ctypes.create_unicode_buffer(n + 1)
        u32.GetWindowTextW(h, b, n + 1)
        if want in b.value.lower() and u32.IsWindowVisible(h):
            found.append(h)
    return True
u32.EnumWindows(enum, 0)
if not found:
    sys.exit('no window')
h = found[0]
if u32.IsIconic(h):
    u32.ShowWindow(h, 4)           # SW_SHOWNOACTIVATE: restore, keep focus where it is
    import time; time.sleep(1.5)
r = W.RECT(); u32.GetClientRect(h, ctypes.byref(r))
w, hh = r.right, r.bottom
hdc = u32.GetDC(h); mdc = g32.CreateCompatibleDC(hdc)
bmp = g32.CreateCompatibleBitmap(hdc, w, hh); g32.SelectObject(mdc, bmp)
u32.PrintWindow(h, mdc, 3)          # PW_CLIENTONLY | PW_RENDERFULLCONTENT

class BIH(ctypes.Structure):
    _fields_ = [('biSize', W.DWORD), ('biWidth', W.LONG), ('biHeight', W.LONG),
                ('biPlanes', W.WORD), ('biBitCount', W.WORD), ('biCompression', W.DWORD),
                ('biSizeImage', W.DWORD), ('a', W.LONG), ('b', W.LONG), ('c', W.DWORD), ('d', W.DWORD)]
bi = BIH(); bi.biSize = ctypes.sizeof(BIH); bi.biWidth = w; bi.biHeight = -hh
bi.biPlanes = 1; bi.biBitCount = 32
buf = ctypes.create_string_buffer(w * hh * 4)
g32.GetDIBits(mdc, bmp, 0, hh, buf, ctypes.byref(bi), 0)
raw = bytearray()
for y in range(hh):
    raw.append(0)
    row = buf.raw[y * w * 4:(y + 1) * w * 4]
    for x in range(0, len(row), 4):
        raw += bytes((row[x + 2], row[x + 1], row[x]))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, hh, 8, 2, 0, 0, 0))
                      + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
print(out, w, hh)
