// Content-versioned HDR companions. Guest storage and its byte layout stay intact.
#pragma once
#include <cstdint>
#include <array>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <xenos/renderer/gpu.h>
#include <xenos/ucode.h>
#include <xenos/renderer/hdr_overlay.h>

namespace xenos::renderer::native_hdr {
// RGBA8 storage -> the presenter's fetch view. Byte endian permutations compose
// by XOR; the selector's alpha/constant cases are reconstructed from the SDR
// reference because snapshot alpha belongs to HDR validity.
inline uint32_t PresentCompanionSwizzle(uint32_t swizzle,uint32_t copy_endian,uint32_t fetch_endian) {
  constexpr uint32_t permutation[4]={0,1,3,2};
  const uint32_t flip=permutation[copy_endian&3]^permutation[fetch_endian&3];
  uint32_t result=0;
  for(unsigned c=0;c<4;++c) {
    uint32_t selector=(swizzle>>(c*3))&7;
    if(selector<4) selector^=flip;
    result|=selector<<(c*3);
  }
  return result;
}
inline std::array<uint8_t,32> SceneProxySlots(uint32_t hdr_mask, uint32_t used_mask) {
  std::array<uint8_t,32> slots; slots.fill(255);
  uint32_t free = ~used_mask;
  for (uint32_t i = 0; i < 32; ++i) if (hdr_mask & (1u << i)) {
    if (!free) { slots.fill(255); return slots; }
    uint32_t spare = 31;
    while (!(free & (1u << spare))) --spare;
    slots[i] = uint8_t(spare); free &= ~(1u << spare);
  }
  return slots;
}

// Reaching definitions across the shader CFG. Unconditional writes kill old
// dependencies; conditional writes/branches merge them. Only upper RGB clips
// reachable from an HDR sample are extended in the HDR evaluation. Coverage
// and alpha are taken from the original bounded evaluation by the wrapper.
inline void ExtendSceneClamps(ucode::Program& program, uint32_t hdr_mask) {
  using namespace ucode;
  struct State { std::array<uint8_t,128> r{}; uint8_t pv = 0; bool ps = false; };
  const size_t n = program.control_flow.size();
  if (!n) return;
  std::vector<State> incoming(n);
  std::vector<bool> reached(n,false), queued(n,false);
  std::vector<size_t> returns;
  for (size_t i=0;i<n;++i) if (program.control_flow[i].opcode == CfOpcode::kCondCall && i+1<n) returns.push_back(i+1);
  std::deque<size_t> work{0}; reached[0]=queued[0]=true;
  while (!work.empty()) {
    const size_t index=work.front(); work.pop_front(); queued[index]=false;
    auto& cf=program.control_flow[index]; State state=incoming[index];
    const auto read = [&](const Operand& operand, unsigned lane) {
      if (operand.source != Source::kRegister) return false;
      const unsigned component=unsigned(operand.components[lane]);
      if (component>=4) return false;
      if (operand.addressing != Addressing::kAbsolute)
        return std::any_of(state.r.begin(),state.r.end(),[&](uint8_t mask){return (mask&(1u<<component))!=0;});
      return operand.index<128 && (state.r[operand.index]&(1u<<component))!=0;
    };
    const auto write = [&](Result& result,uint8_t dependencies,bool conditional) {
      uint8_t bits=0;
      for(unsigned c=0;c<4;++c) if ((result.write_mask&(1u<<c)) && unsigned(result.components[c])<4 &&
          (dependencies&(1u<<unsigned(result.components[c])))) bits|=uint8_t(1u<<c);
      if (result.clamp && (bits&7)) result.hdr_upper_extension=true;
      if (result.target != Target::kRegister) return;
      if (result.addressing != Addressing::kAbsolute) { for(auto& reg:state.r) reg|=bits; return; }
      if (result.index<128) state.r[result.index]=uint8_t((conditional ? state.r[result.index] :
          state.r[result.index]&~result.write_mask)|bits);
    };
    for(auto& instruction:cf.instructions) {
      if(instruction.kind==Instruction::Kind::kTextureFetch) {
        auto& fetch=instruction.tfetch;
        const bool hdr=fetch.opcode==FetchOpcode::kTextureFetch && fetch.fetch_constant<32 &&
            (hdr_mask&(1u<<fetch.fetch_constant));
        write(fetch.result,hdr?7:0,fetch.predicated);
      } else if(instruction.kind==Instruction::Kind::kAlu) {
        auto& alu=instruction.alu; uint8_t vector=0;
        const bool horizontal=alu.vector_opcode==AluVectorOpcode::kDp3 || alu.vector_opcode==AluVectorOpcode::kDp4 ||
            alu.vector_opcode==AluVectorOpcode::kDp2Add || alu.vector_opcode==AluVectorOpcode::kMax4;
        for(unsigned c=0;c<4;++c) for(unsigned o=0;o<alu.vector_operand_count;++o) {
          if(horizontal) { for(unsigned k=0;k<alu.vector_operands[o].component_count;++k)
            if(read(alu.vector_operands[o],k)) vector|=15; }
          else if(read(alu.vector_operands[o],c)) vector|=uint8_t(1u<<c);
        }
        bool scalar=false;
        for(unsigned o=0;o<alu.scalar_operand_count;++o) for(unsigned k=0;k<alu.scalar_operands[o].component_count;++k)
          scalar|=read(alu.scalar_operands[o],k);
        if(alu.scalar_opcode==AluScalarOpcode::kAddsPrev || alu.scalar_opcode==AluScalarOpcode::kMulsPrev ||
           alu.scalar_opcode==AluScalarOpcode::kMulsPrev2 || alu.scalar_opcode==AluScalarOpcode::kSubsPrev ||
           alu.scalar_opcode==AluScalarOpcode::kRetainPrev) scalar|=state.ps;
        if(alu.scalar_opcode==AluScalarOpcode::kMulsc0 || alu.scalar_opcode==AluScalarOpcode::kMulsc1 ||
           alu.scalar_opcode==AluScalarOpcode::kAddsc0 || alu.scalar_opcode==AluScalarOpcode::kAddsc1 ||
           alu.scalar_opcode==AluScalarOpcode::kSubsc0 || alu.scalar_opcode==AluScalarOpcode::kSubsc1)
          scalar|=state.ps || state.pv || vector;
        write(alu.vector_result,vector,alu.predicated);
        write(alu.scalar_result,scalar?15:0,alu.predicated);
        if(!alu.IsVectorOpDefaultNop()) state.pv=alu.predicated ? uint8_t(state.pv|vector) : vector;
        if(!alu.IsScalarOpDefaultNop()) state.ps=alu.predicated ? state.ps||scalar : scalar;
      }
    }
    const auto merge = [&](size_t next,const State& value) {
      if(next>=n) return;
      bool changed=!reached[next]; reached[next]=true;
      auto& into=incoming[next];
      for(unsigned r=0;r<128;++r) { const uint8_t bits=uint8_t(into.r[r]|value.r[r]); changed|=bits!=into.r[r]; into.r[r]=bits; }
      changed|=(into.pv|value.pv)!=into.pv || (value.ps&&!into.ps); into.pv|=value.pv; into.ps|=value.ps;
      if(changed&&!queued[next]) {queued[next]=true;work.push_back(next);}
    };
    if(cf.exec_type!=ControlFlow::ExecType::kUnconditional) merge(index+1,incoming[index]);
    if(cf.opcode==CfOpcode::kReturn) { for(size_t next:returns) merge(next,state); }
    else if(cf.opcode==CfOpcode::kCondJmp || cf.opcode==CfOpcode::kCondCall) {
      merge(cf.target,state);
      if(cf.branch_type!=ControlFlow::ExecType::kUnconditional || cf.opcode==CfOpcode::kCondCall) merge(index+1,state);
    } else if(cf.opcode==CfOpcode::kLoopStart || cf.opcode==CfOpcode::kLoopEnd) {
      merge(cf.target,state); merge(index+1,state);
    } else if(!cf.exec_end) merge(index+1,state);
  }
}

inline std::string TransportScenePairSource(std::string material,uint32_t hdr_mask,uint32_t used_mask) {
  const auto slots=SceneProxySlots(hdr_mask,used_mask);
  if(!hdr_mask) return {};
  std::string declarations;
  for(unsigned i=0;i<32;++i) if(hdr_mask&(1u<<i)) {
    if(slots[i]==255) return {};
    declarations+="XE_VK("+std::to_string(slots[i])+", 1) Texture2D<float4> xe_hdr_src_"+std::to_string(i)+
        " : register(t"+std::to_string(slots[i])+", space0);\n";
    const std::string object="xe_tex_2d_"+std::to_string(i);
    size_t position=0; unsigned samples=0;
    while((position=material.find(object+".",position))!=std::string::npos) {
      const size_t open=material.find('(',position+object.size()+1);
      if(open==std::string::npos) return {};
      if(material.compare(position+object.size()+1,13,"GetDimensions")==0) {position=open+1;continue;}
      size_t end=open+1;unsigned depth=1;
      while(end<material.size()&&depth) {if(material[end]=='(')++depth;else if(material[end]==')')--depth;++end;}
      if(depth) return {};
      const auto original=material.substr(position,end-position);
      const auto proxy="xe_hdr_src_"+std::to_string(i)+original.substr(object.size());
      const auto pair="(hdrScene ? "+proxy+" : "+original+")";
      material.replace(position,end-position,pair);position+=pair.size();++samples;
    }
    if(!samples) return {};
  }
  const std::string signature="XePsOut xe_scene_ui_material(XePsIn xe_in)";
  const auto entry=material.find(signature);
  if(entry==std::string::npos) return {};
  material.replace(entry,signature.size(),declarations+"XePsOut xe_scene_ui_material(XePsIn xe_in, bool hdrScene)");
  const auto gamma=material.find("float xe_degamma1(float g) {");
  if(gamma==std::string::npos) return {};
  material.insert(gamma+sizeof("float xe_degamma1(float g) {")-1,
      "\n  if (g > 1.0) return (1016.0 / 1023.0) * pow(g, 2.2);");
  // All discard decisions belong to the original bounded evaluation. It runs
  // first; the extended evaluation cannot change coverage or alpha.
  size_t discard=0;
  while((discard=material.find("discard;",discard))!=std::string::npos) {
    material.replace(discard,8,"if (!hdrScene) discard;");discard+=23;
  }
  material+=R"(
struct TransportPair { float4 hdr : SV_Target0; float4 sdr : SV_Target1; };
TransportPair main(XePsIn input) {
  TransportPair output;
  output.sdr=xe_scene_ui_material(input,false).color0;
  output.hdr=xe_scene_ui_material(input,true).color0;
  output.hdr.a=output.sdr.a=saturate(output.sdr.a);
  return output;
}
)";
  return material;
}

struct CompanionImage {
  gpu::TextureHandle texture = 0;
  uint32_t width = 0, height = 0;
  uint32_t allocated_width = 0, allocated_height = 0;
  uint64_t generation = 0;
  std::shared_ptr<gpu::TextureHandle> ownership;
};
struct TargetCompanion : CompanionImage {
  uint64_t write_version = 0, used_frame = 0;
};
// Records share immutable allocations. The pool holds weak references, so a
// full resolve can pin a version without copying it. No backend calls happen
// from a shared_ptr destructor (the renderer/backend have different lifetimes).
class CompanionPool {
  struct Entry { std::weak_ptr<gpu::TextureHandle> ownership; uint32_t width,height; uint64_t used; };
  std::unordered_map<gpu::TextureHandle,Entry> entries_;
  uint64_t created_=0,reused_=0;
 public:
  uint64_t Created() const {return created_;}
  uint64_t Reused() const {return reused_;}
  uint64_t Bytes() const {
    uint64_t bytes=0;for(const auto& [texture,e]:entries_)bytes+=uint64_t(e.width)*e.height*8;return bytes;
  }
  CompanionImage Acquire(gpu::Backend& backend,uint32_t width,uint32_t height,uint64_t frame) {
    CompanionImage image; image.width=image.allocated_width=width;image.height=image.allocated_height=height;
    for(auto& [texture,e]:entries_) if(e.ownership.expired() && e.width==width && e.height==height) {
      image.texture=texture;image.ownership=std::make_shared<gpu::TextureHandle>(texture);
      e.ownership=image.ownership;e.used=frame;++reused_;return image;
    }
    gpu::TextureDesc desc;desc.width=width;desc.height=height;desc.format=gpu::Format::kR16G16B16A16Float;
    desc.usage=gpu::kUsageSampled|gpu::kUsageStorage|gpu::kUsageRenderTarget;
    desc.debug_name="Versioned HDR companion allocation";
    image.texture=backend.CreateTexture(desc);
    if(image.texture) {
      image.ownership=std::make_shared<gpu::TextureHandle>(image.texture);
      entries_[image.texture]={image.ownership,width,height,frame};
      ++created_;
    }
    return image;
  }
  void Collect(gpu::Backend& backend,uint64_t frame,uint64_t budget) {
    uint64_t bytes=Bytes();
    std::unordered_map<uint64_t,bool> warm_shapes;
    uint64_t warm_bytes=0;
    for(auto it=entries_.begin();it!=entries_.end();) {
      auto& e=it->second;
      if(!e.ownership.expired()) {e.used=frame;++it;continue;}
      const uint64_t shape=uint64_t(e.width)<<32|e.height;
      // One recently released spare per shape avoids destroying/reallocating a
      // 180 MiB padded image each frame when the active working set is just
      // above the soft budget. Bound the warm reserve and expire after 120 frames.
      const bool warm=budget && e.used+120>=frame && warm_bytes<budget/2 && !warm_shapes.contains(shape);
      if(warm) {warm_shapes[shape]=true;warm_bytes+=uint64_t(e.width)*e.height*8;}
      if(!warm && (bytes>budget || e.used+120<frame)) {
        backend.DestroyTexture(it->first);bytes-=uint64_t(e.width)*e.height*8;it=entries_.erase(it);
      } else ++it;
    }
  }
};

inline bool WritableCompanion(gpu::Backend& backend,CompanionPool& pool,gpu::PipelineHandle copy,
    CompanionImage& image,uint64_t frame,bool preserve) {
  if(!image.texture)return false;
  if(!image.ownership || image.ownership.use_count()==1)return true;
  if(preserve && !copy)return false;
  auto replacement=pool.Acquire(backend,image.allocated_width?image.allocated_width:image.width,
      image.allocated_height?image.allocated_height:image.height,frame);
  if(!replacement.texture)return false;
  if(preserve) StencilReplayTarget::Copy(backend,copy,image.texture,replacement.texture,image.width,image.height);
  replacement.width=image.width;replacement.height=image.height;
  replacement.generation=image.generation;image=std::move(replacement);return true;
}
struct ResolveCompanion : CompanionImage {
  uint32_t start = 0, length = 0, endian = 0, format = 0;
  uint64_t version = 0, frame = 0;
  bool pending = false;
  bool patchable = false;
  uint32_t scale = 1;
  gpu::TextureHandle view_texture = 0, view_reference = 0;
  uint32_t view_width = 0, view_height = 0, view_scale = 1;
  uint64_t view_generation = 0, used_frame = 0;
  bool view_packed = false;
  uint32_t view_swizzle = 0x688;
  bool whole_image = false, storage_swap = false;
  float storage_gain = 1.f;
};

class CompanionStore {
 public:
  std::unordered_map<uint32_t, TargetCompanion> targets;
  std::unordered_map<uint32_t, ResolveCompanion> resolves;

  TargetCompanion* CurrentTarget(uint32_t id, uint64_t write_version) {
    const auto it = targets.find(id);
    if (it == targets.end() || !it->second.texture || !it->second.generation ||
        it->second.write_version != write_version) return nullptr;
    return &it->second;
  }

  const ResolveCompanion* CurrentResolve(uint32_t base, uint64_t version, uint32_t format, uint32_t endian) const {
    const auto it = resolves.find(base);
    if (it == resolves.end() || !it->second.texture || !it->second.generation || !it->second.length ||
        it->second.pending || it->second.version != version || it->second.format != format ||
        it->second.endian != endian) return nullptr;
    return &it->second;
  }

  // A partial write invalidates every overlapping interpretation, including
  // another base pointing into the same allocation. No frame-age assumption.
  void InvalidateRange(uint32_t start, uint32_t length) {
    if (!length) return;
    const uint64_t end = uint64_t(start) + length;
    for (auto& [base, image] : resolves) {
      if (uint64_t(start) < uint64_t(image.start) + image.length && uint64_t(image.start) < end)
        { image.generation = 0; image.patchable = false; }
    }
  }

  template<class Version>
  void BeginWrite(uint32_t start,uint32_t length,Version&& current_version) {
    if(!length) return;
    const uint64_t end=uint64_t(start)+length;
    for(auto& [base,image]:resolves) if(uint64_t(start)<uint64_t(image.start)+image.length && uint64_t(image.start)<end) {
      image.patchable=image.generation && image.version==current_version(image.start,image.length);
      image.generation=0;
    }
  }
};
} // namespace xenos::renderer::native_hdr
