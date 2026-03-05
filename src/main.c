#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

#define TBUFCOLS 256
#define TBUFROWS 128
typedef struct {
  char lines[TBUFROWS][TBUFCOLS];
  int col, row;
} Termbuf;
Termbuf tbuf;

struct timespec thenr, nowr;
long long elapsedr;
/* #define GFXTICKNS 16666667LL */
#define GFXTICKNS 600000000LL
#define GETNS(ts) (clock_gettime(CLOCK_MONOTONIC, &ts))
#define DIFFNS(start, end) \
      ((int64_t)((end).tv_sec - (start).tv_sec) * 1000000000LL + \
      ((end).tv_nsec - (start).tv_nsec))


unsigned int WWIDTH, WHEIGHT;

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
  XSelectInput(display, window, KeyPressMask|PointerMotionMask|StructureNotifyMask);
  XStoreName(display, window, "bj");
  XMapWindow(display, window);
}

void
x11kill() {
  XDestroyWindow(display, window);
  XCloseDisplay(display);
}

void
xstnginit() {
}

void
xstngkill() {
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

void
colorsinit() {
  vis = DefaultVisual(display, DefaultScreen(display));
  cmap = DefaultColormap(display, DefaultScreen(display));
  /* TODO: Pull colors out into config.h */
  XftColorAllocName(display, vis, cmap, "#000000", &colorfg);
  XftColorAllocName(display, vis, cmap, "#6495ED", &colorbg);
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
}

void
ptywrite() {
}

void
ptyread() {
  char buf[256];
  int i, n;
  n = read(ptyfd, buf, sizeof(buf));
  if (n <= 0) { return; }
  for (i = 0; i < n; i++) {
    if (buf[i] == '\r') { tbuf.col = 0; }
    else if (buf[i] == '\n') { if (tbuf.row < TBUFROWS - 1) { tbuf.row++; } }
    else if (buf[i] >= 0x20 && buf[i] < 0x7f) {
      if (tbuf.col < TBUFCOLS - 1) {
        tbuf.lines[tbuf.row][tbuf.col] = buf[i];
        tbuf.col++;
      }
    }
  }
}

void
ptykill() {
  kill(ptypid, SIGHUP);
  close(ptyfd);
}

void
tsminit() {
}

void
tsmkill() {
}

int
main(int argc, char *argv[]) {
  XEvent ev;
  int quit, xfd, r, len;
  fd_set fds;
  struct timeval tv;
  long long remaining;
  char buf[8];
  KeySym ks;
  x11init();
  fontinit();
  colorsinit();
  drawinit();
  tbufinit();
  ptyinit();
  UNUSED(argc); UNUSED(argv);
  quit = 0;
  GETNS(thenr); GETNS(nowr);
  while (!quit) {
    while (XPending(display) > 0) {
      XNextEvent(display, &ev);
      switch (ev.type) {
        case ConfigureNotify:
          if (ev.xconfigure.width != (int)WWIDTH ||
              ev.xconfigure.height != (int)WHEIGHT) {
            WWIDTH = ev.xconfigure.width;
            WHEIGHT = ev.xconfigure.height;
            drawresize();
          }
          break;
        case KeyPress: {
          len = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
          if (len > 0) {
            write(ptyfd, buf, len);
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
    select((ptyfd > xfd ? ptyfd : xfd) + 1, &fds, NULL, NULL, &tv);
    GETNS(nowr);
    if (FD_ISSET(ptyfd, &fds)) { ptyread(); }
    if (DIFFNS(thenr, nowr) >= GFXTICKNS) {
      GETNS(thenr);
      XftDrawRect(xftdraw, &colorbg, 0, 0, WWIDTH, WHEIGHT);
      for (r = 0; r < TBUFROWS; r++) {
        drawcell(0, r, tbuf.lines[r], TBUFCOLS, &colorfg, &colorbg);
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
