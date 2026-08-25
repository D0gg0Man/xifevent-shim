/*
 * xifevent-shim.c -- bound mutter's X11 timestamp round-trip so a dead
 * Xwayland cannot freeze the whole desktop.
 *
 * THE BUG
 *   meta_x11_display_get_current_time_roundtrip() (src/x11/meta-x11-display.c)
 *   pings a property and then calls XIfEvent() to wait for the PropertyNotify.
 *   XIfEvent() has NO timeout: it blocks until the event arrives. If the X
 *   server has gone away it never arrives, and libX11 spins in _XReadEvents().
 *
 *   This runs on the compositor's main loop, reached from an ordinary WAYLAND
 *   window focus:
 *       meta_window_wayland_focus()
 *         -> meta_display_set_input_focus (timestamp=0)
 *           -> meta_display_timestamp_too_old()
 *             -> meta_display_get_current_time_roundtrip()
 *               -> XIfEvent()            <-- never returns
 *   so the entire session freezes with gnome-shell at 100% of a core.
 *
 *   Observed on FuriOS 2026-08-24: Xwayland aborts at startup when it cannot
 *   take the single HWC2 composer client (mutter holds it), and the next focus
 *   change locks up the desktop. mutter already falls back to the monotonic
 *   clock when display->x11_display is NULL -- but a *crashed* Xwayland leaves
 *   that pointer non-NULL, so it takes the X path anyway.
 *
 * THE FIX
 *   Interpose XIfEvent with a bounded version: poll for the event, and if it
 *   does not arrive within XIFEVENT_TIMEOUT_MS, return a synthetic
 *   PropertyNotify carrying a monotonic timestamp. That is exactly the value
 *   mutter's own no-X11 fallback would have used, and it is strictly better
 *   than hanging forever -- the only situation where the timeout fires is one
 *   where the process would otherwise never return.
 *
 *   Applied via the session's LD_PRELOAD rather than by rebuilding libmutter:
 *   the installed libmutter carries FuriOS mobile-only API
 *   (meta_display_set_forward_to_wayland_while_grabbed, meta_window_is_mapped,
 *   meta_display_{get,set}_keyboard_box) that is NOT in FuriLabs/mutter forky
 *   9270fdc, and rebuilding from that commit would drop those symbols and break
 *   the lock screen's call handling.
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <dlfcn.h>
#include <unistd.h>
#include <time.h>
#include <X11/Xlib.h>

#define XIFEVENT_TIMEOUT_MS 500

static int debug_enabled;

/* Same LOG() shape as the other hybris shims. */
#define LOG(fmt, ...) shim_log("xifevent-shim: " fmt, ##__VA_ARGS__)

static void __attribute__((format(printf, 1, 2)))
shim_log(const char *fmt, ...) {
    va_list ap;

    if (!debug_enabled)
        return;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

__attribute__((constructor))
static void shim_init(void)
{
  debug_enabled = getenv("XIFEVENT_SHIM_DEBUG") != NULL;
}

/* The session exports LD_PRELOAD to every child, but only the compositor has
 * the deadlock: it calls XIfEvent() from its main loop, where blocking freezes
 * the desktop. An ordinary X client may legitimately wait a long time for an
 * event, and handing it a synthetic one would be wrong -- so everything except
 * the compositor gets the real, unbounded XIfEvent(). */
static int is_compositor(void) {
  static int cached = -1;
  char buf[256] = {0};
  const char *base;
  ssize_t n;

  if (cached != -1)
    return cached;

  n = readlink("/proc/self/exe", buf, sizeof (buf) - 1);
  if (n < 0)
    {
      cached = 0;
      return cached;
    }
  buf[n] = '\0';
  base = strrchr(buf, '/');
  base = base ? base + 1 : buf;
  cached = (strcmp(base, "gnome-shell") == 0 ||
            strcmp(base, "mutter") == 0);
  return cached;
}

static long monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int XIfEvent(Display *display,
          XEvent  *event_return,
          Bool   (*predicate)(Display *, XEvent *, XPointer),
          XPointer arg) {
  static int (*real_check)(Display *, XEvent *,
                            Bool(*) (Display *, XEvent *, XPointer),
                            XPointer) = NULL;
  long deadline;
  int fd;

  if (!real_check)
    real_check = dlsym(RTLD_NEXT, "XCheckIfEvent");

  if (!is_compositor())
    {
      static int (*passthrough)(Display *, XEvent *,
                                 Bool(*) (Display *, XEvent *, XPointer),
                                 XPointer) = NULL;
      if (!passthrough)
        passthrough = dlsym(RTLD_NEXT, "XIfEvent");
      if (passthrough)
        return passthrough(display, event_return, predicate, arg);
    }

  /* No XCheckIfEvent to build on: fall through to the real blocking call
   * rather than inventing behaviour. */
  if (!real_check)
    {
      static int (*real_if)(Display *, XEvent *,
                             Bool(*) (Display *, XEvent *, XPointer),
                             XPointer) = NULL;
      if (!real_if)
        real_if = dlsym(RTLD_NEXT, "XIfEvent");
      if (!real_if)
        return 0;
      return real_if(display, event_return, predicate, arg);
    }

  XFlush(display);
  fd = ConnectionNumber(display);
  deadline = monotonic_ms() + XIFEVENT_TIMEOUT_MS;

  for (;;)
    {
      struct pollfd pfd;
      int ret;

      if (real_check(display, event_return, predicate, arg))
        return 0;

      if (monotonic_ms() >= deadline)
        break;

      pfd.fd = fd;
      pfd.events = POLLIN;
      pfd.revents = 0;
      ret = poll(&pfd, 1, 50);
      if (ret < 0 && errno != EINTR)
        break;
      /* Peer hung up / bad fd: the server is gone, stop waiting immediately. */
      if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
        break;
    }

  /* Timed out. Hand back a PropertyNotify carrying a monotonic timestamp --
   * what mutter uses when there is no X11 display at all. */
  memset(event_return, 0, sizeof (*event_return));
  event_return->type = PropertyNotify;
  event_return->xproperty.display = display;
  event_return->xproperty.time = (unsigned long) monotonic_ms();

  LOG("XIfEvent timed out after %dms(X server gone?); "
       "returning synthetic timestamp %lu",
       XIFEVENT_TIMEOUT_MS, event_return->xproperty.time);

  return 0;
}
