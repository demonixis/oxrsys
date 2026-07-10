# Vision Pro — UI & UX

**Branch:** `fix/vision-pro-ui`

This is a *client* app, so the UI should stay small — but it must be **seamless
and polished**. The goal is that connecting, entering the immersive view,
recovering from drops, and tweaking a few settings all feel effortless and
"just work", with no dead-ends and no janky transitions.

## Principles

- Minimal surface area: only the controls a viewer actually needs.
- Seamless transitions between the control window and the immersive view — no
  states where the user is stuck with nothing on screen.
- Polished: clear status at all times, sensible defaults, graceful failure.
- Respect the platform: hide the real world as much as Apple allows when the
  user wants a pure rendered view.

## Core flows

### 1. Find / connect to a server
- **Search for server** (discovery) — current entry point.
- With `Auto-enter immersive` enabled, connect to the first discovered server.
  With it disabled, show the discovered server name and let the user connect
  explicitly.
- Clear status text through every phase: searching → found → connecting →
  streaming → error.

### 2. Connection recovery
- If the server disconnects mid-session, **don't dump the user into a dead
  screen**. Offer:
  - **Reconnect to the same server** ("continue previous connection"), and/or
  - **Search for a new server** ("grab a new connection").
- Auto-retry the previous server for a short window before falling back to the
  search UI.
- Distinguish *user-initiated* disconnect from *dropped* connection in the UI.

### 3. Enter / exit the immersive view
- After a server is found, allow re-entering the immersive view with a button
  (we already found the server — no need to re-search).
- `Auto-enter immersive` controls whether connecting immediately opens the
  immersive space. `Keep window in immersive` controls whether the floating
  window remains open while immersed; the default hides it and restores it after
  immersive exit.
- Entering/leaving should be one obvious action, never ambiguous.

### 4. Debug / stats overlay
- **Preview debug info on demand**: FPS, frames delivered/dropped, decode errors,
  latency, packets received (the `StreamStats` we already collect).
- Toggleable — off by default for a clean view, on when diagnosing.
- Consider an in-immersive HUD vs. a panel in the control window.

### 5. Settings
- **Immersion / passthrough control**: make sure we can show *only* the rendered
  view and hide the real world as much as visionOS permits (immersion style).
- `Show hands` maps to visionOS upper-limb visibility while immersed.
- Refresh-rate / quality hints if the server supports them.
- Reconnection behavior (auto-retry on/off, window-hide-on-enter on/off).
- Keep settings few and well-labeled.

## Feature checklist

- [ ] Server discovery list + multi-server pick
- [x] Single-server discovery + explicit connect
- [x] Connection state machine surfaced cleanly (disconnected / discovering /
      connecting / streaming / lost)
- [ ] Reconnect-to-previous vs. find-new on disconnect
- [x] Re-enter immersive view button after server is known
- [x] Setting: auto-enter immersive on connect
- [x] Setting: keep control window visible while immersed
- [x] Setting: visible hands while immersed
- [ ] Setting: immersion style (max-hide passthrough / pure rendered)
- [ ] Debug/stats overlay toggle
- [ ] Polished transitions (no empty/stuck states, no flashes)
- [ ] Clear error + retry affordances everywhere

## Open questions

- [ ] Where should debug info live — control window panel, or an in-immersive HUD?
- [ ] How much of passthrough suppression does visionOS actually allow us, and what
      is the cleanest API for it?
- [ ] Should reconnect be fully automatic, prompted, or configurable (default)?
- [ ] Persist last-used server for one-tap reconnect on next launch?

## Notes

This branch is UX-only; rendering/timing concerns live on
`fix/vision-pro-latency`. Keep changes here free of latency/reprojection logic so
the two branches stay easy to review and merge independently.
