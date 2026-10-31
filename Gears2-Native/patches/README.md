# SDK patches

COGBox's runtime (`rexruntime.dll`) is the rexglue SDK at tag
`nightly-20261002-bd833a2a` with these patches.

## Quick way

```
git clone --branch nightly-20261002-bd833a2a https://github.com/rexglue/rexglue-sdk
cd rexglue-sdk
git apply ../COGBox/Gears2-Native/patches/rexglue-sdk-full.diff
```

`rexglue-sdk-full.diff` is everything the 0.2.5 release runtime is built with: patches 1-45, in one file.
Then move the third-party submodules to the tags in `sdk-pins.txt` and configure with the options in
`sdk-cmake-options.txt`.

## What the other files are

- `rexglue-sdk.diff` is patches 1-37 combined.
- `sdk-patch38-*.diff` to `sdk-patch45-*.diff` came after it. Apply them in number order on top of it.
  Patches 39 and 40 use bare file names: apply 39 inside `src/input/mnk` (`patch -p0`) and 40 inside
  `src/audio/sdl` (`patch -p1`).
- `sdk-patch15` to `sdk-patch37` are the separate patches that went into `rexglue-sdk.diff`. They're kept
  for reference only, so don't apply them again.
- `sdk-patch16-zpd-pairing-UNBUILT.diff` was never built. It isn't in any release.

Gears 3 and Judgment use the same runtime.
