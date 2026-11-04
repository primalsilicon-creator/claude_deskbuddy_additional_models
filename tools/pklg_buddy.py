#!/usr/bin/env python3
"""Extract Claude Buddy NUS traffic from an Apple PacketLogger (.pklg) capture.

Usage: python3 tools/pklg_buddy.py capture.pklg [--raw]

Reassembles ACL fragments, keeps ATT Write Command / Write Request (desktop ->
board) and Handle Value Notification (board -> desktop), splits each
connection+handle stream on '\n', and prints one JSON line per message with a
relative timestamp. Thinking-block signatures are shortened unless --raw.
Connections that never carry a line starting with '{' (mice, keyboards,
watches) are skipped.
"""
import json, struct, sys
from collections import defaultdict

def records(data):
    i = 0
    while i + 13 <= len(data):
        n, = struct.unpack_from('<I', data, i)
        ts, = struct.unpack_from('<Q', data, i + 4)
        secs, usecs = ts & 0xffffffff, ts >> 32
        yield secs + usecs / 1e6, data[i + 12], data[i + 13:i + 4 + n]
        i += 4 + n

def att_pdus(data):
    acl = {}
    for t, kind, p in records(data):
        if kind not in (2, 3) or len(p) < 4:   # 2 = ACL sent, 3 = ACL received
            continue
        h, ln = struct.unpack_from('<HH', p)
        pb, conn = (h >> 12) & 3, h & 0xfff
        key = (conn, kind)
        if pb in (0, 2):
            acl[key] = bytearray()
        b = acl.setdefault(key, bytearray())
        b.extend(p[4:4 + ln])
        if len(b) >= 4:
            l2, cid = struct.unpack_from('<HH', b)
            if len(b) >= 4 + l2:
                if cid == 4:
                    yield t, 'desktop->board' if kind == 2 else 'board->desktop', conn, bytes(b[4:4 + l2])
                acl[key] = bytearray()

def main():
    raw = '--raw' in sys.argv
    data = open([a for a in sys.argv[1:] if a != '--raw'][0], 'rb').read()
    streams, out = defaultdict(bytearray), []
    for t, d, conn, pdu in att_pdus(data):
        if pdu[0] not in (0x12, 0x52, 0x1b) or len(pdu) < 3:
            continue
        handle, = struct.unpack_from('<H', pdu, 1)
        b = streams[(conn, handle, d)]
        b.extend(pdu[3:])
        while b'\n' in b:
            k = b.index(b'\n')
            out.append((t, d, conn, handle, bytes(b[:k])))
            del b[:k + 1]
    out = [o for o in out if o[4].startswith(b'{')]
    if not out:
        print('no JSON lines found'); return
    t0 = out[0][0]
    for t, d, conn, handle, line in out:
        try:
            j = json.loads(line)
            if not raw and isinstance(j.get('content'), list):
                for blk in j['content']:
                    if isinstance(blk, dict) and 'signature' in blk:
                        blk['signature'] = blk['signature'][:12] + '...'
            s = json.dumps(j, ensure_ascii=False)
        except ValueError:
            s = 'UNPARSEABLE ' + line.decode('utf-8', 'replace')
        print(f'{t - t0:8.2f}  {d}  conn=0x{conn:03x} h={handle}  {s}')

if __name__ == '__main__':
    main()
