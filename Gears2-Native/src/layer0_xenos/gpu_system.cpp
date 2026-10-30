// Gears2-Native - MIT. Stage 6 (notes/51): our own graphics system.
//
// What it replaces, re-expressed from the SDK (rexglue, BSD 3-Clause; from Xenia, Copyright Ben Vanik, BSD):
// GraphicsSystem::SetupPresentation / SetupGuestGpu / ReadRegister / WriteRegister / DispatchInterruptCallback /
// MarkVblank and CommandProcessor::WorkerThreadMain / WriteRegister (scratch write-back, COHER dirty bit) /
// MakeCoherent / INTERRUPT. The packets themselves run in our PM4 processor (pm4_shadow.cpp's Pm4Execute, reached
// through the draw observer hub exactly as SDK patches 20-23 reach it), so nothing here parses PM4 except INTERRUPT,
// the one packet Pm4Execute hands back.
#include "xenos/gpu_system.h"
#include <xenos/tracy_profile.h>
#include "xenos/display_size.h"

#include <xenos/dev_tools.h>

#include <rex/cvar.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/draw_observer.h>
#include <rex/graphics/register_file.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging/macros.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/d3d12/d3d12_provider.h>
// SDK Vulkan classes: the SDK built with REXGLUE_USE_VULKAN, and renderdoc_app.h reachable (rex/ui/vulkan/instance.h
// includes it; job 375: rex::runtime doesn't pass that include folder on, CMakeLists links rex::renderdoc for it).
#if defined(REX_HAS_VULKAN) && REX_HAS_VULKAN && __has_include(<renderdoc_app.h>)
#define GN_SDK_VULKAN 1
#else
#define GN_SDK_VULKAN 0
#endif
#if GN_SDK_VULKAN  // notes/68 stage V3
#include <rex/ui/vulkan/provider.h>
#endif
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>
#include <xenos/renderer/gpu.h>

#if defined(_WIN32)
#include <windows.h>  // MonitorFromWindow (xenos/display_size.h)
#endif

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <string>
#include <unordered_map>

REXCVAR_DEFINE_STRING(gow2_gpu, "ours", "Gears2",
                      "Stage 6: ours (default since 6C, job 142: our own graphics system - the SDK's GPU plugin is not "
                      "loaded; presets --gow2_renderer=takeover --gow2_pm4_execute --gow2_pm4_own_draws --gow2_pm4_own_zpd "
                      "--gow2_present=ours) | sdk (--gpu_plugin, the SDK's GPU emulation: the reference for comparisons)");
REXCVAR_DEFINE_BOOL(gow2_gpu_dred, true, "Gears2",
                    "Patch 0006: D3D12 DRED (breadcrumbs + page faults) on our device, so a lost device (TDR / hang) logs "
                    "which GPU work was running and any faulting address (Gears 3 2026-10-23: device lost 6 min into the "
                    "Sovereign prologue with no reason in the log). Small GPU cost; off = as before");
REXCVAR_DECLARE(std::string, gow2_renderer_api);  // shadow_renderer.cpp (notes/68)

REXCVAR_DEFINE_BOOL(gow2_high_resolution_timer, true, "Gears2",
                    "Patch 0087 (frame pacing; Canary main_win.cc win32_high_resolution_timer, on by default there): ask "
                    "Windows for its finest timer resolution (about 0.5 ms instead of 15.6 ms) at start, so short sleeps "
                    "- the game's frame-rate limiter, our 1 ms GPU-wait sleeps - end on time. false = Windows' default");
REXCVAR_DEFINE_BOOL(gow2_gpu_vsync, true, "Gears2",
                    "Stage 6, --gow2_gpu=ours: vblank at the video mode's refresh rate (as the SDK's --vsync); off: "
                    "1000 Hz, and WAIT_REG_MEM yields instead of sleeping");

namespace {

using rex::graphics::RegisterFile;
using rex::X_STATUS;

std::atomic<bool> g_ours{false};

constexpr uint32_t kRegScratchUmsk = 0x01DC, kRegScratchAddr = 0x01DD, kRegCpRbWptr = 0x01C5, kRegScratchReg0 = 0x0578,
                   kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kOpInterrupt = 0x54;

class OurGpuSystem final : public rex::system::IGraphicsSystem {
 public:
  OurGpuSystem() {
    // RegisterFile is a plain array (its constructor lives in the GPU plugin, which is not loaded): zeroed storage.
    std::memset(regs_storage_, 0, sizeof(regs_storage_));
  }
  ~OurGpuSystem() override { Shutdown(); }

  // Notes/68 stage V3: with --gow2_renderer_api=vulkan the SDK's Vulkan provider (and so its Vulkan presenter), when
  // the SDK was built with Vulkan; our Vulkan backend then runs on its device. Otherwise D3D12, as before.
  bool CreateProvider() {
    if (provider_) return true;
    if (REXCVAR_GET(gow2_renderer_api) == "vulkan") {
#if GN_SDK_VULKAN
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (provider_) {
        provider_api_ = "Vulkan";
        return true;
      }
      REXLOG_ERROR("gow2_gpu: unable to create the SDK's Vulkan provider; using D3D12 for the window");
#else
      REXLOG_WARN("gow2_gpu: --gow2_renderer_api=vulkan, but this build has no SDK Vulkan (REXGLUE_USE_VULKAN=OFF or "
                  "renderdoc_app.h not found): the window uses D3D12 and our Vulkan picture is not shown");
#endif
    }
    EnableDred();
    provider_ = rex::ui::d3d12::D3D12Provider::Create();
    provider_api_ = "D3D12";
    return provider_ != nullptr;
  }
  const char* provider_api_ = "D3D12";

  // Patch 0006: before the provider creates the device (DRED settings apply to devices created afterwards).
  void EnableDred() {
    if (dred_tried_ || provider_ || !REXCVAR_GET(gow2_gpu_dred)) return;
    dred_tried_ = true;
    const bool on = xenos::renderer::gpu::EnableDeviceLossDiagnostics();
    REXLOG_INFO("gow2_gpu: device-loss diagnostics (DRED breadcrumbs + page faults) {}", on ? "on" : "not available");
  }
  bool dred_tried_ = false;

  // Patch 0087: NtSetTimerResolution to the finest the system offers (Canary RequestWin32HighResolutionTimer).
  void RequestHighResolutionTimer() {
#if defined(_WIN32)
    if (timer_tried_ || !REXCVAR_GET(gow2_high_resolution_timer)) return;
    timer_tried_ = true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    using QueryFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
    using SetFn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(ntdll, "NtQueryTimerResolution"));
    auto set = reinterpret_cast<SetFn>(GetProcAddress(ntdll, "NtSetTimerResolution"));
    if (!query || !set) return;
    ULONG coarsest = 0, finest = 0, before = 0, after = 0;
    query(&coarsest, &finest, &before);
    set(finest, TRUE, &after);
    REXLOG_INFO("gow2_gpu: timer resolution {:.3f} ms -> {:.3f} ms (finest {:.3f}, coarsest {:.3f}; patch 0087)",
                before / 10000.0, after / 10000.0, finest / 10000.0, coarsest / 10000.0);
#endif
  }
  bool timer_tried_ = false;

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    ::xenos::profiling::Initialize();
    if (presenter_) return X_STATUS_SUCCESS;
    RequestHighResolutionTimer();
    if (!CreateProvider()) {
      REXLOG_ERROR("gow2_gpu: unable to create the D3D12 provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    app_context_ = app_context;
    auto loss = [](bool, bool) { REXLOG_ERROR("gow2_gpu: graphics device lost"); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this, loss]() { presenter_ = provider_->CreatePresenter(loss); });
    } else {
      presenter_ = provider_->CreatePresenter(loss);
    }
    if (!presenter_) {
      REXLOG_ERROR("gow2_gpu: unable to create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("gow2_gpu: OUR graphics system - {} provider and presenter created; the SDK's GPU plugin is not loaded",
                provider_api_);
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = function_dispatcher;
    kernel_state_ = kernel_state;
    memory_ = function_dispatcher->memory();
    if (!CreateProvider()) return X_STATUS_UNSUCCESSFUL;
    vsync_ = REXCVAR_GET(gow2_gpu_vsync);
    // The services our PM4 processor (Pm4Execute) uses - what SDK patches 21-23 gave it.
    services_.write_register = [this](uint32_t i, uint32_t v) { WriteRegister(i, v); };
    services_.read_register = [this](uint32_t i) { return ReadRegisterValue(i); };
    services_.execute_packet = [this](const uint32_t* dwords, uint32_t count) { HandedBack(dwords, count); };
    services_.counter = [this]() { return counter_.load(); };
    services_.running = [this]() { return worker_running_.load(); };
    services_.make_coherent = [this]() { MakeCoherent(); };
    services_.vsync = [this]() { return vsync_; };
    services_.register_file = [this]() -> const RegisterFile* { return &Regs(); };
    services_.swap_done = [this]() { ++counter_; };
    rex::graphics::SetGpuServices(&services_);

    // GPU registers, 0x7FC80000-0x7FC8FFFF (GraphicsSystem::SetupGuestGpu).
    memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
                                   reinterpret_cast<rex::runtime::MMIOReadCallback>(&MmioRead),
                                   reinterpret_cast<rex::runtime::MMIOWriteCallback>(&MmioWrite));
    write_event_ = rex::thread::Event::CreateAutoResetEvent(false);
    worker_running_ = true;
    worker_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          WorkerMain();
          return 0;
        }));
    worker_->set_name("GPU Commands (ours)");
    worker_->Create();
    vsync_running_ = true;
    vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
          VsyncMain();
          return 0;
        }));
    vsync_thread_->set_name("GPU VSync (ours)");
    vsync_thread_->Create();
    REXLOG_INFO("gow2_gpu: guest GPU ours - MMIO range, ring worker and vblank thread ({}) started",
                vsync_ ? "video mode refresh rate" : "1000 Hz, vsync off");
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    interrupt_callback_ = callback;
    interrupt_callback_data_ = user_data;
    REXLOG_INFO("gow2_gpu: SetInterruptCallback({:08X}, {:08X})", callback, user_data);
  }
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    read_index_ = 0;
    ring_ptr_ = ptr;
    ring_bytes_ = uint32_t(1) << (size_log2 + 3);
    REXLOG_INFO("gow2_gpu: ring buffer at {:08X}, {} bytes", ptr, ring_bytes_);
  }
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    read_ptr_writeback_ = ptr;
    (void)block_size_log2;
  }

 private:
  RegisterFile& Regs() { return *reinterpret_cast<RegisterFile*>(regs_storage_); }

  void Shutdown() override {
    worker_running_ = false;
    vsync_running_ = false;
    if (write_event_) write_event_->Set();
    if (worker_) {
      worker_->Wait(0, 0, 0, nullptr);
      worker_.reset();
    }
    if (vsync_thread_) {
      vsync_thread_->Wait(0, 0, 0, nullptr);
      vsync_thread_.reset();
    }
    if (presenter_) {
      if (app_context_) app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      presenter_.reset();
    }
    provider_.reset();
  }

  // ---- registers (GraphicsSystem::ReadRegister/WriteRegister for MMIO; CommandProcessor::WriteRegister) ----
  static uint32_t MmioRead(void*, OurGpuSystem* self, uint32_t addr) { return self->ReadMmio(addr); }
  static void MmioWrite(void*, OurGpuSystem* self, uint32_t addr, uint32_t value) { self->WriteMmio(addr, value); }
  uint32_t ReadMmio(uint32_t addr) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
      case 0x0F00: return 0x08100748;  // RB_EDRAM_TIMING
      case 0x0F01: return 0x0000200E;  // RB_BC_CONTROL
      case 0x194C: {                   // R500_D1MODE_V_COUNTER
        rex::system::X_VIDEO_MODE m;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&m);
        return std::min(uint32_t(m.display_height), uint32_t(0x0FFF));
      }
      case 0x1951: return 1;  // interrupt status: vblank
      case 0x1961: {          // AVIVO_D1MODE_VIEWPORT_SIZE
        rex::system::X_VIDEO_MODE m;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&m);
        return (std::min(uint32_t(m.display_width), uint32_t(0x0FFF)) << 16) |
               std::min(uint32_t(m.display_height), uint32_t(0x0FFF));
      }
      default: return r < RegisterFile::kRegisterCount ? Regs().values[r] : 0;
    }
  }
  void WriteMmio(uint32_t addr, uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      write_index_.store(value);
      write_event_->Set();
    }
    if (r < RegisterFile::kRegisterCount) Regs().values[r] = value;
  }
  uint32_t ReadRegisterValue(uint32_t index) {
    if (index < RegisterFile::kRegisterCount) return const_cast<volatile uint32_t&>(Regs().values[index]);
    auto it = extended_.find(index);
    return it == extended_.end() ? 0 : it->second;
  }
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= RegisterFile::kRegisterCount) {
      extended_[index] = value;
      return;
    }
    RegisterFile& regs = Regs();
    const_cast<volatile uint32_t&>(regs.values[index]) = value;  // volatile for WAIT_REG_MEM
    if (index >= kRegScratchReg0 && index < kRegScratchReg0 + 8) {
      const uint32_t n = index - kRegScratchReg0;
      if ((1u << n) & regs.values[kRegScratchUmsk]) {  // scratch write-back (big-endian)
        if (uint8_t* p = memory_->TranslatePhysical<uint8_t*>(regs.values[kRegScratchAddr] + n * 4)) {
          const uint32_t be = std::byteswap(value);
          std::memcpy(p, &be, 4);
        }
      }
    } else if (index == kRegCoherStatusHost) {
      const_cast<volatile uint32_t&>(regs.values[index]) |= 0x80000000u;  // dirty: the next WAIT_REG_MEM syncs
    }
  }
  void MakeCoherent() {
    volatile uint32_t* v = Regs().values;
    // Patch 0029 (Canary / SDK; Gears 3 bug hunt E6): clear only the status bit (31), not the whole register.
    if (v[kRegCoherStatusHost] & 0x80000000u) v[kRegCoherStatusHost] &= ~0x80000000u;  // COHER_STATUS_HOST.status
  }

  // ---- the one packet Pm4Execute still hands back: INTERRUPT (big-endian dwords, header first) ----
  void HandedBack(const uint32_t* dwords, uint32_t count) {
    const uint32_t header = std::byteswap(dwords[0]);
    const uint32_t opcode = (header >> 8) & 0x7F;
    if ((header >> 30) == 3 && opcode == kOpInterrupt && count >= 2) {
      const uint32_t cpu_mask = std::byteswap(dwords[1]);
      for (uint32_t n = 0; n < 6; ++n)
        if (cpu_mask & (1u << n)) DispatchInterrupt(1, n);
      ++interrupts_;
      return;
    }
    if (unexpected_++ < 8)
      REXLOG_ERROR("gow2_gpu: packet {:08X} (opcode {:02X}) handed back - nothing here runs it (only INTERRUPT is "
                   "expected)", header, opcode);
  }
  void DispatchInterrupt(uint32_t source, uint32_t cpu) {
    if (!interrupt_callback_) return;
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) return;
    if (cpu == 0xFFFFFFFF) cpu = 2;
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, interrupt_callback_data_};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
  }

  // ---- threads ----
  void WorkerMain() {
    while (worker_running_) {
      uint32_t write = write_index_.load();
      if (write == 0xBAADF00D || read_index_ == write || !ring_ptr_) {
        uint32_t loops = 0;
        do {  // spin, then sleep on the event (CommandProcessor::WorkerThreadMain)
          if (loops > 500) rex::thread::Wait(write_event_.get(), true, std::chrono::milliseconds(5));
          rex::thread::MaybeYield();
          ++loops;
          write = write_index_.load();
        } while (worker_running_ && (write == 0xBAADF00D || read_index_ == write || !ring_ptr_));
        if (!worker_running_) break;
      }
      rex::graphics::DrawObserver* observer = rex::graphics::GetDrawObserver();
      if (!observer) {
        if (no_observer_++ == 0)
          REXLOG_ERROR("gow2_gpu: no draw observer - nothing runs the ring (needs --gow2_pm4_execute)");
        read_index_ = write;  // drop it rather than spin
      } else {
        observer->OnPrimaryRange(ring_ptr_ & 0x1FFFFFFF, ring_bytes_ / 4, read_index_, write);
        read_index_ = write;
      }
      if (read_ptr_writeback_) {
        const uint32_t be = std::byteswap(read_index_);
        // Job 173 (split): the title sees the read pointer move once our renderer thread got there - it reuses the
        // ring's space after that, including vertex data the renderer still reads (DrawPrimitiveUP copies).
        if (!XenosPm4DeferWrite(read_ptr_writeback_ & 0x1FFFFFFFu, &be, 4))
          if (uint8_t* p = memory_->TranslatePhysical<uint8_t*>(read_ptr_writeback_)) {
            std::memcpy(p, &be, 4);
            XenosNotifyGuestWrite();  // job 225
          }
      }
    }
  }
  void VsyncMain() {  // GraphicsSystem's vblank worker: counter + interrupt source 0 at the refresh rate
    rex::system::X_VIDEO_MODE m;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&m);
    const double hz = std::max(1.0, double(float(m.refresh_rate)));
    const uint64_t freq = rex::chrono::Clock::guest_tick_frequency();
    const uint64_t interval = vsync_ ? std::max<uint64_t>(1, uint64_t(double(freq) / hz)) : std::max<uint64_t>(1, freq / 1000);
    uint64_t last = rex::chrono::Clock::QueryGuestTickCount();
    while (vsync_running_) {
      const uint64_t now = rex::chrono::Clock::QueryGuestTickCount();
      while (now - last >= interval) {
        ++counter_;
        DispatchInterrupt(0, 2);
        last += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
  }

  alignas(RegisterFile) unsigned char regs_storage_[sizeof(RegisterFile)];
  std::unordered_map<uint32_t, uint32_t> extended_;
  std::unique_ptr<rex::ui::GraphicsProvider> provider_;  // D3D12, or Vulkan (notes/68 V3)
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  rex::graphics::GpuServices services_;
  std::unique_ptr<rex::thread::Event> write_event_;
  rex::system::object_ref<rex::system::XHostThread> worker_, vsync_thread_;
  std::atomic<bool> worker_running_{false}, vsync_running_{false};
  std::atomic<uint32_t> write_index_{0xBAADF00D}, counter_{0};
  uint32_t read_index_ = 0, ring_ptr_ = 0, ring_bytes_ = 0, read_ptr_writeback_ = 0;
  uint32_t interrupt_callback_ = 0, interrupt_callback_data_ = 0;
  uint64_t interrupts_ = 0, unexpected_ = 0, no_observer_ = 0;
  bool vsync_ = true;
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> XenosCreateOurGpuSystemIfRequested() {
  const std::string mode = REXCVAR_GET(gow2_gpu);
  if (mode != "ours") {
    if (!mode.empty() && mode != "sdk") REXLOG_WARN("gow2_gpu: unknown '{}' (sdk | ours); using the SDK's", mode);
    return nullptr;
  }
  g_ours = true;
  // Our graphics system needs the whole takeover chain (no SDK GPU code is there to fall back on): preset it.
  const std::pair<const char*, const char*> preset[] = {
      {"gow2_renderer", "takeover"}, {"gow2_pm4_execute", "true"}, {"gow2_pm4_own_draws", "true"},
      {"gow2_pm4_own_zpd", "true"},  {"gow2_present", "ours"}};
  std::string set;
  for (const auto& [name, value] : preset) {
    if (rex::cvar::GetFlagByName(name) != value) {
      rex::cvar::SetFlagByName(name, value);
      set += fmt::format(" --{}={}", name, value);
    }
  }
  REXLOG_INFO("gow2_gpu: ours - preset{}", set.empty() ? " already given on the command line" : set);
  return std::make_unique<OurGpuSystem>();
}

bool XenosOurGpuSystem() { return g_ours.load(); }

// ---- "Match my monitor": the window and monitor sizes (xenos/display_size.h) ----
namespace xenos::display {
namespace {
std::atomic<void*> g_window{nullptr};
std::atomic<uint64_t> g_surface{0};  // width << 32 | height
}  // namespace
void SetWindow(void* native_handle) { g_window.store(native_handle); }
void SetSurfaceSize(uint32_t width, uint32_t height) {
  if (width && height) g_surface.store(uint64_t(width) << 32 | height);
}
bool SurfaceSize(uint32_t& width, uint32_t& height) {
  const uint64_t v = g_surface.load();
  width = uint32_t(v >> 32);
  height = uint32_t(v);
  return width && height;
}
bool MonitorSize(uint32_t& width, uint32_t& height) {
  width = height = 0;
#if defined(_WIN32)
  HWND hwnd = static_cast<HWND>(g_window.load());
  HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (mon && GetMonitorInfoW(mon, &mi)) {
    width = uint32_t(std::max<LONG>(0, mi.rcMonitor.right - mi.rcMonitor.left));
    height = uint32_t(std::max<LONG>(0, mi.rcMonitor.bottom - mi.rcMonitor.top));
  }
#endif
  return width && height;
}
}  // namespace xenos::display
