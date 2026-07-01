# 7. Lessons Learned

The debugging war-stories — the bugs that shaped the design. Read this before
touching anything memory-, audio-, or RAM-related; several "obvious"
improvements have already been tried and bitten us. The blow-by-blow is in the
archived [session log](archive/PROGRESS-sessions-de.md).

## Internal RAM is the bottleneck (boot loops)

esp-sr + Wi-Fi + TLS + task stacks all compete for ~512 KB of internal SRAM.
Two distinct boot loops came from overrunning it:

- **Three WakeNets at once.** Loading Mycroft+Alexa+Jarvis together and
  filtering by index *looked* fine (the models loaded), but the very next
  step — creating the AFE task stacks — failed (`assert ok == pdPASS`) and the
  device rebooted endlessly. Fix: keep **one** WakeNet loaded. (The panic was
  initially missed because USB re-enumerates on every reboot and swallowed the
  log.) Switching originally rebooted to reload the model; Phase B2 replaced that
  with a **runtime swap** — `destroy` then recreate the AFE with the new WakeNet
  (peak still one model), coordinated so no task is mid-AFE-call. The AFE also
  exposes only two wake-word slots, so three-at-once was never an option anyway.
- **Too-large I2S-DMA buffer.** 24 KB DMA failed to allocate at boot
  (`ESP_ERROR_CHECK` → loop).

**Takeaway:** anything that grows internal-RAM usage must be checked against the
esp-sr baseline. Big buffers belong in PSRAM.

## TLS hardware-AES starvation

Trying to smooth TTS with a **16 KB** I2S-DMA buffer booted fine but then the
TLS hardware-AES couldn't allocate at runtime (`esp-aes: Failed to allocate
memory`) — cloud uploads died. The internal heap simply has no room for a big
DMA buffer next to esp-sr/Wi-Fi/TLS.

**Takeaway:** **do not enlarge the I2S-DMA buffer.** If TTS latency/choppiness
ever needs fixing, use a PSRAM jitter buffer (a ring buffer + playback task,
DMA left at default) — see [08-status-and-roadmap.md](08-status-and-roadmap.md).

## mbedTLS didn't fit in internal SRAM

The first cloud request failed at the handshake: `mbedtls_ssl_setup -0x7F00`
(alloc failed) — the ~50 KB handshake had no room next to esp-sr. Fix:
`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` (mbedTLS allocates from PSRAM). This,
plus "never two TLS connections at once", is why the cloud pipeline is strictly
sequential.

## MultiNet phoneme / FST bugs

The English MultiNet7 model has genuinely broken pieces:

- **`MgNcTS` ("minutes") is dead in the FST** — phrases using it never reach the
  detected state. Workaround: register without the final S (`MgNcT`); the spoken
  plural-S is tolerated as junk. Side effect: "stop timer" matches as "stop"
  (the endpoint fires before "timer").
- **On-device G2P mishears "minute"** as `MgNoT` ("min-oot"); it's spoken
  `MgNcT`. So short aliases get explicit phonemes.
- **"sixty"** as one token never matches (likely FST-dead too); split as
  `SgKS Tm` plus pronunciation aliases.

These were diagnosed by logging the decoder's raw string in the detecting state.
Host G2P is `managed_components/espressif__esp-sr/tool/multinet_g2p.py` (needs a
venv with `g2p_en`+`pandas` and the NLTK `averaged_perceptron_tagger_eng`).

**Takeaway:** if a phrase never matches, log the decoder raw string first; add
explicit phonemes; don't just chase the threshold. Also: `switch_loader_mode()`
is a NULL pointer in the quantized model — calling it boot-loops.

## The vad_cache double-start

Feeding the AFE's `vad_cache` to MultiNet/recorder doubled the phrase start —
the decoder heard "SfPSfT" ("set set"), and the WAV contained "Set Set Timer".
The cache overlapped already-delivered frames rather than prepending older
audio. Fix: ignore `vad_cache` entirely. Capture starts right after the wake
word, so the onset is complete without it.

## Whisper hallucinates from silence

A near-silent capture made Whisper invent text ("Vielen Dank."), which the LLM
then politely answered. Fix: the **silence guard** — discard captures with
< 1000 ms of VAD speech. The threshold had to account for ~600 ms of phantom
"speech" the VAD reports per capture (wake-word reverb); a real short question
measures ~3700 ms, so 1000 ms separates cleanly. The naive 250 ms guess would
**not** have worked.

## The GPIO15↔GPIO6 short (the "mic reads constant max" mystery)

After one flash, the wake word stopped working — the mic read constant
full-scale. Root cause was a **wiring short between amp BCLK (GPIO15) and mic SD
(GPIO6)**, which only manifested once the amp's I2S1 started clocking GPIO15
(before that, GPIO15 was never driven, so every isolated test passed). It likely
also caused an earlier brownout/USB dropout. Diagnosed via mic RMS logging
(rms 32767), mic-solo vs. mic+amp tests, and a GPIO-toggle continuity check.

**Takeaway:** "mic reads constant max" → suspect wiring; raise the level log to
INFO and check lines with a GPIO toggle.

## SoftAP kept kicking the setup client

In APSTA mode the setup portal kept disconnecting the phone (join/leave reason
1): the ESP has one radio, so the AP had to follow the station's channel and
fought the client. Fix: run the portal in **pure AP mode** during setup; the
device reboots after saving and the station connection returns.

## Display is BGR, and shows color wrong as RGB

User bubbles came out orange instead of blue and the listening button blue
instead of red: the panel is **BGR**, not RGB. It went unnoticed at first
because grey/green survive an R/B swap nearly unchanged. Fix: BGR color space in
`display.c` (independent of the endianness byte-swap in the flush callback).

## I2S "rattle" after playback

Without `auto_clear` on the TX channel, the DMA looped the last buffer forever
after the final write. Fix: `chan_cfg.auto_clear = true`.

## Open observation: display flicker during playback

The display flickers slightly while the speaker plays. Suspected a supply-rail
dip (amp peak current pulls the rail → backlight modulates), i.e. hardware, not
software. Not yet resolved — see [08-status-and-roadmap.md](08-status-and-roadmap.md).
