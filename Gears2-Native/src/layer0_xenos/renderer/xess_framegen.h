// Layer 0 (game-free): Intel XeSS frame generation (XeSS-FG, "XeFG") with Xe Low Latency (XeLL), EXPERIMENTAL,
// D3D12 only (--gow2_frame_generation=xess). See xess_framegen.cpp and docs\plan-049-xefg.md. Internal to layer 0, the
// same shape as dlss_framegen.h: frame_generation.cpp forwards its public functions (dev_tools.h) here while XeFG owns
// the swap chain, and latency_reflex.cpp leaves NVAPI Reflex / Anti-Lag 2 off while XeLL drives the latency markers.
#pragma once

#include <cstdint>

struct XenosFgFrame;

// Same as XenosDlssFgFallback: called from the swap-chain hook when XeFG turns out not to run on this PC.
using XenosXefgFallback = bool (*)(void* dxgi_factory2, void* d3d12_queue, void* hwnd, const void* desc1,
                                   void** swap_chain1);

// Loads libxess_fg.dll + libxell.dll from the exe's folder and installs the swap-chain and latency hooks. false = not
// available (the reason is logged); nothing was installed and the caller falls back. fallback may be null.
bool XenosXefgInstall(bool hudless, XenosXefgFallback fallback);
// True from a successful install until XeFG is found not to run on this PC (then the fallback takes over).
bool XenosXefgActive();
// XeLL owns the latency markers: NVAPI Reflex and Anti-Lag 2 must stay off.
bool XenosXefgOwnsLatency();
bool XenosXefgWanted();
uint32_t XenosXefgHudlessFormat();
bool XenosXefgHudlessSize(uint32_t& width, uint32_t& height);
void XenosXefgSetInputs(void* guest_output_resource, const XenosFgFrame* frame);
void XenosXefgFrameSubmitted();
