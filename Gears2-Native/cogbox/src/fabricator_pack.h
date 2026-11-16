// COGBox - a mod folder packed into a zip for publishing (fabricator_pack.cpp). The zip has a stable name per folder so a
// stopped publish of the same unchanged folder can go on where it stopped: <state dir>\uploads\<key>.zip, key = the
// first 16 hex digits of the SHA-256 of the folder's full path (lower case), with <key>.json beside it, the manifest:
// every packed file's path, size and last-write time, and the zip's size. A publish of the folder reuses the zip when
// the manifest still matches the folder (nothing packed again: the same bytes, so fab_upload's remembered session
// matches); otherwise it is packed again. test/fabricator_net_test.cpp covers it.
#pragma once
#include "fabricator_net.h"

#include <cstdint>
#include <functional>
#include <string>

// <state dir>\uploads\<key>.zip for this folder (nothing made).
std::string fab_pack_path(const std::string &folder);

// Packs folder (every file except what COGBox keeps beside a mod: fabricator.json, '.'-named entries, .partial
// unpackings; links refused) into fab_pack_path(folder), or reuses the zip already there when its manifest matches.
// before_repack runs once when a zip or manifest of this folder is there but no longer matches, before it is replaced
// (the caller deletes its old server session). "" when the zip is ready (*zip = its path, *reused = nothing packed);
// "cancelled"; else what went wrong (no paths, only names inside the mod). The store's limits (50 000 files, 8 GiB
// unpacked, 4 GiB packed) stop it early. A failed or cancelled pack leaves no zip and no manifest.
std::string fab_pack_folder(const std::string &folder, FabProgress &p, std::string *zip, bool *reused,
                            const std::function<void()> &before_repack);

// Deletes a packed zip and its manifest (a published, refused or discarded publish).
void fab_pack_drop(const std::string &zip);

// Deletes packed zips (and manifests) in <state dir>\uploads last written more than max_age_s ago, except keep: an
// upload session lives a day on the server, so an older zip can no longer be resumed.
void fab_pack_sweep(int64_t max_age_s, const std::string &keep);
