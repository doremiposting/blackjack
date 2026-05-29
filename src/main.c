#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <math.h>

#include <errno.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>
#include <X11/Xatom.h>

#include "main.h"

Display *display;
Window window;
XWindowAttributes wa = {0};
GC gc;
Atom wmdelwin;
XftFont *font;
Pixmap pixmap;
XftDraw *xftdraw;
Visual *vis;
Colormap cmap;
int ptyfd;
pid_t ptypid;
int visrows, viscols;
volatile sig_atomic_t toggletheme;
int isdark;
int fontsize;
Atom xaclipboard, xautf8str, xatargets, xaseldata;
Atom xanetname;
char *cliptext;
size_t cliptextsz;
int selactive, selexists, selancrow, selanccol;
int selrow1, selcol1, selrow2, selcol2;
int selscrolldir, selscrolltick;
int selmousex, selmousey;

#define TBUFCOLS 256
/* #define TBUFROWS 128 */
#define TBUFROWS 8196
#define CDEFAULT 255 /* sentinel: use terminal defaults for fg and bg */
#define ATTRBOLD (1<<0)
#define ATTRDIM (1<<1)
#define ATTRITALIC (1<<2)
#define ATTRUNDER (1<<3)
#define ATTRREVERSE (1<<4)
typedef struct {
  char ch;
  unsigned char fg, bg;
  unsigned char attrs;
} Cell;
typedef struct {
  Cell lines[TBUFROWS][TBUFCOLS];
  int col, row;
  int scroll;
  int svrow, svcol, svscroll;
  int scrolltop, scrollbot;
} Termbuf;
Termbuf tbufs[2];
Termbuf *tbuf;

typedef enum {
  TSMNORMAL,
  TSMESC,
  TSMCHARSEL,
  TSMCSI,
  TSMOSC
} Tsmstate;
#define TSMPARAMS 8
typedef struct {
  Tsmstate state;
  int params[TSMPARAMS];
  int nparams;
  int crtparam;
  int hascurrent;
  int savecol, saverow;
  int priv;
  int curshape, curblink;
  int appkeys;
  unsigned char sgrfg, sgrbg;
  unsigned char sgrattrs;
  char oscbuf[512];
  int osclen;
} Tsm;
Tsm tsm;

#define NRAINBOW 6
typedef struct {
  const char *black, *brblack;
  const char *red, *brred;
  const char *green, *brgreen;
  const char *yellow, *bryellow;
  const char *blue, *brblue;
  const char *magenta, *brmagenta;
  const char *cyan, *brcyan;
  const char *white, *brwhite;
  char **slushclrs;
  const char *fg, *bg;
  const char *cursorfg, *cursorbg;
  const char *rcursorfg, *rcursorbg;
  const char *rainbow[NRAINBOW];
} Colorscheme;
static const Colorscheme darksch = {
  .black     = "#282c34", .brblack   = "#545862",
  .red       = "#e06c75", .brred     = "#e06c75",
  .green     = "#98c379", .brgreen   = "#98c379",
  .yellow    = "#e5c07b", .bryellow  = "#e5c07b",
  .blue      = "#61afef", .brblue    = "#61afef",
  .magenta   = "#c678dd", .brmagenta = "#c678dd",
  .cyan      = "#56b6c2", .brcyan    = "#56b6c2",
  .white     = "#abb2bf", .brwhite   = "#c8ccd4",
  .slushclrs = (void *)0,
  .fg        = "#abb2bf", .bg        = "#282c34",
  .cursorfg  = "#282c34", .cursorbg  = "#abb2bf",
  .rcursorfg = "#abb2bf", .rcursorbg = "#282c34",
  .rainbow = {
    "#f7768e",
    "#e0af68",
    "#9ece6a",
    "#7dcfff",
    "#7aa2f7",
    "#bb9af7",
  },
};
static const Colorscheme lightsch = {
  .black     = "#000000", .brblack   = "#8e908c",
  .red       = "#c82829", .brred     = "#ff3334",
  .green     = "#718c00", .brgreen   = "#9ec400",
  .yellow    = "#f5871f", .bryellow  = "#eab700",
  .blue      = "#4271ae", .brblue    = "#5795e6",
  .magenta   = "#8959a8", .brmagenta = "#b777e0",
  .cyan      = "#3e999f", .brcyan    = "#54ced6",
  .white     = "#d6d6d6", .brwhite   = "#efefef",
  .slushclrs = (void *)0,
  .fg        = "#4d4d4d", .bg        = "#ffffff",
  .cursorfg  = "#000000", .cursorbg  = "#d6d6d6",
  .rcursorfg = "#d6d6d6", .rcursorbg = "#000000",
	.rainbow = {
    "#cc0000",
    "#c4a000",
    "#4e9a06",
    "#06989a",
    "#3465a4",
    "#75507b",
	},
};
const Colorscheme *clrs;
XftColor colorfg, colorbg;
XftColor palette[16];
XftColor cursorfgclr, cursorbgclr;
XftColor cursorfgrev, cursorbgrev;
XftColor throbpalette[NRAINBOW];
double throbphase;
int throbcsr;
int screendirty;

struct timespec thenr, nowr;
long long elapsedr;
#define GFXTICKNS 16666667LL
/* #define GFXTICKNS 600000000LL */
#define GETNS(ts) (clock_gettime(CLOCK_MONOTONIC, &ts))
#define DIFFNS(start, end) \
      ((int64_t)((end).tv_sec - (start).tv_sec) * 1000000000LL + \
      ((end).tv_nsec - (start).tv_nsec))


unsigned int WWIDTH, WHEIGHT;

#define ERRORTH -1
#define LIGHTTH 0
#define DARKTH 1
int
detectdark() {
  char path[512];
  char *home, *xdg;
  FILE *f;
  char line[256];
  static char *inipath = "gtk-3.0/settings.ini";
  xdg = getenv("XDG_CONFIG_HOME");
  home = getenv("HOME");
  if (xdg) {
    snprintf(path, sizeof(path), "%s/%s", xdg, inipath);
  } else if (home) {
    snprintf(path, sizeof(path), "%s/.config/%s", home, inipath);
  } else { return ERRORTH; }
  f = fopen(path, "r");
  if (!f) { return ERRORTH; }
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "gtk-application-prefer-dark-theme=1")) { fclose(f); return DARKTH; }
  }
  fclose(f);
  return LIGHTTH;
}

void
x11init() {
  WWIDTH = 800;
  WHEIGHT = 600;
  display = XOpenDisplay(NULL);
  if (!display) { fprintf(stderr, "ERROR: Couldn't open display!\n"); exit(1); }
  window = XCreateSimpleWindow(
      display,
      XDefaultRootWindow(display),
      0, 0,
      WWIDTH, WHEIGHT,
      0, 0, 0
    );
  XGetWindowAttributes(display, window, &wa);
  gc = XCreateGC(display, window, 0, NULL);
  wmdelwin = XInternAtom(display, "WM_DELETE_WINDOW", false);
  xaclipboard = XInternAtom(display, "CLIPBOARD", false);
  xautf8str = XInternAtom(display, "UTF8_STRING", false);
  xatargets = XInternAtom(display, "TARGETS", false);
  xaseldata = XInternAtom(display, "XSEL_DATA", false);
  xanetname = XInternAtom(display, "_NET_WM_NAME", false);
  XSetWMProtocols(display, window, &wmdelwin, 1);
  XSelectInput(display, window, KeyPressMask|PointerMotionMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask);
  XStoreName(display, window, "bj");
  XMapWindow(display, window);
}

void
x11kill() {
  XDestroyWindow(display, window);
  XCloseDisplay(display);
}

void
fontinit() {
  char fontspec[128];
  /* TODO: Pull font name out into config.h */
  snprintf(fontspec, sizeof(fontspec), "monospace:size=%d", fontsize);
  font = XftFontOpenName(display, DefaultScreen(display), fontspec);
  if (!font) { fprintf(stderr, "ERROR: Couldn't open font!\n"); exit(1); }
}

void
fontkill() {
  XftFontClose(display, font);
}

void drawresize();
void ptyresize();

void
changefontsz(int delta) {
  int i;
  fontkill();
  fontsize += delta;
  if (fontsize < 6) { fontsize = 6; }
  if (fontsize > 72) { fontsize = 72; }
  fontinit();
  visrows = (int)(WHEIGHT / (unsigned int)(font->ascent + font->descent));
  viscols = (int)(WWIDTH / (unsigned int)font->max_advance_width);
  for (i = 0; i < 2; i++) {
    tbufs[i].scrolltop = 0;
    tbufs[i].scrollbot = 0;
    if (tbufs[i].row >= tbufs[i].scroll + visrows) {
      tbufs[i].row = tbufs[i].scroll + visrows - 1;
    }
  }
  screendirty = 1;
  drawresize();
  ptyresize();
}

void
clipcopy(const char *text, int len, Time t) {
  free(cliptext);
  cliptext = malloc((size_t)len);
  if (!cliptext) { cliptextsz = 0; return; }
  memcpy(cliptext, text, (size_t)len);
  cliptextsz = (size_t)len;
  XSetSelectionOwner(display, xaclipboard, window, t);
}

void
clippaste(Time time) {
  XConvertSelection(display, xaclipboard, xautf8str, xaseldata, window, time);
}

void applycolors();
void
colorsinit() {
  vis = DefaultVisual(display, DefaultScreen(display));
  cmap = DefaultColormap(display, DefaultScreen(display));
  applycolors();
}

void
applycolors() {
  int i;
  const char *palstrs[16];
  static int inited = 0;
  if (inited) {
    XftColorFree(display, vis, cmap, &colorfg);
    XftColorFree(display, vis, cmap, &colorbg);
    XftColorFree(display, vis, cmap, &cursorfgclr);
    XftColorFree(display, vis, cmap, &cursorbgclr);
    XftColorFree(display, vis, cmap, &cursorfgrev);
    XftColorFree(display, vis, cmap, &cursorbgrev);
    for (i = 0; i < 16; i++) { XftColorFree(display, vis, cmap, &palette[i]); }
    for (i = 0; i < NRAINBOW; i++) { XftColorFree(display, vis, cmap, &throbpalette[i]); }
  }
  inited = 1;
  clrs = isdark ? &darksch : &lightsch;
  palstrs[0] = clrs->black; palstrs[8] = clrs->brblack;
  palstrs[1] = clrs->red; palstrs[9] = clrs->brred;
  palstrs[2] = clrs->green; palstrs[10] = clrs->brgreen;
  palstrs[3] = clrs->yellow; palstrs[11] = clrs->bryellow;
  palstrs[4] = clrs->blue; palstrs[12] = clrs->brblue;
  palstrs[5] = clrs->magenta; palstrs[13] = clrs->brmagenta;
  palstrs[6] = clrs->cyan; palstrs[14] = clrs->brcyan;
  palstrs[7] = clrs->white; palstrs[15] = clrs->brwhite;
  for (i = 0; i < 16; i++) { XftColorAllocName(display, vis, cmap, palstrs[i], &palette[i]); }
  for (i = 0; i < NRAINBOW; i++) { XftColorAllocName(display, vis, cmap, clrs->rainbow[i], &throbpalette[i]); }
  XftColorAllocName(display, vis, cmap, clrs->fg, &colorfg);
  XftColorAllocName(display, vis, cmap, clrs->bg, &colorbg);
  XftColorAllocName(display, vis, cmap, clrs->cursorfg, &cursorfgclr);
  XftColorAllocName(display, vis, cmap, clrs->cursorbg, &cursorbgclr);
  XftColorAllocName(display, vis, cmap, clrs->rcursorfg, &cursorfgrev);
  XftColorAllocName(display, vis, cmap, clrs->rcursorbg, &cursorbgrev);
}

static void
handlesigusr1(int sig) {
  UNUSED(sig);
  toggletheme = 1;
} 

void
killcolors() {
  int i;
  XftColorFree(display, vis, cmap, &colorfg);
  XftColorFree(display, vis, cmap, &colorbg);
  XftColorFree(display, vis, cmap, &cursorfgclr);
  XftColorFree(display, vis, cmap, &cursorbgclr);
  XftColorFree(display, vis, cmap, &cursorfgrev);
  XftColorFree(display, vis, cmap, &cursorbgrev);
  for (i = 0 ; i < 16 ; i++) { XftColorFree(display, vis, cmap, &palette[i]); }
  for (i = 0 ; i < NRAINBOW ; i++) { XftColorFree(display, vis, cmap, &throbpalette[i]); }
}

void
drawinit() {
  pixmap = XCreatePixmap(display, window, WWIDTH, WHEIGHT, (unsigned int)wa.depth);
  xftdraw = XftDrawCreate(display, pixmap, vis, cmap);
}

void
drawkill() {
  XftDrawDestroy(xftdraw);
  XFreePixmap(display, pixmap);
}

static void
cellsetrow(Cell *cells, int n) {
  int i;
  for (i = 0 ; i < n ; i++) {
    cells[i].ch = ' ';
    cells[i].fg = CDEFAULT;
    cells[i].bg = CDEFAULT;
    cells[i].attrs = 0;
  }
}

static XftColor *
cellcolor(unsigned char idx, int isfg) {
  if (idx == CDEFAULT) { return isfg ? &colorfg : &colorbg; }
  if (idx < 16) { return &palette[idx]; }
  return isfg ? &colorfg : &colorbg; /* TODO: 256-color fallback */
}

static void
blendcolor(XftColor *dst, XftColor *a, XftColor *b, double t) {
  dst->color.red = (unsigned short)(a->color.red * (1.0 - t) + b->color.red * t);
	dst->color.green = (unsigned short)(a->color.green * (1.0 - t) + b->color.green * t);
	dst->color.blue = (unsigned short)(a->color.blue * (1.0 - t) + b->color.blue * t);
  dst->color.alpha = 0xffff; /* TODO: Blend transparencies */
  dst->pixel = 0;
}

void
drawcell(int col, int row, Cell *cell, XftColor *fg, XftColor *bg) {
  int cw, ch, x, y;
  cw = font->max_advance_width;
  ch = font->ascent + font->descent;
  x = col * cw;
  y = row * ch;
  XftDrawRect(xftdraw, bg, x, y, (unsigned int)cw, (unsigned int)ch);
  XftDrawStringUtf8(xftdraw, fg, font,
      x, y + font->ascent,
      (FcChar8 *)&cell->ch, 1);
  if (cell->attrs & ATTRUNDER) {
    XftDrawRect(xftdraw, fg, x, (y + ch - 1), (unsigned int)cw, 1);
  }
}

void
drawflush() {
  XCopyArea(display, pixmap, window, gc, 0, 0, WWIDTH, WHEIGHT, 0, 0);
  XFlush(display);
}

void
drawresize() {
  XftDrawDestroy(xftdraw);
  XFreePixmap(display, pixmap);
  pixmap = XCreatePixmap(display, window, WWIDTH, WHEIGHT, (unsigned int)wa.depth);
  xftdraw = XftDrawCreate(display, pixmap, vis, cmap);
}

void
tbufclear(Termbuf *b) {
  int r, c;
  for (r = 0; r < TBUFROWS; r++) {
    for (c = 0; c < TBUFCOLS; c++) {
      b->lines[r][c].ch = ' ';
      b->lines[r][c].fg = CDEFAULT;
      b->lines[r][c].bg = CDEFAULT;
      b->lines[r][c].attrs = 0;
    }
  }
  b->row = 0;
  b->col = 0;
  b->scroll = 0;
  b->svrow = 0;
  b->svcol = 0;
  b->scrolltop = 0;
  b->scrollbot = 0;
}

void
tbufinit() {
  tbufclear(&tbufs[0]);
  tbufclear(&tbufs[1]);
  tbuf = &tbufs[0];
  tsm.sgrfg = CDEFAULT;
  tsm.sgrbg = CDEFAULT;
  tsm.sgrattrs = 0;
}


void
ptyinit() {
  int mfd, sfd;
  pid_t pid;
  char *shell, *args[2];
  mfd = posix_openpt(O_RDWR | O_NOCTTY);
  if (mfd < 0) { fprintf(stderr, "ERROR: posix_openpt failed!\n"); exit(1); }
  grantpt(mfd);
  unlockpt(mfd);
  sfd = open(ptsname(mfd), O_RDWR | O_NOCTTY);
  if (sfd < 0) { fprintf(stderr, "ERROR: sub-pty failed to open!\n"); exit(1); }
  pid = fork();
  if (pid < 0) { fprintf(stderr, "ERROR: fork() failed!\n"); exit(1); }
  if (pid == 0) {
    close(mfd);
    setsid();
    ioctl(sfd, TIOCSCTTY, 0);
    dup2(sfd, STDIN_FILENO);
    dup2(sfd, STDOUT_FILENO);
    dup2(sfd, STDERR_FILENO);
    close(sfd);
    shell = "/bin/bash";
    args[0] = shell;
    args[1] = NULL;
    execvp(shell, args);
    exit(1);
  }
  close(sfd);
  ptyfd = mfd;
  ptypid = pid;
  fcntl(ptyfd, F_SETFL, fcntl(ptyfd, F_GETFL) | O_NONBLOCK);
}

void
ptyresize() {
  struct winsize ws;
  ws.ws_row = (short unsigned int)visrows;
  ws.ws_col = (short unsigned int)(WWIDTH / (unsigned int)font->max_advance_width);
  ws.ws_xpixel = (short unsigned int)WWIDTH;
  ws.ws_ypixel = (short unsigned int)WHEIGHT;
  ioctl(ptyfd, TIOCSWINSZ, &ws);
}

void
ptywrite() {
}

void tsmcsi(char z);
void tsmproc(char c);

int
ptyread() {
  char buf[256];
  int i, n;
  n = (int)read(ptyfd, buf, sizeof(buf));
  if (n < 0 && errno == EIO) { return -1; }
  if (n <= 0) { return 0; }
  for (i = 0; i < n; i++) { tsmproc(buf[i]); }
  return 0;
}

void
ptykill() {
  kill(ptypid, SIGHUP);
  close(ptyfd);
}

void
tsminit() {
  tsm.state = TSMNORMAL;
  tsm.nparams = 0;
  tsm.crtparam = 0;
  tsm.hascurrent = 0;
  tsm.savecol = 0;
  tsm.saverow = 0;
  tsm.priv = 0;
  tsm.curshape = 0; tsm.curblink = 0;
  tsm.sgrfg = tsm.sgrbg = CDEFAULT;
  tsm.sgrattrs = 0;
  tsm.osclen = 0;
}

void
tsmcsi(char z) {
  int p, q, r, c, i;
  int t, b, nt, nm, v;
  if (tsm.hascurrent && tsm.nparams < TSMPARAMS) {
    tsm.params[tsm.nparams++] = tsm.crtparam;
  }
  p = tsm.nparams > 0 ? tsm.params[0] : 0;
  q = tsm.nparams > 1 ? tsm.params[1] : 0;
  switch (z) {
    case 'A':
      if (!p) { p = 1; }
      tbuf->row -= p;
      if (tbuf->row < tbuf->scroll) { tbuf->row = tbuf->scroll; }
      break;
    case 'B':
      if (!p) { p = 1; }
      tbuf->row += p;
      if (tbuf->row >= tbuf->scroll + visrows) { tbuf->row = tbuf->scroll + visrows - 1; }
      break;
    case 'C':
      if (!p) { p = 1; }
      tbuf->col += p;
      if (tbuf->col >= TBUFCOLS) { tbuf->col = TBUFCOLS - 1; }
      break;
    case 'D':
      if (!p) { p = 1; }
      tbuf->col -= p;
      if (tbuf->col < 0) { tbuf->col = 0; }
      break;
    case 'G': case '`': /* Cursor Horizontal Absolute */
      c = p ? p - 1 : 0;
      tbuf->col = c;
      if (tbuf->col >= TBUFCOLS) { tbuf->col = TBUFCOLS - 1; }
      break;
    case 'd': /* Line Position Absolute */
      r = p ? p - 1 : 0;
      tbuf->row = tbuf->scroll + r;
      if (tbuf->row >= tbuf->scroll + visrows) { tbuf->row = tbuf->scroll + visrows - 1; }
      break;
    case 'E': /* Cursor Next Line */
      if (!p) { p = 1; }
      tbuf->row += p;
      if (tbuf->row >= tbuf->scroll + visrows) { tbuf->row = tbuf->scroll + visrows - 1; }
      tbuf->col = 0;
      break;
    case 'F': /* Cursor Preceding Line */
      if (!p) { p = 1; }
      tbuf->row -= p;
      if (tbuf->row < tbuf->scroll) { tbuf->row = tbuf->scroll; }
      tbuf->col = 0;
      break;
    case 'H': case 'f':
      r = p ? p - 1 : 0;
      c = q ? q - 1 : 0;
      tbuf->row = tbuf->scroll + r;
      tbuf->col = c;
      if (tbuf->row >= tbuf->scroll + visrows) { tbuf->row = tbuf->scroll + visrows - 1; }
      if (tbuf->col >= TBUFCOLS) { tbuf->col = TBUFCOLS - 1; }
      break;
    case 'J':
      if (p == 0) {
        cellsetrow(&tbuf->lines[tbuf->row][tbuf->col], TBUFCOLS - tbuf->col);
        for (r = tbuf->row + 1; r < tbuf->scroll + visrows && r < TBUFROWS; r++) {
          cellsetrow(tbuf->lines[r], TBUFCOLS);
        }
      } else if (p == 1) {
        for (r = tbuf->scroll; r < tbuf->row && r < TBUFROWS; r++) {
          cellsetrow(tbuf->lines[r], TBUFCOLS);
        }
        cellsetrow(tbuf->lines[r], tbuf->col + 1);
      } else if (p == 2) {
        for (r = tbuf->scroll; r < tbuf->scroll + visrows && r < TBUFROWS; r++) {
          cellsetrow(tbuf->lines[r], TBUFCOLS);
        }
      }
      break;
    case 'K':
      if (p == 0) { cellsetrow(&tbuf->lines[tbuf->row][tbuf->col], viscols - tbuf->col); }
      else if (p == 1) { cellsetrow(tbuf->lines[tbuf->row], tbuf->col + 1); }
      else if (p == 2) { cellsetrow(tbuf->lines[tbuf->row], viscols); }
      break;
    case 'P': /* Delete Character */
      if (!p) { p = 1; }
      if (p > TBUFCOLS - tbuf->col) { p = TBUFCOLS - tbuf->col; }
      nm = TBUFCOLS - tbuf->col - p;
      if (nm > 0) {
        memmove(&tbuf->lines[tbuf->row][tbuf->col], &tbuf->lines[tbuf->row][tbuf->col + p], (long unsigned int)(nm) * sizeof(tbuf->lines[0][0])); 
      }
      cellsetrow(&tbuf->lines[tbuf->row][TBUFCOLS - p], p);
      break;
    case 'X': /* Erase Character */
      if (!p) { p = 1; }
      if (p > TBUFCOLS - tbuf->col) { p = TBUFCOLS - tbuf->col; }
      cellsetrow(&tbuf->lines[tbuf->row][tbuf->col], p);
      break;
    case '@': /* Insert Character */
      if (!p) { p = 1; }
      if (p > TBUFCOLS - tbuf->col) { p = TBUFCOLS - tbuf->col; }
      nm = TBUFCOLS - tbuf->col - p;
      if (nm > 0) {
        memmove(&tbuf->lines[tbuf->row][tbuf->col + p], &tbuf->lines[tbuf->row][tbuf->col], (long unsigned int)(nm) * sizeof(tbuf->lines[0][0])); 
      }
      cellsetrow(&tbuf->lines[tbuf->row][tbuf->col], p);
      break;
    case 'h':
      if (tsm.priv) {
        for (i = 0; i < tsm.nparams; i++) {
          if (tsm.params[i] == 1049 && tbuf == &tbufs[0]) {
            tbuf->svrow = tbuf->row; tbuf->svcol = tbuf->col;
            tbuf->svscroll = tbuf->scroll;
            tbufclear(&tbufs[1]);
            tbuf = &tbufs[1];
          } else if (tsm.params[i] == 1) { tsm.appkeys = 1; }
        }
      }
      break;
    case 'l':
      if (tsm.priv) {
        for (i = 0; i < tsm.nparams; i++) {
          if (tsm.params[i] == 1049 && tbuf == &tbufs[1]) {
            tbuf = &tbufs[0];
            tbuf->row = tbuf->svrow; tbuf->col = tbuf->svcol;
            tbuf->scroll = tbuf->svscroll;
          } else if (tsm.params[i] == 1) { tsm.appkeys = 0; }
        }
      }
      break;
    case 'm':
      /* SGR: no params = reset all */
      if (tsm.nparams == 0) {
        tsm.sgrfg = CDEFAULT; tsm.sgrbg = CDEFAULT; tsm.sgrattrs = 0;
        break;
      }
      i = 0;
      while (i < tsm.nparams) {
        v = tsm.params[i];
        if (v == 0)  { tsm.sgrfg = CDEFAULT; tsm.sgrbg = CDEFAULT; tsm.sgrattrs = 0; }
        else if (v == 1) { tsm.sgrattrs |=  ATTRBOLD; }
        else if (v == 2) { tsm.sgrattrs |=  ATTRDIM; }
        else if (v == 3) { tsm.sgrattrs |=  ATTRITALIC; }
        else if (v == 4) { tsm.sgrattrs |=  ATTRUNDER; }
        else if (v == 7) { tsm.sgrattrs |=  ATTRREVERSE; }
        else if (v == 22) { tsm.sgrattrs &= (unsigned char)~ATTRBOLD; }
        else if (v == 23) { tsm.sgrattrs &= (unsigned char)~ATTRITALIC; }
        else if (v == 24) { tsm.sgrattrs &= (unsigned char)~ATTRUNDER; }
        else if (v == 27) { tsm.sgrattrs &= (unsigned char)~ATTRREVERSE; }
        else if (v >= 30 && v <= 37) { tsm.sgrfg = (unsigned char)(v - 30); }
        else if (v == 38 && i + 2 < tsm.nparams && tsm.params[i+1] == 5) {
          tsm.sgrfg = (unsigned char)tsm.params[i+2]; i += 2;
        }
        else if (v == 39) { tsm.sgrfg = CDEFAULT; }
        else if (v >= 40 && v <= 47) { tsm.sgrbg = (unsigned char)(v - 40); }
        else if (v == 48 && i + 2 < tsm.nparams && tsm.params[i+1] == 5) {
          tsm.sgrbg = (unsigned char)tsm.params[i+2]; i += 2;
        }
        else if (v == 49) { tsm.sgrbg = CDEFAULT; }
        else if (v >= 90 && v <= 97) { tsm.sgrfg = (unsigned char)(v - 90 + 8); }
        else if (v >= 100 && v <= 107) { tsm.sgrbg = (unsigned char)(v - 100 + 8); }
        i++;
      }
      break;
    case 'q':
      if (tsm.priv) {
        tsm.curblink = (p == 0 || p == 1 || p == 3 || p == 5);
        if (p <= 2) { tsm.curshape = 0; }
        else if (p <= 4) { tsm.curshape = 1; }
        else { tsm.curshape = 2; }
      }
      break;
    case 'r':
      tbuf->scrolltop = p ? p - 1 : 0;
      tbuf->scrollbot = q ? q - 1 : visrows - 1;
      if (tbuf->scrollbot >= visrows) { tbuf->scrollbot = visrows - 1; }
      if (tbuf->scrolltop >= tbuf->scrollbot) { tbuf->scrolltop = 0; tbuf->scrollbot = visrows - 1; }
      tbuf->row = tbuf->scroll + tbuf->scrolltop;
      tbuf->col = 0;
      break;
    case 'L':
      if (!p) { p = 1; }
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nt = tbuf->row;
      nm = b - nt - p + 1;
      if (nm > 0) { memmove(tbuf->lines[nt + p], tbuf->lines[nt], (long unsigned int)nm * sizeof(tbuf->lines[0])); }
      for (r = nt; r < nt + p && r <= b; r++) { cellsetrow(tbuf->lines[r], TBUFCOLS); }
      break;
    case 'M':
      if (!p) { p = 1; }
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nt = tbuf->row;
      nm = b - nt - p + 1;
      if (nm > 0) { memmove(tbuf->lines[nt], tbuf->lines[nt + p], (long unsigned int)nm * sizeof(tbuf->lines[0])); }
      for (r = b - p + 1; r <= b; r++) { cellsetrow(tbuf->lines[r], TBUFCOLS); }
      break;
    case 'S':
      if (!p) { p = 1; }
      t = tbuf->scroll + tbuf->scrolltop;
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nm = b - t - p + 1;
      if (nm > 0) { memmove(tbuf->lines[t], tbuf->lines[t + p], (long unsigned int)nm * sizeof(tbuf->lines[0])); }
      for (r = b - p + 1; r <= b; r++) { cellsetrow(tbuf->lines[r], TBUFCOLS); }
      break;
    case 'T':
      if (!p) { p = 1; }
      t = tbuf->scroll + tbuf->scrolltop;
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nm = b - t - p + 1;
      if (nm > 0) { memmove(tbuf->lines[t + p], tbuf->lines[t], (long unsigned int)nm * sizeof(tbuf->lines[0])); }
      for (r = t; r < t + p; r++) { cellsetrow(tbuf->lines[r], TBUFCOLS); }
      break;
    case 's': /* Save cursor */
      tsm.savecol = tbuf->col;
      tsm.saverow = tbuf->row;
      break;
    case 'u': /* Restore cursor */
      tbuf->col = tsm.savecol;
      tbuf->row = tsm.saverow;
      break;
    default:
      break;
  }
}

static void
tbufindex() {
  int bot, top;
  top = tbuf->scroll + tbuf->scrolltop;
  bot = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
  if (tbuf->row == bot) {
    memmove(tbuf->lines[top], tbuf->lines[top + 1], (long unsigned int)(bot - top) * sizeof(tbuf->lines[0]));
    cellsetrow(tbuf->lines[bot], TBUFCOLS);
    screendirty = 1;
  } else {
    if (tbuf->row < TBUFROWS - 1) { tbuf->row++; }
    cellsetrow(tbuf->lines[tbuf->row], TBUFCOLS);
    if (!tbuf->scrollbot && tbuf->row >= tbuf->scroll + visrows) {
      tbuf->scroll = tbuf->row - visrows + 1;
      screendirty = 1;
    }
  }
}

static void
tbufrevindex () {
  int bot, top;
  top = tbuf->scroll + tbuf->scrolltop;
  bot = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
  if (tbuf->row == top) {
    memmove(tbuf->lines[top + 1], tbuf->lines[top], (long unsigned int)(bot - top) * sizeof(tbuf->lines[0]));
    cellsetrow(tbuf->lines[top], TBUFCOLS);
  } else {
    if (tbuf->row > tbuf->scroll) { tbuf->row--; }
  }
}

static void
tsmosc() {
  char *sep, *title;
  int cmd;
  sep = memchr(tsm.oscbuf, ';', (size_t)tsm.osclen);
  if (!sep) { tsm.osclen = 0; return; }
  *sep = '\0';
  cmd = atoi(tsm.oscbuf);
  title = sep+1;
  if (cmd == 0 || cmd == 2) {
    /* XStoreName covers legacy WM_NAME */
    XStoreName(display, window, title);
    /* _NET_WM_NAME for modern cases */
    XChangeProperty(display, window, xanetname, xautf8str,
        8, PropModeReplace, (unsigned char *)title, (int)strlen(title));
  }
  tsm.osclen = 0;
}

void
tsmproc(char c) {
  unsigned char uc;
  uc = (unsigned char)c;
  switch (tsm.state) {
    case TSMNORMAL:
      if (uc == 0x1B) { tsm.state = TSMESC; }
      else if (uc == 0x07) { /* TODO: BEL */ }
      else if (uc == '\b') { if (tbuf->col > 0) { tbuf->col--; } }
      else if (uc == '\t') {
        tbuf->col = (tbuf->col + 8) & ~7;
        if (tbuf->col >= TBUFCOLS) { tbuf->col = TBUFCOLS - 1; }
      }
      else if (uc == '\r') { tbuf->col = 0; }
      else if (uc == '\n') { tbufindex(); }
      else if (uc >= 0x20 && uc < 0x7F) {
        if (tbuf ->col >= viscols) {
          tbuf->col = 0; tbufindex();
        }
        tbuf->lines[tbuf->row][tbuf->col].ch = c;
        tbuf->lines[tbuf->row][tbuf->col].fg = tsm.sgrfg;
        tbuf->lines[tbuf->row][tbuf->col].bg = tsm.sgrbg;
        tbuf->lines[tbuf->row][tbuf->col].attrs = tsm.sgrattrs;
        tbuf->col++;
        if (tbuf->row >= tbuf->scroll + visrows) {
          tbuf->scroll = tbuf->row - visrows + 1;
        }
        screendirty = 1;
      }
      break;
    case TSMESC:
      if (c == '[') {
        tsm.state = TSMCSI;
        tsm.nparams = 0;
        tsm.crtparam = 0;
        tsm.hascurrent = 0;
        tsm.priv = 0;
      } else if (c == '(' || c == ')' || c == '*' || c == '+') {
        tsm.state = TSMCHARSEL;
      } else if (c == 'M') { /* reverse index */
        tbufrevindex();
        tsm.state = TSMNORMAL;
      } else if (c == 'D') { /* cursor down or advance */
        tbufindex();
        tsm.state = TSMNORMAL;
      } else if (c == 'E') { /* Next line CR */
        tbuf->col = 0;
        tbufindex();
        tsm.state = TSMNORMAL;
      } else if (c == '7') { /* Save cursor pos */
        tsm.savecol = tbuf->col;
        tsm.saverow = tbuf->row;
        tsm.state = TSMNORMAL;
      } else if (c == '8') { /* Restore cursor pos */
        tbuf->col = tsm.savecol;
        tbuf->row = tsm.saverow;
        tsm.state = TSMNORMAL;
      } else if (c == 'c') { /* full reset */
        tbufinit();
        tsminit();
      } else if (c == ']') { /* OSC, title BEL */
        tsm.state = TSMOSC;
      } else if (c == '=' || c == '>') {
        /* Application/normal keypad mode: skip for now */
        tsm.state = TSMNORMAL;
      } else { /* Unrecognized two-byte */
        tsm.state = TSMNORMAL;
      }
      break;
    case TSMOSC:
      if (uc == 0x07) {
        tsm.oscbuf[tsm.osclen] = '\0';
        tsmosc();
        tsm.state = TSMNORMAL;
      } else if (uc == 0x1B) {
        /* ESC \ is the alternative string terminator (ST);
         * process now, let TSMESC consume the trailing backslash */
        tsm.oscbuf[tsm.osclen] = '\0';
        tsmosc();
        tsm.state = TSMESC;
      } else if (tsm.osclen < (int)sizeof(tsm.oscbuf) - 1) {
        tsm.oscbuf[tsm.osclen++] = (char)uc;
      }
      break;
    case TSMCHARSEL:
      /* Drop first byte and discard */
      /* Vestigial from VT100 */
      /* TODO: DEC line-drawing (ESC ( 0) used by ncurses */
      tsm.state = TSMNORMAL;
      break;
    case TSMCSI:
      if (uc >= '0' && uc <= '9') {
        tsm.crtparam = tsm.crtparam * 10 + (uc - '0');
        tsm.hascurrent = 1;
      } else if (c == ';') {
        if (tsm.nparams < TSMPARAMS) { tsm.params[tsm.nparams++] = tsm.crtparam; }
        tsm.crtparam = 0;
        tsm.hascurrent = 0;
      } else if (c == '?') {
        tsm.priv = 1; /* private sequence */
      } else if (c == '>' || c == '|') {
        /* intermediate bytes: flag and keep collecting */
      } else if (uc >= 0x40 && uc <= 0x7E) {
        /* final byte: dispatch then reset */
        tsmcsi(c);
        tsm.state = TSMNORMAL;
        tsm.priv = 0;
      } else {
        /* malformed */
        tsm.state = TSMNORMAL;
      }
      break;
    default: break;
  }
}

static void
selcopytext(Time t) {
  int r, c, endcol, len, cap;
  char *buf, *p;
  Cell *cell;
  cap = (selrow2 - selrow1 + 1) * (viscols + 1) + 1;
  buf = calloc((size_t)cap, sizeof(size_t));
  if (!buf) { return; }
  p = buf;
  for (r = selrow1; r <= selrow2; r++) {
    endcol = (r == selrow2) ? selcol2 : viscols - 1;
    /* strip trailing spaces */
    while (endcol > 0 && tbuf->lines[r][endcol].ch == ' ') { endcol--; }
    c = (r == selrow1) ? selcol1 : 0;
    for (; c <= endcol; c++) {
      cell = &tbuf->lines[r][c];
      *p++ = cell->ch ? cell->ch : ' ';
    }
    if (r < selrow2) { *p++ = '\n'; }
  }
  *p = '\0';
  len = (int)(p - buf);
  clipcopy(buf, len, t);
  free(buf);
}

static void
pixeltocell(int px, int py, int *col, int *row) {
  int cw, ch;
  cw = font->max_advance_width;
  ch = font->ascent + font->descent;
  *col = px / cw;
  *row = tbuf->scroll + py / ch;
  if (*col < 0) { *col = 0; }
  if (*col >= viscols) { *col = viscols - 1; }
  if (*row < tbuf->scroll) { *row = tbuf->scroll; }
  if (*row >= tbuf->scroll + visrows) { *row = tbuf->scroll + visrows - 1; }
}

#define SELSCROLLZONE 20
int
main(int argc, char *argv[]) {
  XEvent ev, reply;
  int quit, xfd, r, len, maxscroll, darkth, sel;
  int cw, ch, cx, cy, crow, ccol, reverse, cidx, ncol;
  fd_set fds;
  struct timeval tv;
  long long remaining;
  char buf[8];
  KeySym ks;
  Cell *curcell;
  double segf, tpos, bright;
  XftColor *cfg, *cbg, throb;
  XSelectionRequestEvent *rq;
  Atom supported[2], type;
  int fmt;
  unsigned long ni, after;
  unsigned char *data;
  Cell *cell;
  XftColor *nfg, *nbg, *nrfg, *nrbg, *ntmp;
  int mcol, mrow, rrow, rcol;
  int newcol, newrow;
  int incell; /* westfallen */
  darkth = detectdark();
  toggletheme = 0;
  fontsize = 13;
  if (darkth < 0) {
    fprintf(stderr, "Error when reading theme files!\n");
  } else if (darkth) { isdark = 1; } else { isdark = 0; }
  signal(SIGUSR1, handlesigusr1);
  x11init();
  fontinit();
  colorsinit();
  drawinit();
  tbufinit();
  tsminit();
  ptyinit();
  ptyresize();
  UNUSED(argc); UNUSED(argv);
  visrows = (int)WHEIGHT / (font->ascent + font->descent);
  viscols = (int)WWIDTH / font->max_advance_width;
  quit = 0;
  throbcsr = 0;
  GETNS(thenr); GETNS(nowr);
  while (!quit) {
    if (toggletheme) {
      toggletheme = 0;
      isdark = !isdark;
      applycolors();
    }
    while (XPending(display) > 0) {
      XNextEvent(display, &ev);
      switch (ev.type) {
        case ConfigureNotify:
          if (ev.xconfigure.width != (int)WWIDTH ||
              ev.xconfigure.height != (int)WHEIGHT) {
            WWIDTH = (unsigned int)ev.xconfigure.width;
            WHEIGHT = (unsigned int)ev.xconfigure.height;
            visrows = (int)WHEIGHT / (font->ascent + font->descent);
            viscols = (int)WWIDTH / font->max_advance_width;
            for (r = 0; r < 2; r++) {
              tbufs[r].scrolltop = 0;
              tbufs[r].scrollbot = 0;
              if (tbufs[r].row >= tbufs[r].scroll + visrows) {
                tbufs[r].row = tbufs[r].scroll + visrows - 1;
              }
            }
            screendirty = 1;
            drawresize();
            ptyresize();
          }
          break;
        case SelectionRequest:
          rq = &ev.xselectionrequest;
          reply.xselection.type = SelectionNotify;
          reply.xselection.display = rq->display;
          reply.xselection.requestor = rq->requestor;
          reply.xselection.selection = rq->selection;
          reply.xselection.target = rq->target;
          reply.xselection.time = rq->time;
          reply.xselection.property = None; /* default: refuse */
          if (rq->target == xatargets) {
            supported[0] = xautf8str;
            supported[1] = XA_STRING;
            XChangeProperty(rq->display, rq->requestor, rq->property,
                XA_ATOM, 32, PropModeReplace,
                (unsigned char *)supported, 2);
	    reply.xselection.property = rq->property;
          } else if ((rq->target == xautf8str || rq->target == XA_STRING)
                    && cliptext && cliptextsz > 0) {
            XChangeProperty(rq->display, rq->requestor, rq->property,
                rq->target, 8, PropModeReplace,
                (unsigned char *) cliptext, (int)cliptextsz);
            reply.xselection.property = rq->property;
          }
          XSendEvent(rq->display, rq->requestor, false, 0, &reply);
          break;
        case SelectionNotify:
          if (ev.xselection.property == None) { break; }
          if (XGetWindowProperty(display, window, xaseldata,
              0, (1 << 20), true, AnyPropertyType,
              &type, &fmt, &ni, &after, &data) == Success && data) {
            /* XXX: https://stackoverflow.com/questions/40576003/ignoring-warning-wunused-result */
            /* >That (void) alone isn't enough is on purpose */
            (void)!write(ptyfd, data, (size_t)ni);
            XFree(data);
          }
          break;
        case MotionNotify: {
            if (!selactive) { break; }
            pixeltocell(ev.xmotion.x, ev.xmotion.y, &mcol, &mrow);
            selmousex = ev.xmotion.x;
            selmousey = ev.xmotion.y;
            if (ev.xmotion.y < SELSCROLLZONE) {
              selscrolldir = -1; /* scroll up */
            } else if (ev.xmotion.y >= (int)WHEIGHT - SELSCROLLZONE) {
              selscrolldir = 1; /* scroll down */
            } else { selscrolldir = 0; }
            if (mrow < selancrow || (mrow == selancrow && mcol < selanccol)) {
              selrow1 = mrow; selcol1 = mcol;
              selrow2 = selancrow; selcol2 = selanccol;
            } else {
              selrow1 = selancrow; selcol1 = selanccol;
              selrow2 = mrow; selcol2 = mcol;
            }
            selexists = 1;
            screendirty = 1;
          }
          break;
        case ButtonRelease : {
            if (ev.xbutton.button != Button1 || !selactive) { break; }
            selactive = 0;
            pixeltocell(ev.xbutton.x, ev.xbutton.y, &rcol, &rrow);
            if (rrow < selancrow || (rrow == selancrow && rcol < selanccol)) {
              selrow1 = rrow; selcol1 = rcol;
              selrow2 = selancrow; selcol2 = selanccol;
            } else {
              selrow1 = selancrow; selcol1 = selanccol;
              selrow2 = rrow; selcol2 = rcol;
            }
            if (selrow1 == selrow2 && selcol1 == selcol2) {
              selexists = 0;
            } else { selexists = 1; selcopytext(ev.xbutton.time); }
            screendirty = 1;
            selscrolldir = 0;
            selscrolltick = 0;
          }
          break;
        case KeyPress: {
            len = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_Prior) {
              if (ev.xkey.state & ShiftMask) {
                tbuf->scroll -= visrows;
                if (tbuf->scroll < 0) { tbuf->scroll = 0; }
              } else {
                (void)!write(ptyfd, "\033[5~", 4);
              }
            } else if (ks == XK_Next) {
              if (ev.xkey.state & ShiftMask) {
                maxscroll = tbuf->row - visrows + 1;
                if (maxscroll < 0) { maxscroll = 0; }
                tbuf->scroll += visrows;
                if (tbuf->scroll > maxscroll) { tbuf->scroll = maxscroll; }
              } else {
                (void)!write(ptyfd, "\033[6~", 4);
              }
            } else if (ks == XK_Up) {
              (void)!write(ptyfd, tsm.appkeys ? "\033OA" : "\033[A", 3);
            } else if (ks == XK_Down) {
              (void)!write(ptyfd, tsm.appkeys ? "\033OB" : "\033[B", 3);
            } else if (ks == XK_Right) {
              (void)!write(ptyfd, tsm.appkeys ? "\033OC" : "\033[C", 3);
            } else if (ks == XK_Left) {
              (void)!write(ptyfd, tsm.appkeys ? "\033OD" : "\033[D", 3);
            } else if (ks == XK_Home) {
              /* (void)!write(ptyfd, tsm.appkeys ? "\033OH" : "\033[1~", 4); */
              (void)!write(ptyfd, "\033[1~", 4);
            } else if (ks == XK_End) {
              /* (void)!write(ptyfd, tsm.appkeys ? "\033OF" : "\033[4~", 4); */
              (void)!write(ptyfd, "\033[4~", 4);
            }else if (ks == XK_Delete) {
              (void)!write(ptyfd, "\033[3~", 4);
            } else if (ks == XK_Insert) {
              (void)!write(ptyfd, "\033[2~", 4);
            }
            else if (ev.xkey.state & Mod1Mask) {
              if (ev.xkey.state & ShiftMask) {
                if (ks == XK_j || ks == XK_J) { changefontsz(-1); }
                else if (ks == XK_k || ks == XK_K) { changefontsz(+1); }
                else if (ks == XK_t || ks == XK_T) {
                  throbcsr = !throbcsr;
                  if (throbcsr) { throbphase = 0.0; }
                }
              }
              else if (ks == XK_c) {
                if (selexists) { selcopytext(ev.xkey.time); }
              }
              else if (ks == XK_v) { clippaste(ev.xkey.time); }
            }
            else if (ks == XK_F1) { (void)!write(ptyfd, "\033OP", 3); }
            else if (ks == XK_F2) { (void)!write(ptyfd, "\033OQ", 3); }
            else if (ks == XK_F3) { (void)!write(ptyfd, "\033OR", 3); }
            else if (ks == XK_F4) { (void)!write(ptyfd, "\033OS", 3); }
            else if (ks == XK_F5) { (void)!write(ptyfd, "\033[15~", 5); }
            else if (ks == XK_F6) { (void)!write(ptyfd, "\033[17~", 5); }
            else if (ks == XK_F7) { (void)!write(ptyfd, "\033[18~", 5); }
            else if (ks == XK_F8) { (void)!write(ptyfd, "\033[19~", 5); }
            else if (ks == XK_F9) { (void)!write(ptyfd, "\033[20~", 5); }
            else if (ks == XK_F10) { (void)!write(ptyfd, "\033[21~", 5); }
            else if (ks == XK_F11) { (void)!write(ptyfd, "\033[23~", 5); }
            else if (ks == XK_F12) { (void)!write(ptyfd, "\033[24~", 5); }
            else if (len > 0) {
              (void)!write(ptyfd, buf, (size_t)len);
            }
          }
          break;
        case ButtonPress: {
            if (ev.xbutton.button == Button4) {
              tbuf->scroll -= 3;
              if (tbuf->scroll < 0) { tbuf->scroll = 0; }
            } else if (ev.xbutton.button == Button5) {
              maxscroll = tbuf->row - visrows + 1;
              if (maxscroll < 0) { maxscroll = 0; }
              tbuf->scroll += 3;
              if (tbuf->scroll > maxscroll) { tbuf->scroll = maxscroll; }
            } else if (ev.xbutton.button == Button1) {
              if ((ev.xbutton.state & ShiftMask) && selexists) {
                pixeltocell(ev.xbutton.x, ev.xbutton.y, &newcol, &newrow);
                if (newrow < selancrow ||
                    (newrow == selancrow && newcol < selanccol)) {
                  selrow1 = newrow; selcol1 = newcol;
                  selrow2 = selancrow; selcol2 = selanccol;
                } else {
                  selrow1 = selancrow; selcol1 = selanccol;
                  selrow2 = newrow; selcol2 = newcol;
                }
                selexists = 1;
                selcopytext(ev.xbutton.time);
                screendirty = 1;
              } else {
                pixeltocell(ev.xbutton.x, ev.xbutton.y, &selanccol, &selancrow);
                selrow1 = selrow2 = selancrow;
                selcol1 = selcol2 = selanccol;
                selactive = 1;
                selexists = 0;
                screendirty = 1;
              }
            }
          }
          break;
        case ClientMessage:
          if ((Atom) ev.xclient.data.l[0] == wmdelwin) { quit = 1; }
          break;
        default:
          break;
      }
    }
    GETNS(nowr);
    remaining = GFXTICKNS - DIFFNS(thenr, nowr);
    if (remaining < 0) { remaining = 0; }
    xfd = ConnectionNumber(display);
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    FD_SET(ptyfd, &fds);
    tv.tv_sec = remaining / 1000000000LL;
    tv.tv_usec = (remaining % 1000000000LL) / 1000LL;
    sel = select((ptyfd > xfd ? ptyfd : xfd) + 1, &fds, NULL, NULL, &tv);
    GETNS(nowr);
    if (sel > 0 && FD_ISSET(ptyfd, &fds)) {
      if (ptyread() < 0) { quit = 1; }
    }
    if (DIFFNS(thenr, nowr) >= GFXTICKNS) {
      GETNS(thenr);
      cw = font->max_advance_width;
      ch = font->ascent + font->descent;
      crow = tbuf->row - tbuf->scroll;
      ccol = tbuf->col;
      cx = ccol * cw;
      cy = crow * ch;
      if (selactive && selscrolldir != 0) {
        selscrolltick++;
        if (selscrolltick >= 3) { /* rate limit -20rows/sec at 60fps */
          selscrolltick = 0;
          if (selscrolldir < 0) {
            tbuf->scroll--;
            if (tbuf->scroll < 0) { tbuf->scroll = 0; }
          } else {
            maxscroll = tbuf->row - visrows + 1;
            if (maxscroll < 0) { maxscroll = 0; }
            tbuf->scroll++;
            if (tbuf->scroll > maxscroll) { tbuf->scroll = maxscroll; }
          }
          /* XXX: rcol/rrow: they're only used inside the ButtonRelease case
             so there's no actual conflict. */
          pixeltocell(selmousex, selmousey, &rcol, &rrow);
          if (rrow < selancrow ||
              (rrow == selancrow && rcol < selanccol)) {
            selrow1 = rrow; selcol1 = rcol;
            selrow2 = selancrow; selcol2 = selanccol;
          } else {
            selrow1 = selancrow; selcol1 = selanccol;
            selrow2 = rrow; selcol2 = rcol;
          }
          selexists = 1;
          screendirty = 1;
        }
      }
      if (screendirty) {
        screendirty = 0;
        XftDrawRect(xftdraw, &colorbg, 0, 0, WWIDTH, WHEIGHT);
        for (r = 0; r < visrows && (tbuf->scroll + r) < TBUFROWS; r++) {
          for (ncol = 0; ncol < viscols; ncol++) {
            cell = &tbuf->lines[tbuf->scroll + r][ncol];
            nfg = cellcolor(cell->fg, 1);
            nbg = cellcolor(cell->bg, 0);
            if (cell->attrs & ATTRREVERSE) {
              ntmp = nfg; nfg = nbg; nbg = ntmp;
            }
            incell = selexists &&
              ((r + tbuf->scroll) > selrow1 ||
               ((r + tbuf->scroll) == selrow1 && ncol >= selcol1)) &&
              ((r + tbuf->scroll) < selrow2 ||
               ((r + tbuf->scroll) == selrow2 && ncol <= selcol2));
            if (incell) {
              ntmp = nfg; nfg = nbg; nbg = ntmp; /* swap fg/bg for hilight */
            }
            drawcell(ncol, r, cell, nfg, nbg);
          }
        }
      }
      if (crow >= 0 && crow < visrows) {
        curcell = &tbuf->lines[tbuf->row][ccol];
        reverse = curcell->attrs & ATTRREVERSE;
        nrfg = cellcolor(curcell->fg, 1);
        nrbg = cellcolor(curcell->bg, 0);
        if (reverse) { ntmp = nrfg; nrfg = nrbg; nrbg = ntmp; }
        drawcell(ccol, crow, curcell, nrfg, nrbg);
        if (throbcsr) {
          throbphase += 1.0/180;
          if (throbphase >= 1.0) { throbphase -= 1.0; }
          segf = throbphase * NRAINBOW;
          cidx = ((int)segf % NRAINBOW);
          tpos = segf - (int)segf;
          bright = 0.5 + 0.5 * sin(tpos * 2.0 * M_PI);
          blendcolor(&throb, &colorbg, &throbpalette[cidx], bright);
          cfg = &colorfg;
          cbg = &throb;
        } else {
          cfg = reverse ? &cursorfgrev : &cursorfgclr;
          cbg = reverse ? &cursorbgrev : &cursorbgclr;
        }
        if (tsm.curshape == 1) {
          XftDrawRect(xftdraw, cbg, cx, cy + ch - 2, (unsigned int)cw, 2);
        } else if (tsm.curshape == 2) {
          XftDrawRect(xftdraw, cbg, cx, cy, 2, (unsigned int)ch);
        } else {
          drawcell(ccol, crow, curcell, cfg, cbg);
        }
        drawflush();
      }
    }
  }
  ptykill();
  drawkill();
  killcolors();
  fontkill();
  x11kill();
  return 0;
}
