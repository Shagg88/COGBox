#!/bin/sh
# CogBox: regenerate src/stale_known.h from the SHA256SUMS.txt of every release. Usage: tools/stale_known_gen.sh SUMS...
# (each a release's SHA256SUMS.txt; pass all of them, old and new).
cd "$(dirname "$0")/.."
{ echo '// GENERATED: the SHA-256 of every .exe / .dll in the SHA256SUMS.txt of each published COGBox release.'
  echo '// stale_files.cpp only moves an unlisted program file whose hash is in here (or in _old-files\shipped\).'
  echo '// Add the new release'"'"'s hashes after each release: tools/stale_known_gen.sh.'
  echo '#pragma once'; echo 'static const char *const kKnownOldProgramHashes[] = {'
  cat "$@" | tr -d '\r' | awk 'length($1)==64{print tolower($1)}' | sort -u | sed 's/.*/    "&",/'
  echo '};'; } > src/stale_known.h
