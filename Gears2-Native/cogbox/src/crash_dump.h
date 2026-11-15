// COGBox - GitHub #286 (review r75): the game's crash minidump, written from outside the game (xenos/crash_dump_ipc.h).
#pragma once

#include <windows.h>

// Started right after the game: a small thread waits for the game's crash request (or its end) and writes the dump
// with MiniDumpWriteDump on the game's process handle. The handle is duplicated; the caller keeps its own.
void crash_dump_watch(HANDLE proc, DWORD pid);

// The writing itself (the thread above calls it; tests call it directly): reads the game's info block, writes the
// .dmp it names and puts the result in the block. False when there was no valid request.
bool crash_dump_write(HANDLE proc, DWORD pid);
