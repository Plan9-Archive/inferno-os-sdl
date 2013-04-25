#define _GNU_SOURCE 1
#define XTHREADS
#include "dat.h"
#include "fns.h"
#undef log2
#include <draw.h>
#include "cursor.h"
#include "keyboard.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <SDL/SDL.h>

void flushmemscreen(Rectangle r);
void flushwholescreen();

extern ulong displaychan;

static SDL_Surface *screen = NULL;
static SDL_Surface *swscreen = NULL;
static int triedscreen = 0;

static Queue *sdl_evq = nil;
static Queue *sdl_refreshq = nil;
static int sdlx, sdly;

static void sdlhandleevent(SDL_Event e);
static void sdlblitscreen(SDL_Rect r);

char *gkscanid = "emu_SDL";

static int
countbits(int n)
{
	int c = 0;
	while(n) {
		n &= (n - 1);
		c++;
	}
	return c;
}

void
sdleventproc(void *arg)
{
	USED(arg);
	SDL_Event e;

	while(1) {
		while(qlen(sdl_evq) >= sizeof(e)) {
			qread(sdl_evq, &e, sizeof(e));
			sdlhandleevent(e);
		}
		osmillisleep(1);
		osyield();
	}
}

/*
   SDL doesn't like for some things to be touched from multiple threads.  This
   is fixed, as I understand it, in the next version, but for now, we're
   confining rendering and events to one proc.
*/
static void sdl_proc(void *rq)
{
	const SDL_VideoInfo *info;
	const SDL_PixelFormat *pf;
	SDL_Event e;
	SDL_Rect sr, dr;
	int flags = 0;
	int i;

	i = SDL_Init(SDL_INIT_EVERYTHING);
	if(i < 0) {
		fprint(2, "emu: couldn't SDL_Init(): %r (SDL: %s)\n",
			SDL_GetError());
		cleanexit(0);
	}
	SDL_EnableUNICODE(1);

	info = SDL_GetVideoInfo();
	if(!info) {
		fprint(2, "emu: couldn't SDL_GetVideoInfo():  %r, SDL: %s\n",
		       SDL_GetError());
		SDL_Quit(); // FIXME:  Put this into cleanexit
		cleanexit(0);
	}

	flags |= SDL_SWSURFACE;
	// This upset the cursor for some reason.  I suspect my hardware.
	// flags |= SDL_DOUBLEBUF; 
	if(info->blit_hw)
		flags |= SDL_HWACCEL;

	screen = SDL_SetVideoMode(sdlx, sdly, info->vfmt->BitsPerPixel, flags);
	if(!screen) {
		fprint(2, "emu: win-SDL SetVideoMode failed: %r, (SDL: %s)\n",
		       SDL_GetError());
		SDL_Quit(); // FIXME:  Put this into cleanexit
		cleanexit(0);
	}
	kproc("sdleventproc", sdleventproc, NULL, 0);
	SDL_WM_SetCaption("Inferno", "Inferno");

	// Set up the buffer to draw on:
	pf = screen->format;
	swscreen = SDL_CreateRGBSurface(SDL_SWSURFACE, screen->w, screen->h,
		pf->BitsPerPixel, pf->Rmask, pf->Gmask, pf->Bmask, pf->Amask);

	if(!swscreen) {
		fprint(2, "emu: win-SDL CreateRGBSurface failed: %r, (SDL: %s)\n",
		       SDL_GetError());
		SDL_Quit(); // FIXME:  Put this into cleanexit
		cleanexit(0);
	}

	qproduce((Queue *)rq, &info, sizeof(SDL_VideoInfo *));

	i = 1;
	while(1) {
		/*
			FIXME: Update events seem to come a bit too infrequently
			(like...almost never) so this loop just clears the
			refresh queue and then refreshes the whole screen.  This
			made it eat CPU like crazy, so I put the thread to sleep
			once per loop.  That's two hacks.
		*/
		if(!i)
			osmillisleep(15);
		i = 0;

		while(qlen(sdl_refreshq) >= sizeof(sr)) {
			qread(sdl_refreshq, &sr, sizeof(sr));
			i++;
			/*sdlblitscreen(sr);*/
		}

		sr.x = 0;
		sr.y = 0;
		sr.w = swscreen->w;
		sr.h = swscreen->h;
		sdlblitscreen(sr);

		while(SDL_PollEvent(&e)) {
			i++;
			qproduce(sdl_evq, &e, sizeof(e));
		}

		osyield();
	}
}

static void
sdl_initscreen(int xsize, int ysize, ulong reqchan, ulong *chan, int *d)
{
	Queue *readyq;
	SDL_VideoInfo *info;
	const SDL_PixelFormat *pf;
	int bpp;
	int rlen, glen, blen, pad, c;
	int i;

	sdl_evq = qopen(sizeof(SDL_Event) * 32, 0, nil, nil);
	sdl_refreshq = qopen(sizeof(Rect) * 32, 0, nil, nil);
	sdlx = xsize;
	sdly = ysize;

	// Wait for the video thread to come back before we continue:
	readyq = qopen(sizeof(SDL_VideoInfo *), 0, nil, nil);
	//kproc("sdlproc", sdl_proc, readyq, KPX11);
	kproc("sdlproc", sdl_proc, readyq, 0);
	qread(readyq, &info, sizeof(SDL_VideoInfo *));
	qfree(readyq);
	readyq = nil;

	pf = swscreen->format;
	*d = pf->BitsPerPixel;
	rlen = countbits(pf->Rmask);
	glen = countbits(pf->Gmask);
	blen = countbits(pf->Bmask);
	pad = *d - (rlen + glen + blen);

	if(pf->Rshift > pf->Bshift) {
		c = CHAN3(CRed, rlen, CGreen, glen, CBlue, blen);
	} else {
		c = CHAN3(CBlue, blen, CGreen, glen, CRed, rlen);
	}
	if(pad > 0)
		c |= CHAN1(CIgnore, pad) << 24;
	*chan = c;
}

void sdlhandlekey(SDL_KeyboardEvent e)
{
	if(gkscanq != nil && e.keysym.sym < 0x80) {
		uchar ch = e.keysym.sym;
		if(ch == SDLK_RETURN)
			ch = '\n';
		if(e.type == SDL_KEYUP)
			ch |= 0x80;
		qproduce(gkscanq, &ch, 1);
		return;
	}

	if(e.type != SDL_KEYDOWN)
		return;

	int key = e.keysym.unicode? e.keysym.unicode : e.keysym.sym;

	switch(key) {
	case SDLK_UNKNOWN:
	case SDLK_RSHIFT:
	case SDLK_LSHIFT:
	case SDLK_RCTRL:
	case SDLK_LCTRL:
	case SDLK_RMETA:
	case SDLK_LMETA:
		return;

	// Unfortunately, SDL seems to send Delete for Backspace and CR for LF.
	// This may have to do with running under the Linux fbdev driver.
	case SDLK_DELETE:
		key = 8;
		break;
	case SDLK_RETURN:
		key = '\n';
		break;

	case SDLK_HOME:
		key = Home;
		break;
	case SDLK_END:
		key = End;
		break;
	case SDLK_INSERT:
		key = Ins;
		break;

	case SDLK_LEFT:
		key = Left;
		break;
	case SDLK_RIGHT:
		key = Right;
		break;
	case SDLK_UP:
		key = Up;
		break;
	case SDLK_DOWN:
		key = Down;
		break;

	case SDLK_PAGEUP:
		key = Pgup;
		break;
	case SDLK_PAGEDOWN:
		key = Pgdown;
		break;

	case SDLK_LALT:
	case SDLK_RALT:
		key = Latin;
		break;
	/* There are probably others that need to be handled. */
	}

	gkbdputc(gkbdq, key);
}

/*
	Luckily, SDL uses the same mask for buttons as Inferno.
	TODO:  Double-click.	
	TODO: Occasionally, SDL often seems to have odd opinions about
		mouse-chording.  Specifically, holding 1 and clicking 2, 3
		works.  Holding 1 and clicking 3 does not.
*/
void sdlhandlemouse(SDL_Event e)
{
	int b, x, y;
	b = SDL_GetMouseState(&x, &y);
	mousetrack(b, x, y, 0);
}

static void sdlhandleevent(SDL_Event e)
{
	switch(e.type) {
	case SDL_KEYUP:
	case SDL_KEYDOWN:
		sdlhandlekey(e.key);
		break;

	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
	case SDL_MOUSEMOTION:
		sdlhandlemouse(e);
		break;

	case SDL_ACTIVEEVENT:
	case SDL_SYSWMEVENT:
	case SDL_VIDEOEXPOSE:
		flushwholescreen();
		break;

	case SDL_QUIT:
		SDL_Quit();
		cleanexit(0);
		break;
	}
}

void
flushwholescreen()
{
	Rectangle r;
	r.min.x = 0;
	r.min.y = 0;
	r.max.x = swscreen->w;
	r.max.y = swscreen->h;
	flushmemscreen(r);
}

// *d is, I think, supposed to be filled in with depth in bits
// width is how many *bytes* wide the screen is.
// Think the softscreen flag is for software vs. hardware buffers.
// It's expected to return a pointer to the display buffer (i.e., raw pixels)...I think.
uchar*
attachscreen(Rectangle *r, ulong *chan, int *d, int *width, int *softscreen)
{
	Xsize &= ~0x3;	/* ensure multiple of 4 */

	r->min.x = 0;
	r->min.y = 0;
	r->max.x = Xsize;
	r->max.y = Ysize;

	// Fill in the screen data if present, initialize a screen if not.
	if(!triedscreen) {
		triedscreen = 1;
		sdl_initscreen(Xsize, Ysize, displaychan, chan, d);
	} else {
		*chan = displaychan;
		*d = swscreen->format->BitsPerPixel;
	}

	*width = (Xsize/4)*(*d/8);
	*softscreen = 0;
	displaychan = *chan;

	return swscreen->pixels;
}

static SDL_Rect
rect2sdl(Rectangle r)
{
	SDL_Rect sr;
	sr.x = r.min.x;
	sr.y = r.min.y;
	sr.w = r.max.x - r.min.x;
	sr.h = r.max.y - r.min.y;
	return sr;
}

static int sdlmaybelock(SDL_Surface *s)
{
	if(SDL_MUSTLOCK(s)) {
		if(SDL_LockSurface(s) < 0) {
			return -1;
		}
	}
	return 0;
}

static void sdlmaybeunlock(SDL_Surface * s)
{
	if(SDL_MUSTLOCK(s)) {
		SDL_UnlockSurface(s);
	}
}

#define max(a,b)	((a) >  (b) ? (a) : (b))
#define min(a,b)	((a) <= (b) ? (a) : (b))

void
flushmemscreen(Rectangle r)
{
	SDL_Rect sr;
	int i;

	if(!screen || !swscreen) {
		fprint(2, "emu-SDL: attempt to flush video memory with no screen\n");
		return;
	}

	r.min.x = max(0, r.min.x);
	r.min.y = max(0, r.min.y);
	r.max.x = min(swscreen->w-1, r.max.x);
	r.max.y = min(swscreen->h-1, r.max.y);

	sr = rect2sdl(r);
	qwrite(sdl_refreshq, &sr, sizeof(sr));
}


static void
sdlblitscreen(SDL_Rect r)
{
	int i;
	SDL_Rect d = r;

	i = SDL_BlitSurface(swscreen, &r, screen, &d);
	if(i == 0 && sdlmaybelock(screen) == 0) {
		SDL_Flip(screen);
		sdlmaybeunlock(screen);
	}
}

void
setpointer(int x, int y)
{
	SDL_WarpMouse(x, y);
}

/* SDL Handles this for us: */
void
drawcursor(Drawcursor* c)
{
}


static char *snarfbuffer = nil;
/*
 * Snarf buffer.
 * FIXME:  I don't think SDL 1.2 has facilities for this; 2.0 seems to.
 */
char*
clipread(void)
{
	if(snarfbuffer == nil)
		return nil;
	return strdup(snarfbuffer);
}

int
clipwrite(char *buf)
{
	snarfbuffer = strdup(buf);
}
