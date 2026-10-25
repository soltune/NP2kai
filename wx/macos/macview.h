/* === macOS native screen output for wx port ===
 * Frames are copied into IOSurfaces and shown as the contents of a
 * CALayer on top of the panel's NSView, so scaling and rotation run
 * on the GPU in the window server instead of wxBitmap + StretchBlit.
 */

#ifndef NP2_WX_MACVIEW_H
#define NP2_WX_MACVIEW_H

#include <compiler.h>

typedef struct _macview MACVIEW;

/* nsview: the NSView* of the screen panel (wxWindow::GetHandle()) */
MACVIEW *macview_create(void *nsview);
void     macview_destroy(MACVIEW *mv);

/* src: emulator surface, 16bpp RGB565 or 32bpp BGRX.
 * rot: SCRNMODE_ROTATEMASK bits of scrnmode. */
void     macview_present(MACVIEW *mv, const UINT8 *src, int width, int height,
                         int bpp, UINT8 rot);
/* call after the panel is resized */
void     macview_layout(MACVIEW *mv);

/* native fullscreen of the window that holds nsview.
 * cb(ctx, TRUE) runs when entering starts, cb(ctx, FALSE) after leaving,
 * whether triggered here, by the green button or by Esc. */
typedef void (*MACVIEW_FSCB)(void *ctx, BOOL fullscreen);
void     macview_watchfullscreen(void *nsview, MACVIEW_FSCB cb, void *ctx);
void     macview_unwatchfullscreen(void);
BOOL     macview_isfullscreen(void *nsview);
void     macview_setfullscreen(void *nsview, BOOL fullscreen);

/* mouse capture: detach the cursor from mouse movement and read
 * the raw deltas, instead of warping the pointer back every event */
void     macview_setmouserelative(BOOL relative);
void     macview_getmousedelta(int *dx, int *dy);

/* bring the process to the front (needed when not run as an .app) */
void     macview_activateapp(void);

#endif /* NP2_WX_MACVIEW_H */
