# Code generator template overrides (COGBox SDK patch 110, game side)

The rexglue code generator (`bin\rexglue.exe` in each game's SDK prefix) embeds its templates. A manifest's
`[entrypoint] template_dir` points it at this folder, and the two files here replace the embedded
`codegen/pch_h` and `codegen/init_cpp` (rexglue `codegen_writer.cpp` loadOverrides; the path is relative to the manifest's
folder, where CMake runs the codegen).

They are the SDK's own templates (sdk-fix060 = the prefix tool's, checked byte for byte by job 1927) plus patch 110:
on Windows the generated code adds no 0x1000 for guest addresses from 0xE0000000 (`REX_PHYS_HOST_OFFSET(addr) 0u`), and
`<project>_init.cpp` asks the runtime for the matching layout (`rex::memory::detail::SetE0ViewMapped(true)`) before it
creates guest memory. Needs a runtime with patch 110 (rexruntime 064b or newer) and its `rex/system/xmemory.h`.
When the SDK's code generator is updated, these two files must be re-made from its new templates.
