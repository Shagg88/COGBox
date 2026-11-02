# SDK patch 60 - walk key (GitHub #125)

File: `src/input/mnk/mnk_input_driver.cpp` (rexglue SDK runtime). Not built yet - ships with the next runtime build.

- `keybind_walk` (default empty = off): while held, the keyboard's left stick is scaled to `mnk_walk_scale`
  (default 0.5), so the character walks instead of jogging.
- `mnk_walk_toggle=true`: each press switches walking on / off instead.
- Only the keyboard's own stick is scaled; a real controller is untouched.

Still to do after the runtime ships: a "Walk" key and a Hold / Toggle choice on the launcher's key page (they pass
`--keybind_walk=...` and `--mnk_walk_toggle=true`). Older runtimes ignore unknown options, so the launcher side can only
be added together with this runtime.
