/*
 * The single translation unit that compiles dr_flac itself. Everything else
 * includes the header for declarations only.
 *
 * Why each define matters on this target:
 *   NO_STDIO — there is no fopen(); all file access goes through FatFs, which
 *              the decoder reaches via our io callbacks.
 *   NO_OGG   — Ogg-encapsulated FLAC is not something a local music library
 *              contains, and dropping it saves flash.
 *   NO_SIMD  — the x86/ARM NEON paths do not apply to Cortex-M7; leaving this
 *              on invites the compiler down code paths that cannot build.
 *   NO_WCHAR — no wide-char filenames without stdio.
 *
 * CRC stays ENABLED. It is the only thing that will tell you a bad SD read
 * corrupted a frame rather than the decoder being wrong, and on a device
 * whose storage is a card in a socket that matters.
 */
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_IMPLEMENTATION
#include "../../third_party/dr_flac.h"
