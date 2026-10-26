# 08 - System calls Gears 3 imports that the toolkit only stubs (2026-10-22)

Method: every __imp__ name referenced by our generated code (281) against the pinned rexglue source's
REX_EXPORT_STUB list (2,407). Note: our SDK copy also has Gears 2's patches 25-27 (local players, getsockopt,
XUserReadStats), so NetDll_getsockopt below is implemented in our build.

38 stubs: IoCheckShareAccess IoCompleteRequest IoDeleteDevice IoDismountVolume IoInvalidDeviceRequest
IoRemoveShareAccess IoSetShareAccess NetDll_XNetConnect NetDll_XNetQosGetListenStats NetDll_XNetQosLookup
NetDll_XNetRegisterKey NetDll_XNetServerToInAddr NetDll_XNetUnregisterInAddr NetDll_XNetUnregisterKey
NetDll_getsockname NetDll_getsockopt(patched) NtWriteFileGather ObIsTitleObject ObReferenceObject
RtlUpcaseUnicodeChar XAudioGetDuckerLevel XNetLogonGetTitleID XamShow*UI (14: achievements, friends, messages,
marketplace, gamercard, players, quick chat...) XamUserCreateStatsEnumerator XamVoiceSubmitPacket.
(XamMediaVerification* also appear in the log as stubs - imported by ordinal, not in this list.)

VERIFIED: the rexglue stub (REX_STUB, hook.h 52-56) logs "STUB" and leaves r3 untouched - the caller sees whatever
was in r3, usually its own first argument. Xenia Canary does the same for exports it has no handler for
(UndefinedCallExtern, x64_emitter.cc 497-507: returns 0 to the host, r3 unchanged), and Canary has no handler for
RtlUpcaseUnicodeChar, NtWriteFileGather, ObReferenceObject, ObIsTitleObject, XamUserCreateStatsEnumerator or
IoDismountVolume. So our behaviour matches Canary's, where Gears 3 plays.

Watch list (GUESS - none has caused a problem yet):
- NtWriteFileGather: scatter/gather file writes. If saves fail, look here first.
- XamUserCreateStatsEnumerator: leaderboards (Horde/Versus stats screens); Gears 2's XUserReadStats needed patch 27.
- RtlUpcaseUnicodeChar: returns its argument unchanged by accident (r3 = the char) - fine for case-sensitive data.
- ObReferenceObject: reference counting no-op - harmless unless an object is freed early.
- XamShow*UI: Guide screens (friends, achievements...) - pressing those menu items may do nothing.
- Networking (XNet*, getsockname): online only.
