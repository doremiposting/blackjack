#include <stdio.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "main.h"

Display *display;
Window window;
XWindowAttributes wa = {0};
GC gc;
Atom wmdelwin;

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
  XSelectInput(display, window, KeyPressMask|PointerMotionMask);
  XStoreName(display, window, "bj");
  XMapWindow(display, window);
}

void
x11kill() {
  XCloseDisplay(display);
}

void
xstnginit() {
  return;
}

void
xstngkill() {
  return;
}

void
fontinit() {
}

void
fontkill() {
}

void
colorsinit() {
}

void
killcolors() {
}

void
drawinit() {
}

void
drawkill() {
}

void
drawcell(int col, int row, const char *str, size_t len,
    XftColor *fg, XftColor *bg) {
  UNUSED(col); UNUSED(row); UNUSED(str); UNUSED(len); UNUSED(fg); UNUSED(bg);
}

void
drawflush() {
}

void
ptyinit() {
}

void
ptywrite() {
}

void
ptyread() {
}

void
ptykill() {
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
  int quit;
  x11init();
  UNUSED(argc); UNUSED(argv);
  quit = 0;
  while (!quit) {
    while (XPending(display) > 0) {
      XNextEvent(display, &ev);
      switch (ev.type) {
        case KeyPress:
        switch (XLookupKeysym(&ev.xkey, 0)) {
          case 'q':
            quit = 1;
          default:
        }
        break;
        case ClientMessage:
          if ((Atom) ev.xclient.data.l[0] == wmdelwin) { quit = 1; }
          break;
        default:
          break;
      }
    }
  }
  x11kill();
  return 0;
}
