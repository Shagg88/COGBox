# Contributing to COGBox

Thanks for helping! Bug reports, translations, code and testing are all welcome. A few rules keep the project, and
you, safe.

## 1. Your contributions are MIT-licensed

COGBox's own code is under the MIT licence (`LICENSE`). By contributing, you agree that your contribution is
licensed under the same MIT licence ("inbound = outbound"), and that you have the right to submit it.

## 2. Sign off your commits (DCO)

Each commit must carry a `Signed-off-by:` line with your name (or the handle you use publicly) and an email:

    git commit -s -m "Fix the thing"

By signing off you certify the [Developer Certificate of Origin 1.1](https://developercertificate.org/), in short:
the contribution is your own work (or you have the right to submit it under the MIT licence), and you understand it
and your sign-off are public and kept with the project.

## 3. No proprietary content - ever

Don't put any of these in a pull request, issue, discussion, attachment or log:

- game files of any kind: executables (`.xex`, `.exe`), disc images, packages, title updates, DLC, maps, textures,
  audio, video, shaders or other data extracted from a game;
- code copied from leaked, decompiled or disassembled proprietary sources, or from SDKs whose licence doesn't allow
  it (e.g. the Xbox 360 XDK). Research findings written in your own words (addresses, structure layouts, behaviour)
  are fine;
- keys, cracks, copy-protection or Games for Windows - LIVE bypasses, or links to any of them or to download sites for
  games;
- vendor libraries or other third-party code whose licence doesn't allow redistribution, or without its licence text.

If you add third-party code that is allowed, keep its licence header, add its licence to the `licenses` folder and
`THIRD_PARTY_NOTICES.md`, and say so in the pull request.

Contributions that break these rules are removed (including from history where possible).

## 4. Problem reports and logs

Use COGBox's problem report zip: it leaves out saves and game files and replaces your user name. Check what you attach.

## 5. Support for your own copies only

We help with the original retail releases. We don't help with pirated copies, "repacks" or bypasses, and don't
accept changes that only exist to support them.

## 6. Translations

Language files are `Gears2-Native/cogbox/lang/<code>.txt`. Add lines at the end; mark machine-made translations as
such in a `#` comment.

See also `LEGAL.md` and `PRIVACY.md`.
