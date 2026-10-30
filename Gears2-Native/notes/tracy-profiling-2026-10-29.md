# Tracy profiling enabled

October 29, 2026. Tracy 0.14.1 is enabled in this PC's five normal build configurations: Gears 2 base/TU6, Gears 3 base/TU6 and Judgment Release. Launch a game through CogBox normally, then connect the matching viewer. Restart a game that was already running when its executable was rebuilt.

## Connect the viewer

1. Open `Gears2-Native/third_party/tracy-tools-0.14.1/tracy-profiler.exe`.
2. Connect to **127.0.0.1**, default port **8086**. Run one game at a time for these captures.
3. Reproduce the hitch, disconnect and save the capture in Tracy.

The matching tools were downloaded from the [official Tracy 0.14.1 release](https://github.com/wolfpld/tracy/releases/tag/v0.14.1). The Windows archive's SHA-256 was checked against the release asset digest. Tools and native DLLs are local dependencies, outside the tracked source files.

For a timed capture without opening the viewer, start the game and run this from the COGBox project folder:

```powershell
& .\Gears2-Native\tools\capture-tracy.ps1 -Seconds 30
```

It saves under `Gears2-Native/out/tracy/`. An optional `-Output` selects another file. Existing files are not overwritten. Open the resulting `.tracy` file in the viewer. The helper connects to an existing game; it does not start or stop the game.

## Recorded information

- Named **COGBox renderer** and **COGBox D3D12 recorder** threads.
- Renderer batches, D3D12 recording batches, command-recorder waits and fence waits.
- Guest completion waits, texture loading/creation and DXC shader compilation.
- Swap/submission work, named end-of-frame steps and periodic diagnostic logging.
- Frame markers, renderer elapsed-time plots and completed GPU-time plots from the existing timestamps.

This is a CPU timeline with GPU-time plots. Detailed Tracy GPU pass zones remain future work. The GPU plot reports newly completed timestamp work and can lag the frame being prepared. Renderer elapsed time includes waits; it is not OS CPU execution time.

Collection is **on demand**: zones are recorded when a capture client/viewer connects. Profiling adds work during capture, so use a disconnected viewer or the disabled flag when comparing ordinary playing performance. The D3D12 debug layer is independent and is not needed for Tracy.

## Controls and building

- Native launch flag **`--gow2_tracy=false`** disables COGBox Tracy initialization/zones after a restart, across all three games. It was checked in Judgment: the log reports disabled, and port 8086 has no listener.
- The default runtime flag is true when support is compiled in. Keep the matching `TracyClient.dll` beside the executable even when disabling collection with the flag.
- CMake **`GN_WITH_TRACY=ON`** enables the shared renderer instrumentation in optimized Release builds. All five local build caches are explicitly configured ON. The build copies its SDK's matching Tracy DLL beside each executable.
- Fresh build configurations default to **`GN_WITH_TRACY=OFF`**, retaining the project's existing public-release policy. The public packager intentionally rejects Tracy-linked programs and Tracy DLLs. Before public packaging, rebuild with profiling OFF and ensure unused Tracy DLLs are absent from the packaging inputs. No public package was produced or published here.
- Tracy's definitions are private to the renderer target. The generated game code's expensive per-call profiling counters stay disabled. No SDK source or generated game code was changed.

## Validation

All five normal executable configurations built successfully. The shared suite passed **139 tests / 818,002 assertions**. Live captures were recorded and decoded with Tracy's CSV exporter in all three games:

| Game / scene | Capture duration | Frame markers | Zones |
| --- | ---: | ---: | ---: |
| Judgment / Museum opening | 20.1 s | 1,610 | 274,355 |
| Gears 3 TU6 / Gridlock Horde | 5.1 s | 592 | 83,086 |
| Gears 2 TU6 / Hospital | 5.1 s | 580 | 110,677 |

The samples verify connection, recording, frame markers, populated renderer zones and readable capture files. They are not FPS benchmarks. No checked game log reported a guest exception, device loss or error-level message. The disabled startup path was also checked. Test games used isolated user folders; normal saves and launcher settings were preserved.

Captures and exported zone tables are in `work/tracy/`: `judgment-smoke.tracy`, `gears3-smoke.tracy` and `gears2-smoke.tracy`. Test logs are in `work/judgment-performance/tracy-*`, `work/optimization-round2/tracy-gears3/` and `work/framepacing-regression/tracy-gears2/`. Pre-Tracy executable backups are in `work/tracy/backups/`.

No shader translation or shader-cache identity changed. Enabling Tracy does not require another full shader compilation. The remaining stutters described in the performance report still need diagnosis; enabling profiling is not itself a stutter fix.
