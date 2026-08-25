# xifevent-shim

Stops a dead X server from freezing a GNOME Wayland session.

`meta_x11_display_get_current_time_roundtrip()` pings a property and waits for
the reply with `XIfEvent()`. `XIfEvent()` has **no timeout**: it blocks until
the matching event arrives. If the X server has gone away that event never
comes, and libX11 spins in `_XReadEvents()`.

This runs on the compositor's main loop, and it is reached from an ordinary
**Wayland** window focus:

```
meta_window_wayland_focus()
  -> meta_display_set_input_focus(timestamp=0)
    -> meta_display_timestamp_too_old()
      -> meta_display_get_current_time_roundtrip()
        -> XIfEvent()          <-- never returns
```

So the entire desktop freezes with gnome-shell pinned at 100% of a core.

mutter already falls back to the monotonic clock when there is no X11 display
at all but a *crashed* Xwayland leaves that pointer non-NULL, so it takes the
X path anyway.

Interpose `XIfEvent()` with a bounded version: poll for the event, and if it
does not arrive within 500 ms return a synthetic `PropertyNotify` carrying a
monotonic timestamp 
The timeout can only fire in a situation where the process would otherwise
never return, so it is strictly an improvement on hanging.

Scoped to the compositor (`gnome-shell`, `mutter`). Every other client gets the
real, unbounded `XIfEvent()`: an ordinary X client may legitimately wait a long
time for an event, and handing it a synthetic one would be wrong.

## Usage

```sh
make
sudo make install
```

Then add it to the compositor's `LD_PRELOAD`, e.g. in the session launcher:

```sh
LD_PRELOAD="...:/usr/local/lib/xifevent-shim.so"
```

`XIFEVENT_SHIM_DEBUG=1` logs each timeout.


