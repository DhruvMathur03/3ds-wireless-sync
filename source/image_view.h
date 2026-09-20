#pragma once
#include <citro2d.h>
#include <stdbool.h>

/* Decodes the JPEG at `path`, uploads it as a GPU texture sized to fit
 * the bottom screen. Returns false (and leaves nothing open) on any
 * failure - bad path, unsupported/corrupt file, image too large, or
 * out of memory. Closes any previously-open image first. */
bool iv_open(const char *path);

/* Frees the GPU texture. Safe to call even if nothing is open. */
void iv_close(void);

bool iv_is_open(void);

/* Call once per frame, between C2D_SceneBegin(bottomTarget) and
 * C3D_FrameEnd(), instead of your normal bottom-screen draw. `textBuf`
 * is a C2D_TextBuf you already own (this just borrows it for the two
 * text labels); `filename` is shown above the "press B to close" hint. */
void iv_draw(C2D_TextBuf textBuf, const char *filename, bool canSend);
