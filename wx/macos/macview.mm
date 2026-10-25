/* === macOS native screen output for wx port === */

#include "macview.h"
#include <scrnmng.h>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurface.h>

/* Triple buffering: the window server may still be compositing the
 * surface handed over for the previous frame. */
#define MACVIEW_NSURFACE	3

struct _macview {
	NSView		*view;
	CALayer		*layer;
	IOSurfaceRef surf[MACVIEW_NSURFACE];
	int			width;
	int			height;
	int			next;
	UINT8		rot;
};

static IOSurfaceRef createsurface(int width, int height)
{
	NSDictionary *props = @{
		(id)kIOSurfaceWidth:           @(width),
		(id)kIOSurfaceHeight:          @(height),
		(id)kIOSurfaceBytesPerElement: @4,
		(id)kIOSurfacePixelFormat:     @((UInt32)'BGRA'),
	};
	return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

static void releasesurfaces(MACVIEW *mv)
{
	for (int i = 0; i < MACVIEW_NSURFACE; i++) {
		if (mv->surf[i]) {
			CFRelease(mv->surf[i]);
			mv->surf[i] = NULL;
		}
	}
	mv->width = 0;
	mv->height = 0;
}

MACVIEW *macview_create(void *nsview)
{
	MACVIEW *mv = (MACVIEW *)calloc(1, sizeof(MACVIEW));
	if (!mv) {
		return NULL;
	}
	mv->view = (__bridge NSView *)nsview;
	mv->view.wantsLayer = YES;
	mv->view.layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);

	CALayer *layer = [CALayer layer];
	/* no implicit animations: every contents/geometry change is a new frame */
	layer.actions = @{
		@"contents":  [NSNull null],
		@"bounds":    [NSNull null],
		@"position":  [NSNull null],
		@"transform": [NSNull null],
	};
	layer.opaque = YES;
	layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
	/* keep the 16:10 shape; letterbox when the panel has another shape */
	layer.contentsGravity = kCAGravityResizeAspect;
	layer.magnificationFilter = kCAFilterNearest;
	layer.minificationFilter = kCAFilterLinear;
	[mv->view.layer addSublayer:layer];
	mv->layer = [layer retain];

	macview_layout(mv);
	return mv;
}

void macview_destroy(MACVIEW *mv)
{
	if (!mv) {
		return;
	}
	[mv->layer removeFromSuperlayer];
	[mv->layer release];
	releasesurfaces(mv);
	free(mv);
}

void macview_layout(MACVIEW *mv)
{
	if (!mv) {
		return;
	}
	NSRect b = mv->view.bounds;
	CGFloat w = b.size.width;
	CGFloat h = b.size.height;
	BOOL quarter = (mv->rot == SCRNMODE_ROTATELEFT) || (mv->rot == SCRNMODE_ROTATERIGHT);
	/* the wx NSView is flipped, so a positive angle turns clockwise */
	CGFloat angle = 0;
	if (mv->rot == SCRNMODE_ROTATELEFT) {
		angle = -M_PI_2;
	} else if (mv->rot == SCRNMODE_ROTATERIGHT) {
		angle = M_PI_2;
	} else if (mv->rot == SCRNMODE_ROTATEDIR) {
		angle = M_PI;
	}

	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	/* rotated by a quarter turn, the unrotated layer is h x w */
	mv->layer.bounds = CGRectMake(0, 0, quarter ? h : w, quarter ? w : h);
	mv->layer.position = CGPointMake(w / 2, h / 2);
	mv->layer.affineTransform = CGAffineTransformMakeRotation(angle);
	mv->layer.contentsScale = mv->view.window ? mv->view.window.backingScaleFactor : 1.0;
	[CATransaction commit];
}

static void copy16(UINT8 *dst, size_t dstpitch, const UINT8 *src, int width, int height)
{
	for (int y = 0; y < height; y++) {
		const UINT16 *s = (const UINT16 *)(src + (size_t)y * width * 2);
		UINT32 *d = (UINT32 *)(dst + (size_t)y * dstpitch);
		for (int x = 0; x < width; x++) {
			UINT32 p = s[x];
			UINT32 r = (p >> 11) & 0x1f;
			UINT32 g = (p >> 5) & 0x3f;
			UINT32 b = p & 0x1f;
			r = (r << 3) | (r >> 2);
			g = (g << 2) | (g >> 4);
			b = (b << 3) | (b >> 2);
			d[x] = 0xff000000 | (r << 16) | (g << 8) | b;
		}
	}
}

static void copy32(UINT8 *dst, size_t dstpitch, const UINT8 *src, int width, int height)
{
	/* the emulator leaves the 4th byte undefined; force it opaque */
	for (int y = 0; y < height; y++) {
		const UINT32 *s = (const UINT32 *)(src + (size_t)y * width * 4);
		UINT32 *d = (UINT32 *)(dst + (size_t)y * dstpitch);
		for (int x = 0; x < width; x++) {
			d[x] = s[x] | 0xff000000;
		}
	}
}

void macview_present(MACVIEW *mv, const UINT8 *src, int width, int height,
                     int bpp, UINT8 rot)
{
	if (!mv || !src || width <= 0 || height <= 0) {
		return;
	}
	if (mv->width != width || mv->height != height) {
		releasesurfaces(mv);
		for (int i = 0; i < MACVIEW_NSURFACE; i++) {
			mv->surf[i] = createsurface(width, height);
			if (!mv->surf[i]) {
				releasesurfaces(mv);
				return;
			}
		}
		mv->width = width;
		mv->height = height;
	}
	if (mv->rot != rot) {
		mv->rot = rot;
		macview_layout(mv);
	}

	/* pick a surface the window server is not reading */
	IOSurfaceRef surf = NULL;
	for (int i = 0; i < MACVIEW_NSURFACE; i++) {
		int idx = (mv->next + i) % MACVIEW_NSURFACE;
		if (!IOSurfaceIsInUse(mv->surf[idx])) {
			surf = mv->surf[idx];
			mv->next = (idx + 1) % MACVIEW_NSURFACE;
			break;
		}
	}
	if (!surf) {
		/* all busy: drop this frame rather than tear */
		return;
	}

	IOSurfaceLock(surf, 0, NULL);
	UINT8 *dst = (UINT8 *)IOSurfaceGetBaseAddress(surf);
	size_t pitch = IOSurfaceGetBytesPerRow(surf);
	if (bpp == 16) {
		copy16(dst, pitch, src, width, height);
	} else {
		copy32(dst, pitch, src, width, height);
	}
	IOSurfaceUnlock(surf, 0, NULL);

	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	mv->layer.contents = (__bridge id)surf;
	[CATransaction commit];
}

static id s_fsobserver[2];

void macview_watchfullscreen(void *nsview, MACVIEW_FSCB cb, void *ctx)
{
	NSWindow *win = ((__bridge NSView *)nsview).window;
	if (!win) {
		return;
	}
	win.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
	macview_unwatchfullscreen();
	NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
	s_fsobserver[0] = [[nc addObserverForName:NSWindowWillEnterFullScreenNotification
	                                   object:win queue:nil
	                               usingBlock:^(NSNotification *) { cb(ctx, TRUE); }] retain];
	s_fsobserver[1] = [[nc addObserverForName:NSWindowDidExitFullScreenNotification
	                                   object:win queue:nil
	                               usingBlock:^(NSNotification *) { cb(ctx, FALSE); }] retain];
}

void macview_unwatchfullscreen(void)
{
	for (int i = 0; i < 2; i++) {
		if (s_fsobserver[i]) {
			[[NSNotificationCenter defaultCenter] removeObserver:s_fsobserver[i]];
			[s_fsobserver[i] release];
			s_fsobserver[i] = nil;
		}
	}
}

BOOL macview_isfullscreen(void *nsview)
{
	NSWindow *win = ((__bridge NSView *)nsview).window;
	return (win && (win.styleMask & NSWindowStyleMaskFullScreen)) ? TRUE : FALSE;
}

void macview_setfullscreen(void *nsview, BOOL fullscreen)
{
	NSWindow *win = ((__bridge NSView *)nsview).window;
	if (win && macview_isfullscreen(nsview) != fullscreen) {
		[win toggleFullScreen:nil];
	}
}

void macview_setmouserelative(BOOL relative)
{
	CGAssociateMouseAndMouseCursorPosition(relative ? false : true);
	if (relative) {
		/* discard the movement made before the capture */
		int32_t dx, dy;
		CGGetLastMouseDelta(&dx, &dy);
	}
}

void macview_getmousedelta(int *dx, int *dy)
{
	int32_t x, y;
	CGGetLastMouseDelta(&x, &y);
	*dx = x;
	*dy = y;
}

void macview_activateapp(void)
{
	[NSApp activateIgnoringOtherApps:YES];
}
