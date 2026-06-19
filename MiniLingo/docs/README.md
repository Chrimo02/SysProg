# MiniLingo — Developer Documentation

MiniLingo is a self-contained **voice assistant** running on a single
ESP32-S3. You say a wake word ("Mycroft", "Alexa", or "Jarvis"), ask a
question or give a command, and it answers — either entirely on-device
(kitchen timer, volume) or by reaching out to the cloud for speech-to-text,
a language-model answer, and optionally spoken audio back.

It was built incrementally over ~13 development sessions as an elective
project. This `docs/` set is the **definitive, English explanation** of what
was built, **why** it was built that way, and **how the pieces fit together**.
It supersedes the older German documents, which are preserved verbatim in
[archive/](archive/) for historical reference.

## Who this is for

A student or developer who is **new to the codebase** and possibly new to
embedded voice assistants. You do not need prior ESP-IDF experience; terms are
explained in the [glossary](09-glossary.md). You do need a rough idea of C and
of how a microcontroller program is structured.

## Reading order

Read these three first — they give you the whole mental model:

1. **[01-overview.md](01-overview.md)** — what MiniLingo is, the hardware, the
   tech stack, and the feature list.
2. **[02-architecture.md](02-architecture.md)** — the hybrid local/cloud
   design, the FreeRTOS task model, how the tasks talk to each other, the
   memory budget, and the flash partition layout.
3. **[03-request-lifecycle.md](03-request-lifecycle.md)** — a single end-to-end
   walkthrough: from wake word, through capture and routing, to a spoken
   answer and back to idle. This is the spine that ties every subsystem
   together.

Then dive into whatever you need:

- **[04-subsystems/](04-subsystems/)** — one guide per functional area:
  [drivers](04-subsystems/drivers.md),
  [speech](04-subsystems/speech.md),
  [network](04-subsystems/network.md),
  [local features](04-subsystems/local.md),
  [UI](04-subsystems/ui.md),
  [config & provisioning](04-subsystems/config.md).
- **[05-build-and-setup.md](05-build-and-setup.md)** — toolchain, building,
  flashing, Wi-Fi/API-key provisioning, and regenerating the canned audio.
- **[06-design-decisions.md](06-design-decisions.md)** — the *why* behind the
  non-obvious choices, each with its rationale.
- **[07-lessons-learned.md](07-lessons-learned.md)** — the debugging
  war-stories (RAM crunches, phoneme bugs, a wiring short, TLS starvation).
  Read this before changing anything memory- or audio-related.
- **[08-status-and-roadmap.md](08-status-and-roadmap.md)** — what's done, what's
  only been tested on the bench vs. on hardware, and what's left.
- **[09-glossary.md](09-glossary.md)** — every acronym used here.

## Conventions

- File references link to the actual source, e.g.
  [main.c](../main/main.c). Paths are relative to this `docs/` folder.
- "Phase N" refers to the development phases from the original plan; the
  history lives in [archive/PROGRESS-sessions-de.md](archive/PROGRESS-sessions-de.md).
- Code comments in the repo are in German; this documentation is in English.
  Identifiers and log strings are quoted as-is.

## Keeping docs current

When you finish a piece of work, update **[08-status-and-roadmap.md](08-status-and-roadmap.md)**
(the status table + open items) and, if you made a non-obvious choice, add an
entry to [06-design-decisions.md](06-design-decisions.md). The old habit of
appending a session note to `PROGRESS.md` is retired — that file is now an
archive.
