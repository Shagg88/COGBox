// Shared UE3 shader preparation. Our code (MIT).
#pragma once
namespace rex { class Runtime; namespace system { class XThread; } }
void Ue3InstallShaderPrebuildIfRequested(rex::Runtime* runtime);
void Ue3HoldForShaderPrebuild(rex::system::XThread* thread);
