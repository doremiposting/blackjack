#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

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
} Termbuf;
Termbuf tbuf;

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
} Tsm;
Tsm tsm;

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
    XftColorAllocName(display, vis, cmap, "#c0caf5", &colorfg);
    XftColorAllocName(display, vis, cmap, "#1a1b26", &colorbg);
  } else {
    /* TODO: Pull colors out into config.h */
    XftColorAllocName(display, vis, cmap, "#000000", &colorfg);
    XftColorAllocName(display, vis, cmap, "#6495ED", &colorbg);
  }
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
tbufinit() {
  int r, c;
  for (r = 0; r < TBUFROWS; r++) {
    for (c = 0; c < TBUFCOLS; c++) {
      tbuf.lines[r][c] = ' ';
    }
  }
  tbuf.row = 0;
  tbuf.col = 0;
  tbuf.scroll = 0;
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
    shell = "/bin/sh";
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
ptywrite() {
}

void tsmcsi(char z);
void tsmproc(char c);

void
ptyread() {
  char buf[256];
  int i, n;
  n = read(ptyfd, buf, sizeof(buf));
  if (n <= 0) { return; }
  for (i = 0; i < n; i++) { tsmproc(buf[i]); }
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
}

void
tsmcsi(char z) {
  int p, q, r, c;
  if (tsm.hascurrent && tsm.nparams < TSMPARAMS) {
    tsm.params[tsm.nparams++] = tsm.crtparam;
  }
  p = tsm.nparams > 0 ? tsm.params[0] : 0;
  q = tsm.nparams > 1 ? tsm.params[1] : 0;
  switch (z) {
    case 'A': /* cursor up */
      if (!p) { p = 1; }
      tbuf.row -= p;
      if (tbuf.row < tbuf.scroll) { tbuf.row = tbuf.scroll; }
      break;
    case 'B': /* cursor down */
      if (!p) { p = 1; }
      tbuf.row += p;
      if (tbuf.row >= tbuf.scroll + visrows) { tbuf.row = tbuf.scroll + visrows - 1; }
      break;
    case 'C': /* cursor right */
      if (!p) { p = 1; }
      tbuf.col += p;
      if (tbuf.col >= TBUFCOLS) { tbuf.col = TBUFCOLS - 1; }
      break;
    case 'D': /* cursor left */
      if (!p) { p = 1; }
      tbuf.col -= p;
      if (tbuf.col < 0) { tbuf.col = 0; }
      break;
    case 'H': case 'f': /* cursor to row/col (1-based) */
      r = p ? p - 1 : 0;
      c = q ? q - 1 : 0;
      tbuf.row = tbuf.scroll + r;
      tbuf.col = c;
      if (tbuf.row >= tbuf.scroll + visrows) { tbuf.row = tbuf.scroll + visrows - 1; }
      if (tbuf.col >= TBUFCOLS) { tbuf.col = TBUFCOLS - 1; }
      break;
    case 'J': /* erase in display */
      if (p == 0) { /* cursor to end */
        memset(&tbuf.lines[tbuf.row][tbuf.col], ' ', TBUFCOLS - tbuf.col);
        for (r = tbuf.row + 1; r < tbuf.scroll + visrows && r < TBUFROWS; r++) {
          memset(tbuf.lines[r], ' ', TBUFCOLS);
        }
      } else if (p == 1) { /* start to cursor */
        for (r = tbuf.scroll; r < tbuf.row && r < TBUFROWS; r++) {
          memset(tbuf.lines[r], ' ', TBUFCOLS);
        }
        memset(tbuf.lines[r], ' ', tbuf.col + 1);
      } else if (p == 2) { /* whole screen */
        for (r = tbuf.scroll; r < tbuf.scroll + visrows && r < TBUFROWS; r++) {
          memset(tbuf.lines[r], ' ', TBUFCOLS);
        }
      }
      break;
    case 'K': /* erase in line */
      if (p == 0) { memset (&tbuf.lines[tbuf.row][tbuf.col], ' ', TBUFCOLS - tbuf.col); }
      else if (p == 1) { memset(tbuf.lines[tbuf.row], ' ', tbuf.col + 1); }
      else if (p == 2) { memset(tbuf.lines[tbuf.row], ' ', TBUFCOLS); }
      break;
    default: /* SGR (m), mode set/reset (h/l), and miscellaneous */
      break;
  }
}

void
tsmproc(char c) {
  unsigned char uc;
  uc = (unsigned char)c;
  switch (tsm.state) {
    case TSMNORMAL:
      if (uc == 0x1B) { tsm.state = TSMESC; }
      else if (uc == 0x07) { /* TODO: BEL */ }
      else if (uc == '\b') { if (tbuf.col > 0) { tbuf.col--; } }
      else if (uc == '\t') {
        tbuf.col = (tbuf.col + 8) & ~7;
        if (tbuf.col >= TBUFCOLS) { tbuf.col = TBUFCOLS - 1; }
      }
      else if (uc == '\r') { tbuf.col = 0; }
      else if (uc == '\n') {
        if (tbuf.row < TBUFROWS - 1) { tbuf.row++; }
        if (tbuf.row >= tbuf.scroll + visrows) {
          tbuf.scroll = tbuf.row - visrows + 1;
        }
      }
      else if (uc >= 0x20 && uc < 0x7F) {
        if (tbuf.col < TBUFCOLS - 1) {
          tbuf.lines[tbuf.row][tbuf.col] = c;
          tbuf.col++;
          if (tbuf.row >= tbuf.scroll + visrows) {
            tbuf.scroll = tbuf.row - visrows + 1;
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
      } else if (c == '(' || c == ')' || c == '*' || c == '+') {
        tsm.state = TSMCHARSEL;
      } else if (c == 'M') { /* reverse index */
        if (tbuf.row > tbuf.scroll) { tbuf.row--; }
        tsm.state = TSMNORMAL;
      } else if (c == 'D') { /* cursor down or advance */
        if (tbuf.row < TBUFROWS - 1) { tbuf.row++; }
        if (tbuf.row >= tbuf.scroll + visrows) {
          tbuf.scroll = tbuf.row - visrows + 1;
        }
        tsm.state = TSMNORMAL;
      } else if (c == 'E') { /* Next line CR */
        tbuf.col = 0;
        if (tbuf.row < TBUFROWS - 1) { tbuf.row++; }
        if (tbuf.row >= tbuf.scroll + visrows) {
          tbuf.scroll = tbuf.row - visrows + 1;
        }
        tsm.state = TSMNORMAL;
      } else if (c == '7') { /* Save cursor pos */
        tsm.savecol = tbuf.col;
        tsm.saverow = tbuf.row;
        tsm.state = TSMNORMAL;
      } else if (c == '8') { /* Restore cursor pos */
        tbuf.col = tsm.savecol;
        tbuf.row = tsm.saverow;
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
      } else if (c == '?' || c == '>' || c == '|') {
        /* private/intermediate bytes: flag and keep collecting */
      } else if (uc >= 0x40 && uc <= 0x7E) {
        /* final byte: dispatch then reset */
        tsmcsi(c);
        tsm.state = TSMNORMAL;
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
          }
          break;
        case KeyPress: {
            len = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_Prior) {
              tbuf.scroll -= visrows;
              if (tbuf.scroll < 0) { tbuf.scroll = 0; }
            } else if (ks == XK_Next) {
              maxscroll = tbuf.row - visrows + 1;
              if (maxscroll < 0) { maxscroll = 0; }
              tbuf.scroll += visrows;
              if (tbuf.scroll > maxscroll) { tbuf.scroll = maxscroll; }
            } else if (len > 0) {
              write(ptyfd, buf, len);
            }
          }
          break;
        case ButtonPress: {
            if (ev.xbutton.button == Button4) {
              tbuf.scroll -= 3;
              if (tbuf.scroll < 0) { tbuf.scroll = 0; }
            } else if (ev.xbutton.button == Button5) {
              maxscroll = tbuf.row - visrows + 1;
              if (maxscroll < 0) { maxscroll = 0; }
              tbuf.scroll += 3;
              if (tbuf.scroll > maxscroll) { tbuf.scroll = maxscroll; }
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
    if (sel > 0 && FD_ISSET(ptyfd, &fds)) { ptyread(); }
    if (DIFFNS(thenr, nowr) >= GFXTICKNS) {
      GETNS(thenr);
      XftDrawRect(xftdraw, &colorbg, 0, 0, WWIDTH, WHEIGHT);
      for (r = 0; r < visrows && (tbuf.scroll + r) < TBUFROWS; r++) {
        drawcell(0, r, tbuf.lines[tbuf.scroll + r], TBUFCOLS, &colorfg, &colorbg);
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
