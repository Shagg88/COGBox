// Layer 2 (Unreal Engine 3): reading the engine's objects in guest memory - names, classes, properties - for code that
// works with them by name (developer mode's scripted controller). Gears3-Native's ue3/objects.h for Gears of War
// (2006): the same calls, with the engine's object layout as data (UE3 moved its fields between Gears 1, 2 and 3) found
// in memory at start-up (Discover, below) unless --ue3_object_layout gives it. From KiltedKernel's Gears 3 port. Our
// code (MIT).
//   FNameEntry  {u32 Index (= n * 2), u32 HashNext, char Name[]} (name text at Layout::name_text); GNames = array of
//               entry pointers, its first names the engine's hardcoded ones (None, ByteProperty, IntProperty...)
//   UObject     ObjectInternalInteger (its own GObjObjects slot), Outer, Name (index, number), Class
//   UStruct     SuperStruct, Children (first UField; UField Next); UProperty ElementSize, Offset; UBoolProperty BitMask
// Every read is checked (an address that isn't committed reads as 0); safe from any thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue3::obj {

// byte offsets of the fields above (0: not known)
struct Layout {
  uint32_t slot = 0, outer = 0, name = 0, cls = 0;          // UObject
  uint32_t super = 0, children = 0, next = 0;               // UStruct, UField
  uint32_t elem_size = 0, prop_offset = 0, bool_mask = 0;   // UProperty, UBoolProperty
  uint32_t name_text = 8;                                   // FNameEntry's text
  uint32_t name_wide = 0;                                   // 1: the text is 16-bit characters (big-endian)
  uint32_t name_number = 1;                                 // 1: an FName is (index, number); 0: the index alone
};

// Finds GNames and GObjObjects: from the engine's two globals (`names_var`, `objects_var`: TArray {data, count, max})
// when known, else by searching memory; then the layout (`known` when given, else discovered and logged). Call from a
// worker thread until it returns true (cheap to retry; the search runs again only after a failure).
bool Init(uint32_t names_var, uint32_t objects_var, const Layout* known = nullptr);
bool Ready();
const Layout& CurrentLayout();
std::string LayoutText(const Layout& l);            // "slot,outer,name,class,super,children,next,size,offset,mask,text,wide,number"
bool ParseLayout(const std::string& text, Layout* out);

uint32_t Read32(uint32_t address);  // big-endian guest word; 0 when unreadable
float ReadFloat(uint32_t address);
bool Readable(uint32_t address, uint32_t size);
bool Write32(uint32_t address, uint32_t value);
bool Write8(uint32_t address, uint8_t value);

std::string Name(uint32_t index);
std::string ObjectName(uint32_t object);              // "Name" or "Name_<n>"
uint32_t ClassOf(uint32_t object);
uint32_t FindClass(const char* name);                 // the UClass object
bool IsA(uint32_t object, uint32_t cls);              // object's class is cls or derives from it
uint32_t FindObject(const char* name);                // the first object of that name (Default__X included)
std::vector<uint32_t> FindInstancesOf(uint32_t cls);  // every non-default instance of cls or a subclass
std::vector<uint32_t> AllObjects();                   // every object in GObjObjects
int PropertyOffset(uint32_t cls, const char* property);  // -1 when not found
int PropertySize(uint32_t cls, const char* property);    // ElementSize, -1 when not found
uint32_t BoolMask(uint32_t cls, const char* property);   // 0 for any other property type

}  // namespace ue3::obj
