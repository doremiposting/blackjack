#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include <errno.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "main.h"

Display *display;
Window window;
XWindowAttributes wa = {0};
GC gc;
Atom wmdelwin;
XftFont *font;
Pixmap pixmap;
XftDraw *xftdraw;
XftColor colorfg, colorbg;
Visual *vis;
Colormap cmap;
int ptyfd;
pid_t ptypid;
int visrows;
volatile sig_atomic_t toggletheme;
int isdark;

#define TBUFCOLS 256
/* #define TBUFROWS 128 */
#define TBUFROWS 8196
typedef struct {
  char lines[TBUFROWS][TBUFCOLS];
  int col, row;
  int scroll;
  int svrow, svcol;
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
} Tsm;
Tsm tsm;

typedef struct {
  const char *black, *brblack;
  const char *red, *brred;
  const char *green, *brgreen;
  const char *yellow, *bryellow;
  const char *blue, *brblue;
  const char *magenta, *magenta;
  const char *cyan, *brcyan;
  const char *white, *brwhite;
  char **slushclrs;
  const char *fg, *bg;
  const char *cursorgf, *cursorbg;
  const char *rcursorfg, *rcursorbg;
} Colorscheme;
static const Colorscheme darksch = {
  .black     = "#15161e", .brblack   = "#414868",
  .red       = "#f7768e", .brred     = "#f7768e",
  .green     = "#9ece6a", .brgreen   = "#9ece6a",
  .yellow    = "#e0af68", .bryellow  = "#e0af68",
  .blue      = "#7aa2f7", .brblue    = "#7aa2f7",
  .magenta   = "#bb9af7", .brmagenta = "#bb9af7",
  .cyan      = "#7dcfff", .brcyan    = "#7dcfff",
  .white     = "#a9b1d6", .brwhite   = "#c0caf5",
  .slushclrs = (void *)0;
  .fg        = "#c0caf5", .bg        = "#1a1b26",
  .cursorfg  = "#1a1b26", .cursorbg  = "#c0caf5",
  .rcursorfg = "#c0caf5", .rcursorbg = "#1a1b26",
};

static const Colorscheme lightsch = {
  .black     = "#000000", .brblack   = "#444444",
  .red       = "#cc0000", .brred     = "#ef2929",
  .green     = "#4e9a06", .brgreen   = "#8ae234",
  .yellow    = "#c4a000", .bryellow  = "#fce94f",
  .blue      = "#3465a4", .brblue    = "#729fcf",
  .magenta   = "#75507b", .brmagenta = "#ad7fa8",
  .cyan      = "#06989a", .brcyan    = "#34e2e2",
  .white     = "#d3d7cf", .brwhite   = "#eeeeec",
  .slushclrs = (void *)0,
  .fg        = "#000000", .bg        = "#6495ed",
  .cursorfg  = "#6495ed", .cursorbg  = "#000000",
  .rcursorfg = "#000000", .rcursorbg = "#6495ed",
};

const Colorscheme *clrs;


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
  XSetWMProtocols(display, window, &wmdelwin, 1);
  XSelectInput(display, window, KeyPressMask|PointerMotionMask|StructureNotifyMask|ButtonPressMask);
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
  /* TODO: Pull font name out into config.h */
  font = XftFontOpenName(display, DefaultScreen(display), "monospace:size=13");
  if (!font) { fprintf(stderr, "ERROR: Couldn't open font!\n"); exit(1); }
}

void
fontkill() {
  XftFontClose(display, font);
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
  static int inited = 0;
  if (inited) {
    XftColorFree(display, vis, cmap, &colorfg);
    XftColorFree(display, vis, cmap, &colorbg);
  }
  inited = 1;
  if (isdark) {
    clrs = &darksch;
  } else {
    clrs = &lightsch;
  }
  XftColorAllocName(display, vis, cmap, clrs->fg, &colorfg);
  XftColorAllocName(display, vis, cmap, clrs->bg, &colorbg);
}

static void
handlesigusr1(int sig) {
  UNUSED(sig);
  toggletheme = 1;
} 

void
killcolors() {
  XftColorFree(display, vis, cmap, &colorfg);
  XftColorFree(display, vis, cmap, &colorbg);
}

void
drawinit() {
  pixmap = XCreatePixmap(display, window, WWIDTH, WHEIGHT, wa.depth);
  xftdraw = XftDrawCreate(display, pixmap, vis, cmap);
}

void
drawkill() {
  XftDrawDestroy(xftdraw);
  XFreePixmap(display, pixmap);
}

void
drawcell(int col, int row, const char *str, size_t len,
    XftColor *fg, XftColor *bg) {
  int cw, ch, x, y;
  cw = font->max_advance_width;
  ch = font->ascent + font->descent;
  x = col * cw;
  y = row * ch;
  XftDrawRect(xftdraw, bg, x, y, cw * (int)len, ch);
  XftDrawStringUtf8(xftdraw, fg, font,
      x, y + font->ascent,
      (FcChar8 *)str, (int)len);
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
  pixmap = XCreatePixmap(display, window, WWIDTH, WHEIGHT, wa.depth);
  xftdraw = XftDrawCreate(display, pixmap, vis, cmap);
}

void
tbufclear(Termbuf *b) {
  int r, c;
  for (r = 0; r < TBUFROWS; r++) {
    for (c = 0; c < TBUFCOLS; c++) {
      b->lines[r][c] = ' ';
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
  ws.ws_row = visrows;
  ws.ws_col = WWIDTH / font->max_advance_width;
  ws.ws_xpixel = WWIDTH;
  ws.ws_ypixel = WHEIGHT;
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
  n = read(ptyfd, buf, sizeof(buf));
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
}

void
tsmcsi(char z) {
  int p, q, r, c, i;
  int t, b, nt, nm;
  if (tsm.hascurrent && tsm.nparams < TSMPARAMS) {
    tsm.params[tsm.nparams++] = tsm.crtparam;
  }
  p = tsm.nparams > 0 ? tsm.params[0] : 0;
  q = tsm.nparams > 1 ? tsm.params[1] : 0;
  switch (z) {
    case 'A': /* cursor up */
      if (!p) { p = 1; }
      tbuf->row -= p;
      if (tbuf->row < tbuf->scroll) { tbuf->row = tbuf->scroll; }
      break;
    case 'B': /* cursor down */
      if (!p) { p = 1; }
      tbuf->row += p;
      if (tbuf->row >= tbuf->scroll + visrows) { tbuf->row = tbuf->scroll + visrows - 1; }
      break;
    case 'C': /* cursor right */
      if (!p) { p = 1; }
      tbuf->col += p;
      if (tbuf->col >= TBUFCOLS) { tbuf->col = TBUFCOLS - 1; }
      break;
    case 'D': /* cursor left */
      if (!p) { p = 1; }
      tbuf->col -= p;
      if (tbuf->col < 0) { tbuf->col = 0; }
      break;
    case 'H': case 'f': /* cursor to row/col (1-based) */
      r = p ? p - 1 : 0;
      c = q ? q - 1 : 0;
      tbuf->row = tbuf->scroll + r;
      tbuf->col = c;
      if (tbuf->row >= tbuf->scroll + visrows) { tbuf->row = tbuf->scroll + visrows - 1; }
      if (tbuf->col >= TBUFCOLS) { tbuf->col = TBUFCOLS - 1; }
      break;
    case 'J': /* erase in display */
      if (p == 0) { /* cursor to end */
        memset(&tbuf->lines[tbuf->row][tbuf->col], ' ', TBUFCOLS - tbuf->col);
        for (r = tbuf->row + 1; r < tbuf->scroll + visrows && r < TBUFROWS; r++) {
          memset(tbuf->lines[r], ' ', TBUFCOLS);
        }
      } else if (p == 1) { /* start to cursor */
        for (r = tbuf->scroll; r < tbuf->row && r < TBUFROWS; r++) {
          memset(tbuf->lines[r], ' ', TBUFCOLS);
        }
        memset(tbuf->lines[r], ' ', tbuf->col + 1);
      } else if (p == 2) { /* whole screen */
        for (r = tbuf->scroll; r < tbuf->scroll + visrows && r < TBUFROWS; r++) {
          memset(tbuf->lines[r], ' ', TBUFCOLS);
        }
      }
      break;
    case 'K': /* erase in line */
      if (p == 0) { memset (&tbuf->lines[tbuf->row][tbuf->col], ' ', TBUFCOLS - tbuf->col); }
      else if (p == 1) { memset(tbuf->lines[tbuf->row], ' ', tbuf->col + 1); }
      else if (p == 2) { memset(tbuf->lines[tbuf->row], ' ', TBUFCOLS); }
      break;
    case 'h':
      if (tsm.priv) {
        for (i = 0; i < tsm.nparams; i++) {
          if (tsm.params[i] == 1049 && tbuf == &tbufs[0]) {
            tbuf->svrow = tbuf->row;
            tbuf->svcol = tbuf->col;
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
            tbuf->row = tbuf->svrow;
            tbuf->col = tbuf->svcol;
          } else if (tsm.params[i] == 1) { tsm.appkeys = 0; }
        }
      }
      break;
    case 'q':
      if (tsm.priv) {
        tsm.curblink = (p == 0 || p == 1 || p == 3 || p == 5);
        if (p <= 2) { tsm.curshape = 0; } /* block */
        else if (p <= 4) { tsm.curshape = 1; } /* underline */
        else { tsm.curshape = 2; } /* bar */
      }
      break;
    case 'r': /* set scrolling region */
      tbuf->scrolltop = p ? p-1 : 0;
      tbuf->scrollbot = q ? q-1 : visrows - 1;
      /* clamp to 2-row region and ensure within screen */
      if (tbuf->scrollbot >= visrows) { tbuf->scrollbot = visrows - 1; }
      if (tbuf->scrolltop >= tbuf->scrollbot) { tbuf->scrolltop = 0; tbuf->scrollbot = visrows - 1; }
      tbuf->row = tbuf->scroll + tbuf->scrolltop;
      tbuf->col = 0;
      break;
    case 'L': /* insert lines, push lines at cursor down */
      if (!p) { p = 1; }
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nt = tbuf->row;
      nm = b - nt - p + 1;
      if (nm > 0) {
        memmove(tbuf->lines[nt + p], tbuf->lines[nt],
            nm * TBUFCOLS);
      }
      for (r = nt; r < nt + p && r <= b; r++) {
        memset(tbuf->lines[r], ' ', TBUFCOLS);
      }
      break;
    case 'M': /* delete lines, pull lines up to cursor */
      if (!p) { p = 1; }
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nt = tbuf->row;
      nm = b - nt - p + 1;
      if (nm > 0) {
        memmove(tbuf->lines[nt], tbuf->lines[nt + p],
            nm * TBUFCOLS);
      }
      for (r = b - p + 1; r <= b; r++) {
        memset(tbuf->lines[r], ' ', TBUFCOLS);
      }
      break;
    case 'S': /* scroll up, shift region by p */
      if (!p) { p = 1; }
      t = tbuf->scroll + tbuf->scrolltop;
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nm = b - t - p + 1;
      if (nm > 0) { memmove(tbuf->lines[t], tbuf->lines[t+p], nm * TBUFCOLS); }
      for (r = b - p + 1; r <= b; r++) { memset(tbuf->lines[r], ' ', TBUFCOLS); }
      break;
    case 'T': /* scroll down, shift region by p */
      if (!p) { p = 1; }
      t = tbuf->scroll + tbuf->scrolltop;
      b = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
      nm = b - t - p + 1;
      if (nm > 0) { memmove(tbuf->lines[t+p], tbuf->lines[t], nm * TBUFCOLS); }
      for (r = t; r < t + p; r++) { memset(tbuf->lines[r], ' ', TBUFCOLS); }
      break;
    default: /* SGR (m), mode set/reset (h/l), and miscellaneous */
      break;
  }
}

static void
tbufindex() {
  int bot, top;
  top = tbuf->scroll + tbuf->scrolltop;
  bot = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
  if (tbuf->row == bot) {
    memmove(tbuf->lines[top], tbuf->lines[top+1], (bot - top) * TBUFCOLS);
    memset(tbuf->lines[bot], ' ', TBUFCOLS);
  } else {
    if (tbuf->row < TBUFROWS - 1) { tbuf->row++; }
    if (!tbuf->scrollbot && tbuf->row >= tbuf->scroll + visrows) {
      tbuf->scroll = tbuf->row - visrows + 1;
    }
  }
}

static void
tbufrevindex () {
  int bot, top;
  top = tbuf->scroll + tbuf->scrolltop;
  bot = tbuf->scroll + (tbuf->scrollbot ? tbuf->scrollbot : visrows - 1);
  if (tbuf->row == top) {
    memmove(tbuf->lines[top+1], tbuf->lines[top], (bot - top) * TBUFCOLS);
    memset(tbuf->lines[top], ' ', TBUFCOLS);
  } else { if (tbuf->row > tbuf->scroll) { tbuf->row--; }}
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
        if (tbuf->col < TBUFCOLS - 1) {
          tbuf->lines[tbuf->row][tbuf->col] = c;
          tbuf->col++;
          if (tbuf->row >= tbuf->scroll + visrows) {
            tbuf->scroll = tbuf->row - visrows + 1;
          }
        }
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
      /* TODO: Return to TSMESC for now to reset cleanly. */
      if (uc == 0x07) { tsm.state = TSMNORMAL; }
      else if (uc == 0x1B) { tsm.state = TSMESC; }
      /* TODO: Silently consume rest of the payload */
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

int
main(int argc, char *argv[]) {
  XEvent ev;
  int quit, xfd, r, len, maxscroll, darkth, sel;
  int cw, ch, cx, cy, crow, ccol;
  fd_set fds;
  struct timeval tv;
  long long remaining;
  char buf[8];
  KeySym ks;
  toggletheme = 0;
  darkth = detectdark();
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
  visrows = WHEIGHT / (font->ascent + font->descent);
  quit = 0;
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
            WWIDTH = ev.xconfigure.width;
            WHEIGHT = ev.xconfigure.height;
            visrows = WHEIGHT / (font->ascent + font->descent);
            drawresize();
            ptyresize();
          }
          break;
        case KeyPress: {
            len = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_Prior) {
              if (ev.xkey.state & ShiftMask) {
                tbuf->scroll -= visrows;
                if (tbuf->scroll < 0) { tbuf->scroll = 0; }
              } else {
                write (ptyfd, "\033[5~", 4);
              }
            } else if (ks == XK_Next) {
              if (ev.xkey.state & ShiftMask) {
                maxscroll = tbuf->row - visrows + 1;
                if (maxscroll < 0) { maxscroll = 0; }
                tbuf->scroll += visrows;
                if (tbuf->scroll > maxscroll) { tbuf->scroll = maxscroll; }
              } else {
                write(ptyfd, "\033[6~", 4);
              }
            } else if (ks == XK_Up) {
              write(ptyfd, tsm.appkeys ? "\033OA" : "\033[A", 3);
            } else if (ks == XK_Down) {
              write(ptyfd, tsm.appkeys ? "\033OB" : "\033[B", 3);
            } else if (ks == XK_Right) {
              write(ptyfd, tsm.appkeys ? "\033OC" : "\033[C", 3);
            } else if (ks == XK_Left) {
              write(ptyfd, tsm.appkeys ? "\033OD" : "\033[D", 3);
            } else if (ks == XK_Home) {
              /* write(ptyfd, tsm.appkeys ? "\033OH" : "\033[1~", 4); */
              write(ptyfd, "\033[1~", 4);
            } else if (ks == XK_End) {
              /* write(ptyfd, tsm.appkeys ? "\033OF" : "\033[4~", 4); */
              write(ptyfd, "\033[4~", 4);
            }else if (ks == XK_Delete) {
              write(ptyfd, "\033[3~", 4);
            } else if (ks == XK_Insert) {
              write(ptyfd, "\033[2~", 4);
            }
            else if (ks == XK_F1) { write(ptyfd, "\033OP", 3); }
            else if (ks == XK_F2) { write(ptyfd, "\033OQ", 3); }
            else if (ks == XK_F3) { write(ptyfd, "\033OR", 3); }
            else if (ks == XK_F4) { write(ptyfd, "\033OS", 3); }
            else if (ks == XK_F5) { write(ptyfd, "\033[15~", 5); }
            else if (ks == XK_F6) { write(ptyfd, "\033[17~", 5); }
            else if (ks == XK_F7) { write(ptyfd, "\033[18~", 5); }
            else if (ks == XK_F8) { write(ptyfd, "\033[19~", 5); }
            else if (ks == XK_F9) { write(ptyfd, "\033[20~", 5); }
            else if (ks == XK_F10) { write(ptyfd, "\033[21~", 5); }
            else if (ks == XK_F11) { write(ptyfd, "\033[23~", 5); }
            else if (ks == XK_F12) { write(ptyfd, "\033[24~", 5); }
            else if (len > 0) {
              write(ptyfd, buf, len);
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
      XftDrawRect(xftdraw, &colorbg, 0, 0, WWIDTH, WHEIGHT);
      for (r = 0; r < visrows && (tbuf->scroll + r) < TBUFROWS; r++) {
        drawcell(0, r, tbuf->lines[tbuf->scroll + r], TBUFCOLS, &colorfg, &colorbg);
      }
      cw = font->max_advance_width;
      ch = font->ascent + font->descent;
      crow = tbuf->row - tbuf->scroll;
      ccol = tbuf->col;
      cx = ccol * cw;
      cy = crow * ch;
      if (crow >= 0 && crow < visrows) {
        if (tsm.curshape == 1) { /* underline */
          XftDrawRect(xftdraw, &colorfg, cx, cy+ch - 2, cw, 2);
        } else if (tsm.curshape == 2) { /* bar */
          XftDrawRect(xftdraw, &colorfg, cx, cy, 2, ch);
        } else { /* block, needs inverting */
          drawcell(ccol, crow,
              &tbuf->lines[tbuf->row][ccol], 1,
              &colorbg, &colorfg);
        }
      }
      drawflush();
    }
  }
  ptykill();
  drawkill();
  killcolors();
  fontkill();
  x11kill();
  return 0;
}
