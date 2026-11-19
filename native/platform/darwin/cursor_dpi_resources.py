"""Small real Windows resources for cursor and bitmap scaling integration tests."""
import pathlib
import struct
import sys

out = pathlib.Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
# A 2x2 monochrome cursor: top-left white, bottom-left transparent, hot spot (1,1).
header = struct.pack('<IiiHHIIiiII', 40, 2, 4, 1, 1, 0, 16, 0, 0, 0, 0)
payload = header + bytes(4) + bytes([255, 255, 255, 0]) + bytes([0,0,0,0,128,0,0,0,128,0,0,0,0,0,0,0])
(out / 'cursor.cur').write_bytes(struct.pack('<HHHBBBBHHII', 0, 2, 1, 2, 2, 2, 0, 1, 1, len(payload), 22) + payload)
header = struct.pack('<IiiHHIIiiII', 40, 2, 2, 1, 32, 0, 16, 0, 0, 0, 0)
(out / 'bitmap.bmp').write_bytes(struct.pack('<2sIHHI', b'BM', 70, 0, 0, 54) + header + bytes([0,0,255,255] * 4))
(out / 'per-monitor.manifest').write_text('<assembly xmlns="urn:schemas-microsoft-com:asm.v1"><application><windowsSettings>'
    '<dpiAware>true/pm</dpiAware><dpiAwareness>PerMonitorV2, PerMonitor</dpiAwareness>'
    '</windowsSettings></application></assembly>')
(out / 'resources.rc').write_text(f'111 CURSOR "{out / "cursor.cur"}"\n112 BITMAP "{out / "bitmap.bmp"}"\n'
    f'#ifdef PER_MONITOR_MANIFEST\n1 24 "{out / "per-monitor.manifest"}"\n#endif\n')
