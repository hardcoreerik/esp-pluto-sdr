#!/usr/bin/env python3
"""Receive-only probe of the iiod text protocol (reference for the C client). Reads, sets RX LO/rate, reads a few buffers."""
import socket, sys, re, time
host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.228"
s = socket.create_connection((host, 30431), timeout=10)
f = s.makefile("rb")
def line(): return f.readline().decode().rstrip("\r\n")
def cmd(c):
    s.sendall((c + "\r\n").encode())
    return int(line())
def rd(n):
    d = f.read(n); return d
print("VERSION:", (s.sendall(b"VERSION\r\n"), line())[1])
n = cmd("PRINT"); xml = rd(n).decode(); f.readline()
print("xml bytes", n)
for m in re.finditer(r'<device id="([^"]+)" name="([^"]*)"', xml): print(" dev", m.groups())
def rattr(dev, ch, attr, out=False):
    c = f"READ {dev} {'OUTPUT' if out else 'INPUT'} {ch} {attr}" if ch else f"READ {dev} {attr}"
    n = cmd(c)
    if n < 0: return n
    v = rd(n).decode(); f.readline(); return v
def wattr(dev, ch, attr, val, out=False):
    v = str(val)
    c = f"WRITE {dev} {'OUTPUT' if out else 'INPUT'} {ch} {attr} {len(v)}" if ch else f"WRITE {dev} {attr} {len(v)}"
    s.sendall((c + "\r\n").encode()); s.sendall(v.encode()); return int(line())
phy = re.search(r'<device id="([^"]+)" name="ad9361-phy"', xml).group(1)
rx = re.search(r'<device id="([^"]+)" name="cf-ad9361-lpc"', xml).group(1)
print("phy", phy, "rx", rx)
print("rx lo", rattr(phy, "altvoltage0", "frequency", True))
print("rate", rattr(phy, "voltage0", "sampling_frequency"))
print("set rate", wattr(phy, "voltage0", "sampling_frequency", 2400000))
print("set bw", wattr(phy, "voltage0", "rf_bandwidth", 2000000))
print("set lo", wattr(phy, "altvoltage0", "frequency", 162550000, True))
print("gain mode", rattr(phy, "voltage0", "gain_control_mode"), "gain", rattr(phy, "voltage0", "hardwaregain"))
print("TIMEOUT", cmd("TIMEOUT 10000"))
print("OPEN", cmd(f"OPEN {rx} 16384 00000003"))
t0 = time.time(); tot = 0
for i in range(8):
    n = cmd(f"READBUF {rx} 65536")
    if n < 0: print("readbuf err", n); break
    mask = f.readline()  # iiod 0.26: length line, then the channel mask line (8 hex + LF), then n bytes
    d = rd(n); tot += len(d)
    if i == 0: print("first readbuf", n, mask, d[:16].hex())
print("bytes", tot, "in", round(time.time() - t0, 3), "s")
print("CLOSE", cmd(f"CLOSE {rx}"))
