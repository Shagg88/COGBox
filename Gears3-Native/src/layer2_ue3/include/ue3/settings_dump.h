// Layer 2 (Unreal Engine 3): --ue3_dump_settings (diagnostic, off). The DLC light (Shagg 16 Nov: a green / red light on a
// joinable match for "our DLC matches the host's"): job 1962 read the public lobbies' session properties and contexts as
// bare numbers (Gears 3: 1000000B=100 1000001F=-1 10000020=75 1000009E=22 1000009F=0; contexts 0000000C=1 00008001=7
// ...). Every UE3 online settings class (Engine.Settings and its subclasses: the game's party / versus / horde settings)
// carries its own table naming them: PropertyMappings (SettingsPropertyPropertyMetaData: Id, Name, ValueMappings) and
// LocalizedSettingsMappings (LocalizedStringSettingMetaData: Id, Name, ValueMappings of StringIdToStringMapping). This
// logs both for every class default object (Default__<Settings subclass>) once, from a worker thread (reads only), so the
// ids can be named - and whether the host advertises its DLC.
//   Struct layouts come from the engine's own reflection (PropertyOffset / PropertySize on the ScriptStruct); a stride is
//   the end of its last member, rounded up to 4. TArray = {u32 data, i32 num, i32 max}; FName = {index, number}.
#pragma once

#include <ue3/objects.h>

#include <rex/logging/macros.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace ue3::settings_dump {
namespace detail {

struct Fields {
  uint32_t st = 0;      // the ScriptStruct
  uint32_t stride = 0;  // bytes per element
  int id = -1, name = -1, values = -1;
};

inline Fields Describe(const char* struct_name, std::initializer_list<const char*> members) {
  Fields f;
  f.st = ue3::obj::FindObject(struct_name);
  if (!f.st) return f;
  int end = 0;
  for (const char* m : members) {
    const int off = ue3::obj::PropertyOffset(f.st, m), size = ue3::obj::PropertySize(f.st, m);
    const int dim = std::max(1, ue3::obj::PropertyArrayDim(f.st, m));
    if (off >= 0 && size > 0) end = std::max(end, off + size * dim);
  }
  f.stride = uint32_t((end + 3) & ~3);
  f.id = ue3::obj::PropertyOffset(f.st, "Id");
  f.name = ue3::obj::PropertyOffset(f.st, "Name");
  f.values = ue3::obj::PropertyOffset(f.st, "ValueMappings");
  return f;
}

inline std::string FName(uint32_t address) {
  const uint32_t index = ue3::obj::Read32(address), number = ue3::obj::Read32(address + 4);
  std::string n = ue3::obj::Name(index);
  if (number) n += "_" + std::to_string(int(number) - 1);
  return n.empty() ? "?" : n;
}

// "id=Name" pairs of an IdToStringMapping / StringIdToStringMapping array
inline std::string Values(uint32_t array, const Fields& v) {
  if (!v.st || !v.stride || v.id < 0 || v.name < 0) return "(layout not found)";
  const uint32_t data = ue3::obj::Read32(array), num = ue3::obj::Read32(array + 4);
  if (!data || num == 0) return "-";
  std::string out;
  for (uint32_t i = 0; i < num && i < 64; ++i) {
    const uint32_t e = data + i * v.stride;
    if (!out.empty()) out += " ";
    out += std::to_string(int32_t(ue3::obj::Read32(e + uint32_t(v.id)))) + "=" + FName(e + uint32_t(v.name));
  }
  if (num > 64) out += " ... (" + std::to_string(num) + ")";
  return out;
}

inline void DumpArray(const std::string& cls, uint32_t object, int off, const Fields& f, const Fields& vals, const char* what) {
  if (off < 0 || !f.st || !f.stride || f.id < 0 || f.name < 0) return;
  const uint32_t data = ue3::obj::Read32(object + uint32_t(off)), num = ue3::obj::Read32(object + uint32_t(off) + 4);
  for (uint32_t i = 0; data && i < num && i < 96; ++i) {
    const uint32_t e = data + i * f.stride;
    const uint32_t id = ue3::obj::Read32(e + uint32_t(f.id));
    const std::string values = f.values >= 0 ? Values(e + uint32_t(f.values), vals) : "-";
    REXLOG_INFO("ue3_dump_settings: {} {} {:08X} = {} | values: {}", cls, what, id, FName(e + uint32_t(f.name)),
                values.substr(0, 900));
  }
}

inline void Dump(const char* game) {
  const uint32_t settings = ue3::obj::FindClass("Settings");
  if (!settings) {
    REXLOG_WARN("ue3_dump_settings ({}): no Settings class", game);
    return;
  }
  const Fields prop = Describe("SettingsPropertyPropertyMetaData",
                               {"Id", "Name", "ColumnHeaderText", "MappingType", "ValueMappings", "PredefinedValues", "MinVal",
                                "MaxVal", "RangeIncrement"});
  const Fields loc = Describe("LocalizedStringSettingMetaData", {"Id", "Name", "ColumnHeaderText", "ValueMappings"});
  const Fields idmap = Describe("IdToStringMapping", {"Id", "Name"});
  const Fields sidmap = Describe("StringIdToStringMapping", {"Id", "Name", "bIsWildcard"});
  REXLOG_INFO("ue3_dump_settings ({}): strides property {} localized {} id-map {} string-id-map {}", game, prop.stride,
              loc.stride, idmap.stride, sidmap.stride);
  int classes = 0;
  for (uint32_t o : ue3::obj::AllObjects()) {
    const std::string name = ue3::obj::ObjectName(o);
    if (name.rfind("Default__", 0) != 0 || !ue3::obj::IsA(o, settings)) continue;
    const uint32_t cls = ue3::obj::ClassOf(o);
    const std::string cn = ue3::obj::ObjectName(cls);
    ++classes;
    DumpArray(cn, o, ue3::obj::PropertyOffset(cls, "PropertyMappings"), prop, idmap, "property");
    DumpArray(cn, o, ue3::obj::PropertyOffset(cls, "LocalizedSettingsMappings"), loc, sidmap, "context");
  }
  REXLOG_INFO("ue3_dump_settings ({}): {} settings classes", game, classes);
}

}  // namespace detail

// From the game's set-up: a worker thread that waits for the object tables (open() = the game's ue3::obj::Init call),
// then `delay_s` more seconds (the front end's packages loaded), and logs once.
inline void Start(const char* game, std::function<bool()> open, int delay_s) {
  std::thread([game, open, delay_s] {
    for (int i = 0; i < 600 && !open(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!ue3::obj::Ready()) {
      REXLOG_WARN("ue3_dump_settings ({}): object tables not found", game);
      return;
    }
    std::this_thread::sleep_for(std::chrono::seconds(delay_s));
    detail::Dump(game);
  }).detach();
}

}  // namespace ue3::settings_dump
