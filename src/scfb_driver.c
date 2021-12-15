/*
 * Copyright © 2001-2012 Matthieu Herrb
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * Based on fbdev.c written by:
 *
 * Authors:  Alan Hourihane, <alanh@fairlite.demon.co.uk>
 *	     Michel Dänzer, <michdaen@iiic.ethz.ch>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/consio.h>
#include <sys/fbio.h>

/* All drivers need this. */
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Priv.h"

#include "mipointer.h"
#include "micmap.h"
#include "colormapst.h"
#include "xf86cmap.h"
#include "shadow.h"
#include "dgaproc.h"

/* For visuals */
#ifdef HAVE_XF1BPP
# include "xf1bpp.h"
#endif
#ifdef HAVE_XF4BPP
# include "xf4bpp.h"
#endif
#include "fb.h"

#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) < 6
#include "xf86Resources.h"
#include "xf86RAC.h"
#endif

#ifdef XvExtension
#include "xf86xv.h"
#endif

#include "compat-api.h"

#undef	DEBUG
#define	DEBUG	1

#if DEBUG
# define TRACE_ENTER(str)	ErrorF("scfb: " str " %d\n",pScrn->scrnIndex)
# define TRACE_EXIT(str)	ErrorF("scfb: " str " done\n")
# define TRACE(str)		ErrorF("scfb trace: " str "\n")
#else
# define TRACE_ENTER(str)
# define TRACE_EXIT(str)
# define TRACE(str)
#endif

/* Prototypes */
static pointer ScfbSetup(pointer, pointer, int *, int *);
static Bool ScfbGetRec(ScrnInfoPtr);
static void ScfbFreeRec(ScrnInfoPtr);
static const OptionInfoRec * ScfbAvailableOptions(int, int);
static void ScfbIdentify(int);
static Bool ScfbProbe(DriverPtr, int);
static Bool ScfbPreInit(ScrnInfoPtr, int);
static Bool ScfbScreenInit(SCREEN_INIT_ARGS_DECL);
static Bool ScfbCloseScreen(CLOSE_SCREEN_ARGS_DECL);
static void *ScfbWindowLinear(ScreenPtr, CARD32, CARD32, int, CARD32 *,
			      void *);
static void ScfbPointerMoved(SCRN_ARG_TYPE, int, int);
static Bool ScfbEnterVT(VT_FUNC_ARGS_DECL);
static void ScfbLeaveVT(VT_FUNC_ARGS_DECL);
static Bool ScfbSwitchMode(SWITCH_MODE_ARGS_DECL);
static int ScfbValidMode(SCRN_ARG_TYPE, DisplayModePtr, Bool, int);
static void ScfbLoadPalette(ScrnInfoPtr, int, int *, LOCO *, VisualPtr);
static Bool ScfbSaveScreen(ScreenPtr, int);
static void ScfbSave(ScrnInfoPtr);
static void ScfbRestore(ScrnInfoPtr);

/* DGA stuff */
#ifdef XFreeXDGA
static Bool ScfbDGAOpenFramebuffer(ScrnInfoPtr, char **, unsigned char **,
				   int *, int *, int *);
static Bool ScfbDGASetMode(ScrnInfoPtr, DGAModePtr);
static void ScfbDGASetViewport(ScrnInfoPtr, int, int, int);
static Bool ScfbDGAInit(ScrnInfoPtr, ScreenPtr);
#endif
static Bool ScfbDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op,
				pointer ptr);

/* Helper functions */
static pointer scfb_mmap(size_t, off_t, int);

enum { SCFB_ROTATE_NONE = 0,
       SCFB_ROTATE_CCW = 90,
       SCFB_ROTATE_UD = 180,
       SCFB_ROTATE_CW = 270
};

/*
 * This is intentionally screen-independent.
 * It indicates the binding choice made in the first PreInit.
 */
static int pix24bpp = 0;

#define SCFB_VERSION 		0002
#define SCFB_NAME		"scfb"
#define SCFB_DRIVER_NAME	"scfb"

_X_EXPORT DriverRec SCFB = {
	SCFB_VERSION,
	SCFB_DRIVER_NAME,
	ScfbIdentify,
	ScfbProbe,
	ScfbAvailableOptions,
	NULL,
	0,
	ScfbDriverFunc
};

/* Supported "chipsets" */
static SymTabRec ScfbChipsets[] = {
	{ 0, "scfb" },
	{ -1, NULL }
};

/* Supported options */
typedef enum {
	OPTION_SHADOW_FB,
	OPTION_ROTATE
} ScfbOpts;

static const OptionInfoRec ScfbOptions[] = {
	{ OPTION_SHADOW_FB, "ShadowFB", OPTV_BOOLEAN, {0}, FALSE},
	{ OPTION_ROTATE, "Rotate", OPTV_STRING, {0}, FALSE},
	{ -1, NULL, OPTV_NONE, {0}, FALSE}
};

static XF86ModuleVersionInfo ScfbVersRec = {
	"scfb",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XORG_VERSION_CURRENT,
	PACKAGE_VERSION_MAJOR,
	PACKAGE_VERSION_MINOR,
	PACKAGE_VERSION_PATCHLEVEL,
	ABI_CLASS_VIDEODRV,
	ABI_VIDEODRV_VERSION,
	NULL,
	{0, 0, 0, 0}
};

_X_EXPORT XF86ModuleData scfbModuleData = { &ScfbVersRec, ScfbSetup, NULL };

static pointer
ScfbSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&SCFB, module, HaveDriverFuncs);
		return (pointer)1;
	} else {
		if (errmaj != NULL)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
}

/* Private data */
typedef struct {
	int			fd; /* File descriptor of open device. */
	struct video_info	info;
	int			linebytes; /* Number of bytes per row. */
	unsigned char*		fbstart;
	unsigned char*		fbmem;
	size_t			fbmem_len;
	int			rotate;
	Bool			shadowFB;
	void *			shadow;
	CloseScreenProcPtr	CloseScreen;
	CreateScreenResourcesProcPtr CreateScreenResources;
	void			(*PointerMoved)(SCRN_ARG_TYPE, int, int);
	EntityInfoPtr		pEnt;

#ifdef XFreeXDGA
	/* DGA info */
	DGAModePtr		pDGAMode;
	int			nDGAMode;
#endif
	OptionInfoPtr		Options;
} ScfbRec, *ScfbPtr;

#define SCFBPTR(p) ((ScfbPtr)((p)->driverPrivate))

static Bool
ScfbGetRec(ScrnInfoPtr pScrn)
{

	if (pScrn->driverPrivate != NULL)
		return TRUE;

	pScrn->driverPrivate = xnfcalloc(sizeof(ScfbRec), 1);
	return TRUE;
}

static void
ScfbFreeRec(ScrnInfoPtr pScrn)
{

	if (pScrn->driverPrivate == NULL)
		return;
	free(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}

static const OptionInfoRec *
ScfbAvailableOptions(int chipid, int busid)
{
	return ScfbOptions;
}

static void
ScfbIdentify(int flags)
{
	xf86PrintChipsets(SCFB_NAME, "driver for wsdisplay framebuffer",
			  ScfbChipsets);
}

/* Map the framebuffer's memory. */
static pointer
scfb_mmap(size_t len, off_t off, int fd)
{
	int pagemask, mapsize;
	caddr_t addr;
	pointer mapaddr;

	pagemask = getpagesize() - 1;
	mapsize = ((int) len + pagemask) & ~pagemask;
	addr = 0;

	/*
	 * Try and make it private first, that way once we get it, an
	 * interloper, e.g. another server, can't get this frame buffer,
	 * and if another server already has it, this one won't.
	 */
	mapaddr = (pointer) mmap(addr, mapsize,
				 PROT_READ | PROT_WRITE, MAP_SHARED,
				 fd, off);
	if (mapaddr == (pointer) -1) {
		mapaddr = NULL;
	}
#if DEBUG
	ErrorF("mmap returns: addr %p len 0x%x, fd %d, off %lx\n", mapaddr, mapsize, fd, off);
#endif
	return mapaddr;
}

static Bool
ScfbProbe(DriverPtr drv, int flags)
{
	int i, fd, entity;
       	GDevPtr *devSections;
	int numDevSections;
	const char *dev;
	struct fbtype fb;
	Bool foundScreen = FALSE;

	TRACE("probe start");

	/* For now, just bail out for PROBE_DETECT. */
	if (flags & PROBE_DETECT)
		return FALSE;

	if ((numDevSections = xf86MatchDevice(SCFB_DRIVER_NAME,
					      &devSections)) <= 0)
		return FALSE;


	for (i = 0; i < numDevSections; i++) {
		ScrnInfoPtr pScrn = NULL;
		dev = xf86FindOptionValue(devSections[i]->options, "device");
		if ((fd = xf86Info.consoleFd) >= 0 &&
		    ioctl(fd, FBIOGTYPE, &fb) != -1) {
			entity = xf86ClaimFbSlot(drv, 0, devSections[i], TRUE);
			pScrn = xf86ConfigFbEntity(NULL,0,entity,
						   NULL,NULL,NULL,NULL);
			if (pScrn != NULL) {
				foundScreen = TRUE;
				pScrn->driverVersion = SCFB_VERSION;
				pScrn->driverName = SCFB_DRIVER_NAME;
				pScrn->name = SCFB_NAME;
				pScrn->Probe = ScfbProbe;
				pScrn->PreInit = ScfbPreInit;
				pScrn->ScreenInit = ScfbScreenInit;
				pScrn->SwitchMode = ScfbSwitchMode;
				pScrn->AdjustFrame = NULL;
				pScrn->EnterVT = ScfbEnterVT;
				pScrn->LeaveVT = ScfbLeaveVT;
				pScrn->ValidMode = ScfbValidMode;

				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				    "using %s\n", dev != NULL ? dev :
				    "default device");
			}
		}
	}
	free(devSections);
	TRACE("probe done");
	return foundScreen;
}

static Bool
ScfbPreInit(ScrnInfoPtr pScrn, int flags)
{
	ScfbPtr fPtr;
	struct fbtype fb;
	int default_depth, wstype;
	const char *dev;
	char *mod = NULL;
	const char *reqSym = NULL, *s;
	Gamma zeros = {0.0, 0.0, 0.0};
	DisplayModePtr mode;

	if (flags & PROBE_DETECT) return FALSE;

	TRACE_ENTER("PreInit");

	if (pScrn->numEntities != 1) return FALSE;

	pScrn->monitor = pScrn->confScreen->monitor;

	ScfbGetRec(pScrn);
	fPtr = SCFBPTR(pScrn);

	fPtr->pEnt = xf86GetEntityInfo(pScrn->entityList[0]);

#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) < 6
	pScrn->racMemFlags = RAC_FB | RAC_COLORMAP | RAC_CURSOR | RAC_VIEWPORT;
	pScrn->racIoFlags = pScrn->racMemFlags;
#endif

	dev = xf86FindOptionValue(fPtr->pEnt->device->options, "device");
	fPtr->fd = xf86Info.consoleFd;
	if (fPtr->fd == -1) {
		return FALSE;
	}

	if (ioctl(fPtr->fd, FBIOGTYPE, &fb) == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "ioctl FBIOGTYPE: %s\n",
			   strerror(errno));
		return FALSE;
	}

	fPtr->info.vi_depth = fb.fb_depth;
	fPtr->info.vi_width = fb.fb_width;
	fPtr->info.vi_height = fb.fb_height;
	fPtr->info.vi_pixel_size = fb.fb_depth/8;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
	    "Using: depth (%d),\twidth (%d),\t height (%d)\n",
	    fPtr->info.vi_depth,fPtr->info.vi_width, fPtr->info.vi_height);

	if (ioctl(fPtr->fd, FBIO_GETLINEWIDTH, &fPtr->linebytes) == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "ioctl FBIO_GETLINEWIDTH fail: %s. "
			   "Falling back to width * bytes per pixel.\n",
			   strerror(errno));
		fPtr->linebytes = fPtr->info.vi_width *
		    fPtr->info.vi_pixel_size;
	}

	/* Handle depth */
	default_depth = fPtr->info.vi_depth <= 24 ? fPtr->info.vi_depth : 24;
	if (!xf86SetDepthBpp(pScrn, default_depth, default_depth,
		fPtr->info.vi_depth,
		fPtr->info.vi_depth >= 24 ? Support24bppFb|Support32bppFb : 0))
		return FALSE;

	/* Check consistency. */
	if (pScrn->bitsPerPixel != fPtr->info.vi_depth) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "specified depth (%d) or bpp (%d) doesn't match "
		    "framebuffer depth (%d)\n", pScrn->depth,
		    pScrn->bitsPerPixel, fPtr->info.vi_depth);
		return FALSE;
	}
	xf86PrintDepthBpp(pScrn);

	/* Get the depth24 pixmap format. */
	if (pScrn->depth == 24 && pix24bpp == 0)
		pix24bpp = xf86GetBppFromDepth(pScrn, 24);

	/* Color weight */
	if (pScrn->depth > 8) {
		rgb zeros = { 0, 0, 0 }, masks = { 0, 0, 0 };
#ifdef FBIO_GETRGBOFFS
		struct fb_rgboffs offs;

		if (ioctl(fPtr->fd, FBIO_GETRGBOFFS, &offs) == -1) {
			xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				   "ioctl FBIO_GETRGBOFFS fail: %s. "
				   "Falling back to default color format.\n",
				   strerror(errno));
			memset(&offs, 0, sizeof(offs));
		}

		/*
		 * If FBIO_GETRGBOFFS returned any non-zero offset, set the
		 * RGB masks appropriately.
		 *
		 * As there was an issue in Xorg server's RGB masks handling
		 * (https://gitlab.freedesktop.org/xorg/xserver/-/issues/1112),
		 * that is only fixed in master and 21.1.x releases for now,
		 * avoid modifying the masks if they correspond to the default
		 * values used by X.
		 */
		if ((offs.red != 0 || offs.green != 0 || offs.blue != 0) &&
		    !(offs.red == 16 && offs.green == 8 && offs.blue == 0)) {
			masks.red = 0xff << offs.red;
			masks.green = 0xff << offs.green;
			masks.blue = 0xff << offs.blue;
		}
#endif

		if (!xf86SetWeight(pScrn, zeros, masks))
			return FALSE;
	}

	/* Visual init */
	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;

	/* We don't currently support DirectColor at > 8bpp . */
	if (pScrn->depth > 8 && pScrn->defaultVisual != TrueColor) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "Given default visual"
			   " (%s) is not supported at depth %d\n",
			   xf86GetVisualName(pScrn->defaultVisual),
			   pScrn->depth);
		return FALSE;
	}

	xf86SetGamma(pScrn,zeros);

	pScrn->progClock = TRUE;
	pScrn->rgbBits   = 8;
	pScrn->chipset   = "scfb";
	pScrn->videoRam  = fPtr->linebytes * fPtr->info.vi_height;

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Vidmem: %dk\n",
		   pScrn->videoRam/1024);

	/* Handle options. */
	xf86CollectOptions(pScrn, NULL);
	fPtr->Options = (OptionInfoRec *)malloc(sizeof(ScfbOptions));
	if (fPtr->Options == NULL)
		return FALSE;
	memcpy(fPtr->Options, ScfbOptions, sizeof(ScfbOptions));
	xf86ProcessOptions(pScrn->scrnIndex, fPtr->pEnt->device->options,
			   fPtr->Options);

	/* Use shadow framebuffer by default, on depth >= 8 */
	if (pScrn->depth >= 8)
		fPtr->shadowFB = xf86ReturnOptValBool(fPtr->Options,
						      OPTION_SHADOW_FB, TRUE);
	else
		if (xf86ReturnOptValBool(fPtr->Options,
					 OPTION_SHADOW_FB, FALSE)) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "Shadow FB option ignored on depth < 8");
		}

	/* Rotation */
	fPtr->rotate = SCFB_ROTATE_NONE;
	if ((s = xf86GetOptValString(fPtr->Options, OPTION_ROTATE))) {
		if (pScrn->depth >= 8) {
			if (!xf86NameCmp(s, "CW")) {
				fPtr->shadowFB = TRUE;
				fPtr->rotate = SCFB_ROTATE_CW;
				xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
				    "Rotating screen clockwise\n");
			} else if (!xf86NameCmp(s, "CCW")) {
				fPtr->shadowFB = TRUE;
				fPtr->rotate = SCFB_ROTATE_CCW;
				xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
				    "Rotating screen counter clockwise\n");
			} else if (!xf86NameCmp(s, "UD")) {
				fPtr->shadowFB = TRUE;
				fPtr->rotate = SCFB_ROTATE_UD;
				xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
				    "Rotating screen upside down\n");
			} else {
				xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
				    "\"%s\" is not a valid value for Option "
				    "\"Rotate\"\n", s);
				xf86DrvMsg(pScrn->scrnIndex, X_INFO,
				    "Valid options are \"CW\", \"CCW\","
				    " or \"UD\"\n");
			}
		} else {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			    "Option \"Rotate\" ignored on depth < 8");
		}
	}

	/* Fake video mode struct. */
	mode = (DisplayModePtr)malloc(sizeof(DisplayModeRec));
	mode->prev = mode;
	mode->next = mode;
	mode->name = "scfb current mode";
	mode->status = MODE_OK;
	mode->type = M_T_BUILTIN;
	mode->Clock = 0;
	mode->HDisplay = fPtr->info.vi_width;
	mode->HSyncStart = 0;
	mode->HSyncEnd = 0;
	mode->HTotal = 0;
	mode->HSkew = 0;
	mode->VDisplay = fPtr->info.vi_height;
	mode->VSyncStart = 0;
	mode->VSyncEnd = 0;
	mode->VTotal = 0;
	mode->VScan = 0;
	mode->Flags = 0;
	if (pScrn->modes != NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
		   "Ignoring mode specification from screen section\n");
	}
	pScrn->currentMode = pScrn->modes = mode;
	pScrn->virtualX = fPtr->info.vi_width;
	pScrn->virtualY = fPtr->info.vi_height;
	pScrn->displayWidth = pScrn->virtualX;

	/* Set the display resolution. */
	xf86SetDpi(pScrn, 0, 0);

	/* Load bpp-specific modules. */
	switch(pScrn->bitsPerPixel) {
#ifdef HAVE_XF1BPP
	case 1:
		mod = "xf1bpp";
		reqSym = "xf1bppScreenInit";
		break;
#endif
#ifdef HAVE_XF4BPP
	case 4:
		mod = "xf4bpp";
		reqSym = "xf4bppScreenInit";
		break;
#endif
	default:
		mod = "fb";
		break;
	}

	/* Load shadow if needed. */
	if (fPtr->shadowFB) {
		xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
			   "Using \"Shadow Framebuffer\"\n");
		if (xf86LoadSubModule(pScrn, "shadow") == NULL) {
			ScfbFreeRec(pScrn);
			return FALSE;
		}
	}
	if (mod && xf86LoadSubModule(pScrn, mod) == NULL) {
		ScfbFreeRec(pScrn);
		return FALSE;
	}
	TRACE_EXIT("PreInit");
	return TRUE;
}

static void
scfbUpdateRotatePacked(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    shadowUpdateRotatePacked(pScreen, pBuf);
}

static void
scfbUpdatePacked(ScreenPtr pScreen, shadowBufPtr pBuf)
{
    shadowUpdatePacked(pScreen, pBuf);
}

static Bool
ScfbCreateScreenResources(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	ScfbPtr fPtr = SCFBPTR(pScrn);
	PixmapPtr pPixmap;
	Bool ret;

	pScreen->CreateScreenResources = fPtr->CreateScreenResources;
	ret = pScreen->CreateScreenResources(pScreen);
	pScreen->CreateScreenResources = ScfbCreateScreenResources;

	if (!ret)
		return FALSE;

	pPixmap = pScreen->GetScreenPixmap(pScreen);

	if (!shadowAdd(pScreen, pPixmap, fPtr->rotate ?
		scfbUpdateRotatePacked : scfbUpdatePacked,
		ScfbWindowLinear, fPtr->rotate, NULL)) {
		return FALSE;
	}
	return TRUE;
}


static Bool
ScfbShadowInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	ScfbPtr fPtr = SCFBPTR(pScrn);

	if (!shadowSetup(pScreen))
		return FALSE;
	fPtr->CreateScreenResources = pScreen->CreateScreenResources;
	pScreen->CreateScreenResources = ScfbCreateScreenResources;

	return TRUE;
}

static Bool
ScfbScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	ScfbPtr fPtr = SCFBPTR(pScrn);
	VisualPtr visual;
	int ret, flags, ncolors;
	size_t len;

	TRACE_ENTER("ScfbScreenInit");
#if DEBUG
	ErrorF("\tbitsPerPixel=%d, depth=%d, defaultVisual=%s\n"
	       "\tmask: %x,%x,%x, offset: %u,%u,%u\n",
	       pScrn->bitsPerPixel,
	       pScrn->depth,
	       xf86GetVisualName(pScrn->defaultVisual),
	       pScrn->mask.red,pScrn->mask.green,pScrn->mask.blue,
	       pScrn->offset.red,pScrn->offset.green,pScrn->offset.blue);
#endif
	switch (fPtr->info.vi_depth) {
	case 1:
	case 4:
	case 8:
		len = fPtr->linebytes*fPtr->info.vi_height;
		break;
	case 16:
		if (fPtr->linebytes == fPtr->info.vi_width) {
			len = fPtr->info.vi_width*fPtr->info.vi_height*sizeof(short);
		} else {
			len = fPtr->linebytes*fPtr->info.vi_height;
		}
		break;
	case 24:
		if (fPtr->linebytes == fPtr->info.vi_width) {
			len = fPtr->info.vi_width*fPtr->info.vi_height*3;
		} else {
			len = fPtr->linebytes*fPtr->info.vi_height;
		}
		break;
	case 32:
		if (fPtr->linebytes == fPtr->info.vi_width) {
			len = fPtr->info.vi_width*fPtr->info.vi_height*sizeof(int);
		} else {
			len = fPtr->linebytes*fPtr->info.vi_height;
		}
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "unsupported depth %d\n", fPtr->info.vi_depth);
		return FALSE;
	}
	/* TODO: Switch to graphics mode - required before mmap. */
	fPtr->fbmem = scfb_mmap(len, 0, fPtr->fd);

	if (fPtr->fbmem == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "scfb_mmap: %s\n", strerror(errno));
		return FALSE;
	}
	fPtr->fbmem_len = len;

	ScfbSave(pScrn);
	pScrn->vtSema = TRUE;

	/* MI layer */
	miClearVisualTypes();
	if (pScrn->bitsPerPixel > 8) {
		if (!miSetVisualTypes(pScrn->depth, TrueColorMask,
				      pScrn->rgbBits, TrueColor))
			return FALSE;
	} else {
		if (!miSetVisualTypes(pScrn->depth,
				      miGetDefaultVisualMask(pScrn->depth),
				      pScrn->rgbBits, pScrn->defaultVisual))
			return FALSE;
	}
	if (!miSetPixmapDepths())
		return FALSE;

	if (fPtr->rotate == SCFB_ROTATE_CW
	    || fPtr->rotate == SCFB_ROTATE_CCW) {
		int tmp = pScrn->virtualX;
		pScrn->virtualX = pScrn->displayWidth = pScrn->virtualY;
		pScrn->virtualY = tmp;
	}
	if (fPtr->rotate && !fPtr->PointerMoved) {
		fPtr->PointerMoved = pScrn->PointerMoved;
		pScrn->PointerMoved = ScfbPointerMoved;
	}

	fPtr->fbstart = fPtr->fbmem;

	if (fPtr->shadowFB) {
		fPtr->shadow = calloc(1, pScrn->virtualX * pScrn->virtualY *
		    pScrn->bitsPerPixel/8);

		if (!fPtr->shadow) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			    "Failed to allocate shadow framebuffer\n");
			return FALSE;
		}
	}

	switch (pScrn->bitsPerPixel) {
	case 1:
#ifdef HAVE_XF1BPP
		ret = xf1bppScreenInit(pScreen, fPtr->fbstart,
				       pScrn->virtualX, pScrn->virtualY,
				       pScrn->xDpi, pScrn->yDpi,
				       fPtr->linebytes * 8);
		break;
#endif
	case 4:
#ifdef HAVE_XF4BPP
		ret = xf4bppScreenInit(pScreen, fPtr->fbstart,
				       pScrn->virtualX, pScrn->virtualY,
				       pScrn->xDpi, pScrn->yDpi,
				       fPtr->linebytes * 2);
		break;
#endif
	case 8:
	case 16:
	case 24:
	case 32:
		ret = fbScreenInit(pScreen,
		    fPtr->shadowFB ? fPtr->shadow : fPtr->fbstart,
		    pScrn->virtualX, pScrn->virtualY,
		    pScrn->xDpi, pScrn->yDpi,
		    pScrn->displayWidth, pScrn->bitsPerPixel);
		break;
	default:
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Unsupported bpp: %d", pScrn->bitsPerPixel);
		return FALSE;
	} /* case */

	if (!ret)
		return FALSE;

	if (pScrn->bitsPerPixel > 8) {
		/* Fixup RGB ordering. */
		visual = pScreen->visuals + pScreen->numVisuals;
		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed   = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue  = pScrn->offset.blue;
				visual->redMask     = pScrn->mask.red;
				visual->greenMask   = pScrn->mask.green;
				visual->blueMask    = pScrn->mask.blue;
			}
		}
	}

	if (pScrn->bitsPerPixel >= 8) {
		if (!fbPictureInit(pScreen, NULL, 0))
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "RENDER extension initialisation failed.");
	}
	if (fPtr->shadowFB && !ScfbShadowInit(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
		    "shadow framebuffer initialization failed\n");
		return FALSE;
	}

#ifdef XFreeXDGA
	if (!fPtr->rotate)
		ScfbDGAInit(pScrn, pScreen);
	else
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Rotated display, "
		    "disabling DGA\n");
#endif
	if (fPtr->rotate) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO, "Enabling Driver Rotation, "
		    "disabling RandR\n");
		xf86DisableRandR();
		if (pScrn->bitsPerPixel == 24)
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			    "Rotation might be broken in 24 bpp\n");
	}

	xf86SetBlackWhitePixels(pScreen);
	xf86SetBackingStore(pScreen);

	/* Software cursor. */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	/*
	 * Colormap
	 *
	 * Note that, even on less than 8 bit depth frame buffers, we
	 * expect the colormap to be programmable with 8 bit values.
	 * As of now, this is indeed the case on all OpenBSD supported
	 * graphics hardware.
	 */
	if (!miCreateDefColormap(pScreen))
		return FALSE;
	flags = CMAP_RELOAD_ON_MODE_SWITCH;
	ncolors = 256;
	if(!xf86HandleColormaps(pScreen, ncolors, 8, ScfbLoadPalette,
				NULL, flags))
		return FALSE;

	pScreen->SaveScreen = ScfbSaveScreen;

#ifdef XvExtension
	{
		XF86VideoAdaptorPtr *ptr;

		int n = xf86XVListGenericAdaptors(pScrn,&ptr);
		if (n) {
			xf86XVScreenInit(pScreen,ptr,n);
		}
	}
#endif

	/* Wrap the current CloseScreen function. */
	fPtr->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = ScfbCloseScreen;

	TRACE_EXIT("ScfbScreenInit");
	return TRUE;
}

static Bool
ScfbCloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	PixmapPtr pPixmap;
	ScfbPtr fPtr = SCFBPTR(pScrn);


	TRACE_ENTER("ScfbCloseScreen");

	pPixmap = pScreen->GetScreenPixmap(pScreen);
	if (fPtr->shadowFB)
		shadowRemove(pScreen, pPixmap);

	if (pScrn->vtSema) {
		ScfbRestore(pScrn);
		if (munmap(fPtr->fbmem, fPtr->fbmem_len) == -1) {
			xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				   "munmap: %s\n", strerror(errno));
		}

		fPtr->fbmem = NULL;
	}
#ifdef XFreeXDGA
	if (fPtr->pDGAMode) {
		free(fPtr->pDGAMode);
		fPtr->pDGAMode = NULL;
		fPtr->nDGAMode = 0;
	}
#endif
	pScrn->vtSema = FALSE;

	/* Unwrap CloseScreen. */
	pScreen->CloseScreen = fPtr->CloseScreen;
	TRACE_EXIT("ScfbCloseScreen");
	return (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);
}

static void *
ScfbWindowLinear(ScreenPtr pScreen, CARD32 row, CARD32 offset, int mode,
		CARD32 *size, void *closure)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	ScfbPtr fPtr = SCFBPTR(pScrn);

	*size = fPtr->linebytes;
	return ((CARD8 *)fPtr->fbmem + row *fPtr->linebytes + offset);
}

static void
ScfbPointerMoved(SCRN_ARG_TYPE arg, int x, int y)
{
    SCRN_INFO_PTR(arg);
    ScfbPtr fPtr = SCFBPTR(pScrn);
    int newX, newY;

    switch (fPtr->rotate)
    {
    case SCFB_ROTATE_CW:
	/* 90 degrees CW rotation. */
	newX = pScrn->pScreen->height - y - 1;
	newY = x;
	break;

    case SCFB_ROTATE_CCW:
	/* 90 degrees CCW rotation. */
	newX = y;
	newY = pScrn->pScreen->width - x - 1;
	break;

    case SCFB_ROTATE_UD:
	/* 180 degrees UD rotation. */
	newX = pScrn->pScreen->width - x - 1;
	newY = pScrn->pScreen->height - y - 1;
	break;

    default:
	/* No rotation. */
	newX = x;
	newY = y;
	break;
    }

    /* Pass adjusted pointer coordinates to wrapped PointerMoved function. */
    (*fPtr->PointerMoved)(arg, newX, newY);
}

static Bool
ScfbEnterVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	TRACE_ENTER("EnterVT");
	pScrn->vtSema = TRUE;
	TRACE_EXIT("EnterVT");
	return TRUE;
}

static void
ScfbLeaveVT(VT_FUNC_ARGS_DECL)
{
#if DEBUG
	SCRN_INFO_PTR(arg);
#endif

	TRACE_ENTER("LeaveVT");
}

static Bool
ScfbSwitchMode(SWITCH_MODE_ARGS_DECL)
{
#if DEBUG
	SCRN_INFO_PTR(arg);
#endif

	TRACE_ENTER("SwitchMode");
	/* Nothing else to do. */
	return TRUE;
}

static int
ScfbValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode, Bool verbose, int flags)
{
#if DEBUG
	SCRN_INFO_PTR(arg);
#endif

	TRACE_ENTER("ValidMode");
	return MODE_OK;
}

static void
ScfbLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices,
	       LOCO *colors, VisualPtr pVisual)
{

	TRACE_ENTER("LoadPalette");
	/* TODO */
}

static Bool
ScfbSaveScreen(ScreenPtr pScreen, int mode)
{
	ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
	ScfbPtr fPtr = SCFBPTR(pScrn);
	int state;

	TRACE_ENTER("SaveScreen");

	if (!pScrn->vtSema)
		return TRUE;

	if (mode != SCREEN_SAVER_FORCER) {
		/* TODO, if (mode) enable_screen(); else disable_screen(); */
	}
	TRACE_EXIT("SaveScreen");
	return TRUE;
}


static void
ScfbSave(ScrnInfoPtr pScrn)
{
	ScfbPtr fPtr = SCFBPTR(pScrn);

	TRACE_ENTER("ScfbSave");

	TRACE_EXIT("ScfbSave");

}

static void
ScfbRestore(ScrnInfoPtr pScrn)
{
	ScfbPtr fPtr = SCFBPTR(pScrn);
	int mode;

	TRACE_ENTER("ScfbRestore");

	/* Clear the screen. */
	memset(fPtr->fbmem, 0, fPtr->fbmem_len);

	/* Restore the text mode. */
	/* TODO: We need to get first, if we need mode switching */
	TRACE_EXIT("ScfbRestore");
}

#ifdef XFreeXDGA
/***********************************************************************
 * DGA stuff
 ***********************************************************************/

static Bool
ScfbDGAOpenFramebuffer(ScrnInfoPtr pScrn, char **DeviceName,
		       unsigned char **ApertureBase, int *ApertureSize,
		       int *ApertureOffset, int *flags)
{
	*DeviceName = NULL;		/* No special device */
	*ApertureBase = (unsigned char *)(pScrn->memPhysBase);
	*ApertureSize = pScrn->videoRam;
	*ApertureOffset = pScrn->fbOffset;
	*flags = 0;

	return TRUE;
}

static Bool
ScfbDGASetMode(ScrnInfoPtr pScrn, DGAModePtr pDGAMode)
{
	DisplayModePtr pMode;
	int frameX0, frameY0;

	if (pDGAMode) {
		pMode = pDGAMode->mode;
		frameX0 = frameY0 = 0;
	} else {
		if (!(pMode = pScrn->currentMode))
			return TRUE;

		frameX0 = pScrn->frameX0;
		frameY0 = pScrn->frameY0;
	}

	if (!(*pScrn->SwitchMode)(SWITCH_MODE_ARGS(pScrn, pMode)))
		return FALSE;
	(*pScrn->AdjustFrame)(ADJUST_FRAME_ARGS(pScrn, frameX0, frameY0));

	return TRUE;
}

static void
ScfbDGASetViewport(ScrnInfoPtr pScrn, int x, int y, int flags)
{
	(*pScrn->AdjustFrame)(ADJUST_FRAME_ARGS(pScrn, x, y));
}

static int
ScfbDGAGetViewport(ScrnInfoPtr pScrn)
{
	return (0);
}

static DGAFunctionRec ScfbDGAFunctions =
{
	ScfbDGAOpenFramebuffer,
	NULL,       /* CloseFramebuffer */
	ScfbDGASetMode,
	ScfbDGASetViewport,
	ScfbDGAGetViewport,
	NULL,       /* Sync */
	NULL,       /* FillRect */
	NULL,       /* BlitRect */
	NULL,       /* BlitTransRect */
};

static void
ScfbDGAAddModes(ScrnInfoPtr pScrn)
{
	ScfbPtr fPtr = SCFBPTR(pScrn);
	DisplayModePtr pMode = pScrn->modes;
	DGAModePtr pDGAMode;

	do {
		pDGAMode = realloc(fPtr->pDGAMode,
				    (fPtr->nDGAMode + 1) * sizeof(DGAModeRec));
		if (!pDGAMode)
			break;

		fPtr->pDGAMode = pDGAMode;
		pDGAMode += fPtr->nDGAMode;
		(void)memset(pDGAMode, 0, sizeof(DGAModeRec));

		++fPtr->nDGAMode;
		pDGAMode->mode = pMode;
		pDGAMode->flags = DGA_CONCURRENT_ACCESS | DGA_PIXMAP_AVAILABLE;
		pDGAMode->byteOrder = pScrn->imageByteOrder;
		pDGAMode->depth = pScrn->depth;
		pDGAMode->bitsPerPixel = pScrn->bitsPerPixel;
		pDGAMode->red_mask = pScrn->mask.red;
		pDGAMode->green_mask = pScrn->mask.green;
		pDGAMode->blue_mask = pScrn->mask.blue;
		pDGAMode->visualClass = pScrn->bitsPerPixel > 8 ?
			TrueColor : PseudoColor;
		pDGAMode->xViewportStep = 1;
		pDGAMode->yViewportStep = 1;
		pDGAMode->viewportWidth = pMode->HDisplay;
		pDGAMode->viewportHeight = pMode->VDisplay;

		pDGAMode->bytesPerScanline = fPtr->linebytes;

		pDGAMode->imageWidth = pMode->HDisplay;
		pDGAMode->imageHeight =  pMode->VDisplay;
		pDGAMode->pixmapWidth = pDGAMode->imageWidth;
		pDGAMode->pixmapHeight = pDGAMode->imageHeight;
		pDGAMode->maxViewportX = pScrn->virtualX -
			pDGAMode->viewportWidth;
		pDGAMode->maxViewportY = pScrn->virtualY -
			pDGAMode->viewportHeight;

		pDGAMode->address = fPtr->fbstart;

		pMode = pMode->next;
	} while (pMode != pScrn->modes);
}

static Bool
ScfbDGAInit(ScrnInfoPtr pScrn, ScreenPtr pScreen)
{
	ScfbPtr fPtr = SCFBPTR(pScrn);

	if (pScrn->depth < 8)
		return FALSE;

	if (!fPtr->nDGAMode)
		ScfbDGAAddModes(pScrn);

	return (DGAInit(pScreen, &ScfbDGAFunctions,
			fPtr->pDGAMode, fPtr->nDGAMode));
}
#endif

static Bool
ScfbDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op,
    pointer ptr)
{
	xorgHWFlags *flag;

	switch (op) {
	case GET_REQUIRED_HW_INTERFACES:
		flag = (CARD32*)ptr;
		(*flag) = 0;
		return TRUE;
	default:
		return FALSE;
	}
}

