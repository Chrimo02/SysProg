#!/usr/bin/env python3
"""Generiert die Canned-TTS-WAVs fuer die lokalen Commands (Phase 4,
Spec Section 7 "Pre-Generated Audio-Responses") nach spiffs/.

Engines:
  gemini  Gemini-TTS-API (Spec 9.5), braucht GEMINI_API_KEY (env oder
          --api-key). Output: 24 kHz PCM -> WAV.
  say     macOS-Sprachsynthese (offline-Fallback solange kein API-Key
          da ist): say -> AIFF, afconvert -> 16 kHz/16 bit/mono WAV.
  auto    gemini falls API-Key gesetzt, sonst say (Default).

Das Alarm-File (tts_alarm.wav) wird immer synthetisiert (Beep-Pattern,
kein TTS). Aufruf aus dem Projekt-Root:

    python3 scripts/generate_canned_tts.py [--engine auto|gemini|say]
"""

import argparse
import base64
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import urllib.request
import wave
from pathlib import Path

# filename -> gesprochener Text. Die Dateinamen sind die API zwischen
# Script und Firmware (main/local/*.c) — bei Aenderungen beide Seiten
# anpassen.
PHRASES = {
    "tts_timer_set_30s.wav":   "Timer set: thirty seconds.",
    "tts_timer_set_60s.wav":   "Timer set: one minute.",
    "tts_timer_set_300s.wav":  "Timer set: five minutes.",
    "tts_timer_set_600s.wav":  "Timer set: ten minutes.",
    "tts_timer_set_900s.wav":  "Timer set: fifteen minutes.",
    "tts_timer_set_1800s.wav": "Timer set: thirty minutes.",
    "tts_timer_cancelled.wav": "Timer cancelled.",
    "tts_timer_done.wav":      "Your timer is done.",
    "tts_no_timer.wav":        "There is no timer running.",
    "tts_time_left.wav":       "The remaining time is on the display.",
    "tts_volume_changed.wav":  "Okay.",
    "tts_muted.wav":           "Muted.",
    "tts_unmuted.wav":         "Unmuted.",
}

GEMINI_URL = ("https://generativelanguage.googleapis.com/v1beta/models/"
              "gemini-2.5-flash-preview-tts:generateContent")
SAY_RATE = 16000  # 16 kHz reicht fuer Sprache und spart SPIFFS-Platz


def write_wav(path: Path, pcm: bytes, rate: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def gen_gemini(text: str, out: Path, api_key: str, voice: str) -> None:
    body = {
        "contents": [{"parts": [{"text": text}]}],
        "generationConfig": {
            "responseModalities": ["AUDIO"],
            "speechConfig": {
                "voiceConfig": {"prebuiltVoiceConfig": {"voiceName": voice}}
            },
        },
    }
    req = urllib.request.Request(
        GEMINI_URL,
        data=json.dumps(body).encode(),
        headers={"x-goog-api-key": api_key,
                 "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        data = json.load(resp)
    part = data["candidates"][0]["content"]["parts"][0]["inlineData"]
    mime = part["mimeType"]  # z.B. audio/L16;codec=pcm;rate=24000
    rate = int(mime.rsplit("rate=", 1)[1]) if "rate=" in mime else 24000
    write_wav(out, base64.b64decode(part["data"]), rate)


def gen_say(text: str, out: Path, voice: str) -> None:
    with tempfile.NamedTemporaryFile(suffix=".aiff", delete=False) as tmp:
        aiff = Path(tmp.name)
    try:
        cmd = ["say", "-o", str(aiff)]
        if voice:
            cmd += ["-v", voice]
        subprocess.run(cmd + [text], check=True)
        subprocess.run(["afconvert", "-f", "WAVE",
                        "-d", f"LEI16@{SAY_RATE}", "-c", "1",
                        str(aiff), str(out)], check=True)
    finally:
        aiff.unlink(missing_ok=True)


def gen_alarm(out: Path) -> None:
    """3x (3 Beeps 880 Hz, 120 ms) mit Pausen — klassischer Kuechenwecker."""
    rate = SAY_RATE
    pcm = bytearray()

    def tone(freq, ms, amp=0.6):
        n = rate * ms // 1000
        for i in range(n):
            # 10-ms-Fade an beiden Enden gegen Klicks
            env = min(1.0, i / (rate * 0.01), (n - 1 - i) / (rate * 0.01))
            v = int(32767 * amp * env * math.sin(2 * math.pi * freq * i / rate))
            pcm.extend(struct.pack("<h", v))

    def silence(ms):
        pcm.extend(b"\x00\x00" * (rate * ms // 1000))

    for burst in range(3):
        for _ in range(3):
            tone(880, 120)
            silence(80)
        if burst < 2:
            silence(400)
    write_wav(out, bytes(pcm), rate)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--engine", choices=["auto", "gemini", "say"],
                    default="auto")
    ap.add_argument("--api-key", default=os.environ.get("GEMINI_API_KEY", ""))
    ap.add_argument("--voice", default="",
                    help="Gemini: Kore/Puck/... (Default Kore); "
                         "say: macOS-Voice (Default System-Voice)")
    ap.add_argument("--out-dir",
                    default=Path(__file__).resolve().parent.parent / "spiffs")
    args = ap.parse_args()

    engine = args.engine
    if engine == "auto":
        engine = "gemini" if args.api_key else "say"
    if engine == "gemini" and not args.api_key:
        ap.error("--engine gemini braucht GEMINI_API_KEY/--api-key")
    if engine == "say" and sys.platform != "darwin":
        ap.error("--engine say gibt es nur auf macOS")

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    total = 0
    for name, text in PHRASES.items():
        out = out_dir / name
        if engine == "gemini":
            gen_gemini(text, out, args.api_key, args.voice or "Kore")
        else:
            gen_say(text, out, args.voice)
        total += out.stat().st_size
        print(f"  {name:28s} {out.stat().st_size:7d} B  \"{text}\" [{engine}]")

    alarm = out_dir / "tts_alarm.wav"
    gen_alarm(alarm)
    total += alarm.stat().st_size
    print(f"  {'tts_alarm.wav':28s} {alarm.stat().st_size:7d} B  (Beeps)")
    print(f"Summe: {total / 1024:.0f} KB -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
