/* Optional host-only text-to-speech adapter (TTS_TOOLS=1). */
#ifndef MOTIRIS_TTS_H
#define MOTIRIS_TTS_H

#include <stddef.h>

#define MOTIRIS_TTS_TEXT_CAP 16384u

int motiris_tts_enabled(void);
/* 0 = backend accepted all text, 1 = accepted a bounded prefix,
 * -1 = text-only fallback; diag is always NUL-terminated when supplied. */
int motiris_tts_speak(const char *text, char *diag, size_t diag_size);

#endif
