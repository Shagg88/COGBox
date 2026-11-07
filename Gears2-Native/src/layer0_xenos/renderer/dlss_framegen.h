// Layer 0 (game-free): NVIDIA DLSS Frame Generation (DLSS-G / DLSS 4 multi frame generation) through NVIDIA Streamline,
// EXPERIMENTAL, D3D12 only (--gow2_frame_generation=dlss). See dlss_framegen.cpp. Internal to layer 0: frame_generation.cpp
// forwards its public functions (dev_tools.h) here while DLSS-G owns the swap chain, and latency_reflex.cpp leaves its
// NVAPI Reflex / Anti-Lag 2 path off while Streamline's sl.reflex drives the latency markers.
#pragma once

#include <cstdint>

struct XenosFgFrame;

// FSR-or-nothing fallback, called from the swap-chain hook when DLSS-G turns out not to run on this PC (a card older
// than RTX 40, HAGS off, an old driver ...): same arguments as rex::FrameHooks::create_swap_chain.
using XenosDlssFgFallback = bool (*)(void* dxgi_factory2, void* d3d12_queue, void* hwnd, const void* desc1,
                                     void** swap_chain1);

// Loads Streamline from the exe's folder and installs the swap-chain and latency hooks. false = not available (the
// reason is logged); nothing was installed and the caller falls back. fallback may be null (no frame generation).
bool XenosDlssFgInstall(bool hudless, XenosDlssFgFallback fallback);
// True from a successful install until DLSS-G is found not to run on this PC (then the usual path takes over).
bool XenosDlssFgActive();
// Streamline owns the latency markers (sl.reflex / sl.pcl): our NVAPI Reflex and Anti-Lag 2 must stay off.
bool XenosDlssFgOwnsLatency();
bool XenosDlssFgWanted();
uint32_t XenosDlssFgHudlessFormat();
bool XenosDlssFgHudlessSize(uint32_t& width, uint32_t& height);
void XenosDlssFgSetInputs(void* guest_output_resource, const XenosFgFrame* frame);
void XenosDlssFgFrameSubmitted();
