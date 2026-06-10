#!/usr/bin/env python3
"""Extrahiert den Phase-3-WAV-Dump aus einem idf.py-monitor-Log.

Das Firmware-Feature CONFIG_RECORDING_WAV_DUMP gibt nach jedem Capture
den WAV-Buffer Base64-codiert zwischen den Markern WAV_DUMP_BEGIN und
WAV_DUMP_END aus. Log mitschneiden, z. B.:

    idf.py monitor | tee monitor.log     # oder in monitor: Ctrl-T Ctrl-L

Dann:

    python3 tools/extract_wav.py monitor.log [recording.wav]

Bei mehreren Dumps im Log wird der letzte genommen.
"""
import base64
import re
import sys

B64_LINE = re.compile(r"^[A-Za-z0-9+/]+={0,2}$")
BEGIN = re.compile(r"WAV_DUMP_BEGIN bytes=(\d+)")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    log_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "recording.wav"

    expected = None
    chunks = []
    in_dump = False
    with open(log_path, errors="replace") as f:
        for raw in f:
            line = raw.strip()
            m = BEGIN.search(line)
            if m:
                # letzter Dump im Log gewinnt
                expected = int(m.group(1))
                chunks = []
                in_dump = True
                continue
            if "WAV_DUMP_END" in line:
                in_dump = False
                continue
            if in_dump and B64_LINE.match(line):
                chunks.append(line)

    if expected is None:
        sys.exit(f"{log_path}: kein WAV_DUMP_BEGIN gefunden")

    data = base64.b64decode("".join(chunks))
    if len(data) != expected:
        sys.exit(
            f"Dump unvollstaendig: {len(data)} von {expected} Bytes "
            "(Monitor-Log abgeschnitten?)"
        )
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit("Decodierte Daten sind kein WAV (RIFF-Magic fehlt)")

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"{out_path}: {len(data)} Bytes geschrieben")


if __name__ == "__main__":
    main()
