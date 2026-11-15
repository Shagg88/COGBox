# The Fabricator: COGBox's mod store (design)

Status: design agreed with KiltedKernel on 2026-11-15, for Shagg's review. Named by a COGBox player after Gears 3's Horde
vendor. Supersedes the "registry on GitHub" option in COGBox-trilogy's `docs/design/FABRICATOR.md`.

## Why

Players already mod COGBox's games (a folder in `<Game>\mods\`, the launcher's Mods tab, mod API 1, texture packs from
PR #325), but finding mods means Nexus pages, Discord links and zips passed around by hand. The Fabricator puts finding,
installing, updating and publishing mods inside COGBox, for all four games.

**Policy (Shagg, 2026-11-15):** COGBox's GitHub and GitLab never ship Microsoft files. Mods are hosted by the
Fabricator itself, on COGBox's server; the store gives players access to them, players download what they choose, and
what they do with it once it is on their computer is on them. So mod files never go through COGBox's repositories, and
the Fabricator does not depend on GitHub releases (the earlier registry design).

## Goals

- A **FABRICATOR** button on the launcher's home screen, beside PLAY, SETTINGS, ACHIEVEMENTS and SOCIAL, opening a
  screen with three tabs: **Library**, **Store**, **Creator** (layout below; the mockup was agreed).
- Any signed-in COGBox player can publish a mod; it goes live as soon as the automatic checks pass.
- Mods up to **at least 4 GB** (big texture packs, e.g. the 4x Gears 2 pack, about 2 GB).
- Players can report a mod; Shagg takes mods down (and stops a creator publishing) from the admin panel.
- Installing a mod from the Fabricator ends where installing by hand does: a folder in `<Game>\mods\`, turned on in
  the Mods tab, under the game's existing mod rules (modcore, texture packs).

## Non-goals (first version)

- Mods built from the player's own disc (COGBox-trilogy's chapters): listed later as a "build from your disc" kind;
  the first version only hosts files.
- Comments, messages, follows, collections, paid mods.
- Installing required mods automatically (the first version lists them, each with its own Get button).
- A web storefront (the launcher is the store; a read-only web page can come later from the same API).
- Mods for a game other than COGBox's four.

## Anti-goals (what would make it a failure even if it works)

- Any mod file, screenshot or Microsoft asset in COGBox's GitHub or GitLab repositories, release zips or CI artefacts.
- A download that unpacks outside `<Game>\mods\<mod>\`, runs a program, or replaces the player's game files.
- Uploads or downloads that tie up the master server so lobbies, sign-in or signaling stall (it is one VM).
- Tokens, players' addresses or file paths in logs (the server's `log_scrub.ts` rules apply to every new route).
- A player losing a mod folder they changed by hand when the Fabricator updates or removes it.

## Constraints

- **Server:** COGBox's master server, `tools/master-server` (Node / TypeScript, Fastify 5, better-sqlite3), on the
  Oracle VM behind Caddy (`deploy/Caddyfile.example`, host networking), run by Shagg. The Fabricator is new routes and
  tables in it (or a sibling service on the same VM, see "Left to the builder"). The admin panel is
  `tools/admin-panel` (Fastify, its own login, talks to the master server's loopback private API).
- **Accounts:** the existing COGBox sign-in (`/api/v1/auth/claim`; users table: uuid, xuid, display name +
  discriminator, token stored as SHA-256; `checkAuthHeader`). No new account system.
- **Launcher:** `Gears2-Native/cogbox` (C++17, Dear ImGui on SDL3, built with MinGW). It already has: WinHTTP fetches
  with redirects and SHA-256 checks (`update.cpp`), zip unpacking with path safety (`mods_add_zip` in `mods.cpp`),
  7z, JSON (nlohmann), PNG/JPEG decoding to textures (stb_image, `player_icon.cpp`), signed-in requests to the master
  server (`social_backend.cpp`), and the Mods tab with modcore's rules (`mods.cpp`).
- **Storage and bandwidth** are the VM's: disk for the files, and the provider's monthly egress. Both need limits
  (quotas below) and must be visible to Shagg in the admin panel.

## What the player sees (agreed layout)

**Home screen:** a FABRICATOR button after SOCIAL (same style as the others; reachable with a pad, like them).

**Library tab** (opens first):
- Left: filters - All games / Gears 1 / Gears 2 / Gears 3 / Judgment, then Installed / Not installed / Updates.
- Right: a list of the player's mods: thumbnail, name, game, type, size, and a state pill (Installed and on /
  Installed and off / Not installed / Update available / Taken down).
- "Your mods" are the ones the player got from the Fabricator (kept by the server for the account, so they show on a
  new PC as Not installed) plus the mods already in `<Game>\mods\` (shown as local, not from the store).
- Actions per mod: Install, Update, Turn on/off (same as the Mods tab), Uninstall, Open page.

**Store tab:**
- A search box (name, author, tags, description), filters (game, type), and sort (Popular, Rating, New, Recently
  updated, Name).
- A grid of cards: screenshot, name, author, star rating, downloads, and "In library" / "Get".
- A mod's page: screenshots, description, version and date, size, game, type, tags, required mods (each a link),
  the author's other mods, rating (and the player's own rating, once they have it installed), Get / Install /
  Update, Report.

**Creator tab:**
- Left: "+ New upload", then the creator's mods with their state (Live, Taken down, Upload failed) and versions.
- Right, for a new mod or a new version of one: a drop zone (a mod's folder or .zip), then the fields:
  - Name, Game, Type (Texture pack / Campaign / Gameplay / Visuals / Audio / Other), Version.
  - Short description (one line, for the card) and Description (longer, plain text with line breaks).
  - Screenshots: 1 to 6 (PNG or JPEG, shown 16:9), the first is the card's.
  - Tags (up to 8), Required mods (picked from the store), Side (who needs it in multiplayer, as mod.ini's `Side`).
  - "What's new" for a new version.
  - PUBLISH, then progress (checks, upload percentage, resumable), then the result: live, or what the checks found.
- Name, game and type are filled in from the mod's `mod.ini` when it has one.

The Mods tab stays as it is: the Fabricator installs into the same folders, so both show the same mods.

## How it works

### Data (server, SQLite)

- `fab_mods`: id, owner (users.uuid), slug, name, game, type, short and long description, tags, side, created,
  updated, state (live / taken_down / removed_by_owner), download count, rating sum and count.
- `fab_versions`: id, mod, version text, notes, file size, SHA-256, file key on disk, created, state.
- `fab_screens`: mod, position, image key, width, height.
- `fab_requires`: mod, required mod.
- `fab_library`: account, mod, added (the player's "got" list).
- `fab_ratings`: account, mod, stars 1-5, updated (one per account and mod).
- `fab_reports`: id, mod, reporter, reason (a fixed list + text), created, handled by, outcome.
- `fab_creators`: account, publishing allowed (Shagg can stop it), storage used.
- `fab_uploads`: upload sessions (below).

### Upload (resumable, up to 4 GB)

1. The launcher checks the mod locally first (the same checks as the server, so failures show before any upload) and
   zips a folder (miniz, as `main.cpp` already does for reports).
2. `POST /api/v1/fabricator/uploads` with the metadata, size and SHA-256 -> an upload id and a chunk size.
3. `PUT /api/v1/fabricator/uploads/{id}/chunks/{n}` for each chunk, in order, each chunk with its own SHA-256 (an
   interrupted upload asks `GET .../uploads/{id}` which chunks the server has and goes on from there).
4. `POST .../uploads/{id}/finish`: the server joins the chunks, checks the whole file's SHA-256, runs the automatic
   checks, and publishes the version, or answers with what failed.
5. Unfinished sessions expire after 24 hours and their chunks are deleted.

### Automatic checks (server, and the same in the launcher before upload)

- The file is a zip that unpacks (no encryption), at most 4 GB packed and 8 GB unpacked, at most 50 000 files.
- Every path is safe: relative, no `..`, no drive letters or UNC, no links (the rules of `mods_add_zip`).
- No programs or scripts: `.exe .dll .xex .xexp .bat .cmd .ps1 .vbs .js .msi .scr .com .lnk` refused.
- The game matches what the creator chose (a mod.ini `Game` key, if any, must agree).
- mod.ini, when present, parses; its `Api` is no higher than COGBox's newest mod API.
- Screenshots: PNG or JPEG, decode cleanly, at most 4 MB and 3840x2160 each (re-encoded by the server, which also
  strips their metadata).
- Text fields: length limits; names unique per game (case-insensitive) unless the same owner.
- The creator's quota is not exceeded and publishing is allowed for the account.

A mod that fails is not published; the Creator tab shows each reason.

### Download and install

1. Get: `POST /api/v1/fabricator/library/{mod}` (adds it to the player's library), then the version's download
   address.
2. The launcher downloads with HTTP range requests (resumable), checks the SHA-256, and unpacks into
   `<Game>\mods\<folder>.partial`, then renames it into place (as `mods_add_zip` does) and turns it on.
3. The launcher records which Fabricator mod and version a folder holds (a small `fabricator.json` in the mod's
   folder), so the Library can show updates and the Mods tab can show "from the Fabricator".
4. Update: the new version unpacks beside the old one; the old folder moves to `<Game>\mods\.fabricator-backup\`
   (kept until the next update), so a player's own edits are not lost.
5. Download counts: one per account per mod (not per retry).

### Ratings, reports and takedowns

- A player can rate a mod they have installed, 1-5 stars, and change it. Popular = downloads in the last 30 days;
  Rating = a Bayesian average so a single 5-star mod does not top the list.
- Report: a reason (contains malware, someone else's work, broken, offensive, other) and a short text, once per
  account per mod.
- Admin panel, new page: the reports queue (newest first, grouped by mod), each mod's details and files, and actions:
  take down (the mod disappears from the store and its downloads stop; players who installed it keep their copy and
  see "Taken down" in the Library), restore, delete files, stop a creator publishing. Every action is logged with
  who did it.
- A takedown stays reversible until the files are deleted.

### Accounts, quotas and limits

COGBox accounts are cheap (`/api/v1/auth/claim` makes one from a display name), so per-account limits alone can be
dodged by a new account. Agreed for the first version (KiltedKernel, 2026-11-15):

- **An account must be a few days old before it can publish** (the account's creation date; the number of days is a
  setting).
- **A server-wide daily upload cap** (bytes and count per day, across all creators), on top of the per-creator limits.
- Shagg can stop an account publishing; anything else (approving new creators by hand) is not in the first version.

Every number is a setting in the server's configuration, **filled in by Shagg** for the VM's real disk and bandwidth;
the design does not fix them. Starting points for the settings file (to be replaced): 4 GB per file, 20 GB per creator,
3 uploads per creator per day, the last 3 versions' files kept.

- New uploads are refused once the disk passes a set fullness (about 85%), with a clear message in the Creator tab.
- Download rate limits per account so one player cannot saturate the VM.
- The admin panel shows disk used and free, and the month's download volume.

### Browsing, sign-in and deleted accounts (builder's calls, KiltedKernel's agent)

- Anyone can browse the store (listing, search, mod pages, screenshots: public and cacheable). Get, downloads,
  ratings, reports and publishing need the COGBox sign-in (`checkAuthHeader`); download addresses are short-lived and
  tied to the account.
- When an account is deleted (`DELETE /account`), its mods are taken down and their files deleted, and its ratings and
  reports are removed (PRIVACY.md's promise). Players who installed those mods keep their copies; the Library shows them
  as "Removed".

### Folder names and required mods (builder's calls, KiltedKernel's agent)

- A Fabricator install's folder is the mod's store name (made safe for a folder name); if a local folder of that name
  already exists and is not the same Fabricator mod, " (Fabricator)" is added.
- "Required mods" is filled from the mod's `mod.ini` `Requires` when it has one, matched to store mods by name, so the
  store and the game's own rules (modcore) agree. A copy installed by hand is not matched to the store in the first
  version.
- Texture packs (PR #325) are ordinary mods in `<Game>\mods\`, and the Mods tab is shared by all four games, so every
  type installs, turns on and off, and uninstalls the same way.

## Policy text (to add before launch)

- `LEGAL.md`: uploaders are responsible for what they upload; the takedown contact and process.
- `PRIVACY.md`: what the Fabricator keeps (the creator's display name on their mods, ratings, reports, the library
  list) and how to have it removed.
- The Creator tab shows a one-line reminder before PUBLISH, linking to those terms.

## Testing

- Server: unit tests (vitest, as the master server's existing tests) for every route: upload sessions (resume,
  bad chunk, wrong SHA-256, expiry), every automatic check (a fixture zip per refusal), quotas, ratings, reports,
  takedown, search and sort.
- Launcher: tests for the local checks and the install / update / backup logic (the launcher's `test/` style), and
  a manual run against a local master server: publish a mod, find it, install, update, uninstall, report.
- Load: a 4 GB upload and download over a throttled link, with lobbies in use, to check the VM stays responsive.

## Left to the builder

- Whether the Fabricator is routes in the master server or a sibling service on the same VM behind Caddy (a separate
  process keeps big transfers from stalling signaling; it can still read the users table for sign-in). The deployed
  setup is Caddy in front (`deploy/Caddyfile.example`, "as deployed"); deploy changes (a volume for the files, Caddy
  routes) go to Shagg with the PR.
- How files are stored and served (a folder on the VM's disk behind Caddy's file server with short-lived signed
  addresses, or streamed by the service), the chunk size, and the search implementation (SQLite FTS5 is enough).
- The admin panel page's layout.
- The exact ImGui layout details within the agreed screens, the pad navigation, and translations (the launcher's
  `lang\` files).
- Phasing inside the first version (a sensible order: server upload + checks, store listing, launcher Store and
  Library with install, Creator tab, ratings and reports, admin page).

## Later

- "Build from your disc" mods (a recipe the launcher runs locally, for COGBox-trilogy).
- Installing required mods automatically; collections; comments; a web page for each mod.
