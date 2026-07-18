#include "native_renderer/sw2e_native_renderer.h"

#include "native_renderer/sw2e_native_gpu_replay.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/native_render_event_stream.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_BOOL(sw2e_native_renderer, false, "SM2/Native Render",
                    "Enable the SW2E native renderer sidecar event consumer");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_log_interval, 60, "SM2/Native Render",
                     "Frame interval for SW2E native renderer sidecar summaries")
    .range(1, 10000);

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_hash_memory, true, "SM2/Native Render",
                    "Hash small guest-memory samples for native-renderer correlation");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_memory_hash_bytes, 4096, "SM2/Native Render",
                     "Maximum bytes to hash from each native-renderer guest-memory sample")
    .range(0, 65536);

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_dump_samples, false, "SM2/Native Render",
                    "Dump unique guest-memory samples touched by native-renderer fetches");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_dump_priority_samples_only, false, "SM2/Native Render",
                    "Only dump samples from indexed, strip, model-layout, or multi-texture draws");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_dump_gap_samples_only, false, "SM2/Native Render",
                    "Only hash/dump samples from unsupported native layout/transform gap draws");

REXCVAR_DEFINE_STRING(sw2e_native_renderer_sample_root, "extracted/native_render_samples",
                      "SM2/Native Render", "Folder for native-renderer guest-memory samples");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_dump_sample_limit, 256, "SM2/Native Render",
                     "Maximum unique guest-memory samples to dump per run")
    .range(0, 100000);

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_dump_full_textures, false, "SM2/Native Render",
                    "Dump complete supported texture footprints instead of only hash samples");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_full_texture_max_bytes, 8 * 1024 * 1024,
                     "SM2/Native Render",
                     "Maximum bytes for one complete native-renderer texture dump")
    .range(0, 64 * 1024 * 1024);

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay, false, "SM2/Native Render",
                    "Capture supported live draws and replay them through a native PC GPU path");

REXCVAR_DEFINE_STRING(sw2e_native_renderer_gpu_replay_path,
                      "extracted/native_render_samples/native_gpu_replay.bmp",
                      "SM2/Native Render", "Output BMP path for native GPU replay");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_gpu_replay_draw_limit, 7, "SM2/Native Render",
                     "Maximum live draws captured before early native GPU replay completion; 0 "
                     "waits for swap")
    .range(0, 4096);

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_live_present, false, "SM2/Native Render",
                    "Present the captured native GPU replay in a child D3D11 window");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_gpu_replay_live_present_limit, 0, "SM2/Native Render",
                     "Maximum native GPU live replay presents per run; 0 means unlimited")
    .range(0, 100000);

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_suppress_backend_swap, false,
                    "SM2/Native Render",
                    "Suppress the compatibility backend swap after a successful native GPU live "
                    "replay present");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_complete_on_swap, true, "SM2/Native Render",
                    "Complete a native GPU replay batch at each swap instead of only by draw count");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_include_solid_geometry, false,
                    "SM2/Native Render",
                    "Include opt-in solid rectangle-list and triangle-strip families in native replay");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_include_transform_gaps, false,
                    "SM2/Native Render",
                    "Include experimental projected gameplay transform-gap draws in native replay");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_transform_gaps_only, false,
                    "SM2/Native Render",
                    "Only capture experimental projected transform-gap draws in native replay");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_gpu_replay_transform_gap_min_vertices, 32,
                     "SM2/Native Render",
                     "Minimum decoded vertices for projected transform-gap native replay draws")
    .range(3, 100000);

REXCVAR_DEFINE_INT32(sw2e_native_renderer_gpu_replay_projected_min_indices, 0,
                     "SM2/Native Render",
                     "Minimum expanded indices for projected transform-gap native replay draws")
    .range(0, 1000000);

REXCVAR_DEFINE_STRING(sw2e_native_renderer_gpu_replay_projected_vertex_shader_filter, "",
                      "SM2/Native Render",
                      "Optional hex vertex-shader hash filter for projected transform-gap replay");

REXCVAR_DEFINE_STRING(sw2e_native_renderer_gpu_replay_projected_pixel_shader_filter, "",
                      "SM2/Native Render",
                      "Optional hex pixel-shader hash filter for projected transform-gap replay");

REXCVAR_DEFINE_STRING(
    sw2e_native_renderer_gpu_replay_projection_strategy, "shader-final-or-heuristic",
    "SM2/Native Render",
    "Projected transform-gap projection strategy: heuristic, shader-final, shader-bone0-final, "
    "shader-skinned-final, or shader-final-or-heuristic");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_debug_fit_projected_gaps, false,
                    "SM2/Native Render",
                    "Normalize projected transform-gap replay bounds for debug visibility");

REXCVAR_DEFINE_BOOL(sw2e_native_renderer_gpu_replay_normalize_projected_gaps, false,
                    "SM2/Native Render",
                    "Normalize constant-projected transform-gap replay bounds for visibility");

namespace sw2e::native_renderer {
namespace {

using rex::graphics::native_render::DrawEvent;
using rex::graphics::native_render::DrawEventContext;
using rex::graphics::native_render::EventSink;
using rex::graphics::native_render::SwapEvent;
using rex::graphics::native_render::TextureFetchSummary;
using rex::graphics::native_render::VertexAttributeSummary;
using rex::graphics::native_render::VertexFetchSummary;

struct ShaderPairCounter {
  uint64_t vertex_hash = 0;
  uint64_t pixel_hash = 0;
  uint32_t draws = 0;
};

struct TransformGapAttribute {
  uint32_t data_format = 0;
  int32_t offset_words = 0;
  int32_t exp_adjust = 0;
  uint32_t result_storage_target = 0;
  uint32_t result_storage_index = 0;
  uint32_t result_write_mask = 0;
  uint32_t result_used_components = 0;
  uint32_t result_swizzle = 0;
  bool is_signed = false;
  bool is_integer = false;
};

struct TransformGapCounter {
  uint64_t vertex_hash = 0;
  uint64_t pixel_hash = 0;
  uint64_t attribute_signature_hash = 0;
  uint32_t primitive_type = 0;
  bool indexed = false;
  uint32_t fetch_constant = 0;
  uint32_t stride_words = 0;
  uint32_t attribute_count = 0;
  uint32_t recorded_attribute_count = 0;
  uint32_t texture_fetch_count = 0;
  uint32_t first_texture_format = 0;
  uint32_t first_texture_dimension = 0;
  uint32_t first_texture_tiled = 0;
  std::array<TransformGapAttribute, 4> attributes = {};
  uint32_t draws = 0;
};

struct DepthOnlyCounter {
  uint64_t vertex_hash = 0;
  uint64_t pixel_hash = 0;
  uint32_t primitive_type = 0;
  uint32_t index_count = 0;
  bool indexed = false;
  uint32_t normalized_depthcontrol = 0;
  uint32_t rb_depth_info = 0;
  uint32_t normalized_color_mask = 0;
  uint32_t pixel_color_target_mask = 0;
  uint32_t vertex_fetch_count = 0;
  uint32_t texture_fetch_count = 0;
  bool pixel_shader_needed = false;
  bool rasterization_potentially_done = false;
  uint32_t draws = 0;
};

struct ProjectionCandidate {
  bool valid = false;
  bool column_major = false;
  bool has_upstream_transform = false;
  bool use_shared_shader_skin = false;
  const char* source = "none";
  float finite_ratio = 0.0f;
  float inside_ratio = 0.0f;
  float score = 0.0f;
  std::array<uint32_t, 4> constant_indices = {};
  std::array<std::array<float, 4>, 4> rows = {};
  std::array<uint32_t, 3> upstream_constant_indices = {};
  std::array<std::array<float, 4>, 3> upstream_rows = {};
  std::array<bool, rex::graphics::native_render::kMaxFloatConstantSummariesPerDraw>
      shared_shader_constant_present = {};
  std::array<std::array<float, 4>,
             rex::graphics::native_render::kMaxFloatConstantSummariesPerDraw>
      shared_shader_constants = {};
  std::array<float, 3> ndc_mins = {};
  std::array<float, 3> ndc_maxs = {};
};

struct FrameStats {
  uint32_t frame_index = 0;
  uint32_t draws = 0;
  uint32_t indexed_draws = 0;
  uint32_t vertex_fetch_draws = 0;
  uint32_t texture_fetch_draws = 0;
  uint32_t memexport_draws = 0;
  uint32_t output_merger_write_draws = 0;
  uint32_t no_output_skip_candidate_draws = 0;
  uint32_t no_output_point_list_draws = 0;
  uint32_t rasterized_no_output_draws = 0;
  uint32_t viz_query_draws = 0;
  uint32_t native_replay_supported_draws = 0;
  uint32_t native_replay_supported_textured_draws = 0;
  uint32_t native_replay_supported_solid_draws = 0;
  uint32_t native_replay_supported_depth_only_draws = 0;
  uint32_t native_replay_supported_projected_draws = 0;
  uint32_t native_replay_supported_indexed_draws = 0;
  uint32_t native_replay_depth_only_draws = 0;
  uint32_t native_replay_unsupported_indexed_draws = 0;
  uint32_t native_replay_unsupported_shape_draws = 0;
  uint32_t native_replay_unsupported_layout_draws = 0;
  uint32_t native_replay_unsupported_texture_draws = 0;
  uint32_t native_replay_unsupported_transform_draws = 0;
  uint32_t vertex_fetch_count = 0;
  uint32_t texture_fetch_count = 0;
  uint32_t indexed_sample_draws = 0;
  uint32_t hashed_vertex_fetches = 0;
  uint32_t hashed_texture_fetches = 0;
  uint32_t hashed_index_buffers = 0;
  uint32_t vertex_sample_bytes = 0;
  uint32_t texture_sample_bytes = 0;
  uint32_t index_sample_bytes = 0;
  uint32_t other_shader_pair_draws = 0;
  uint32_t min_vertex_fetch_addr = UINT32_MAX;
  uint32_t max_vertex_fetch_end = 0;
  uint32_t min_index_addr = UINT32_MAX;
  uint32_t max_index_end = 0;
  uint64_t vertex_memory_hash = 0;
  uint64_t texture_memory_hash = 0;
  uint64_t index_memory_hash = 0;
  std::array<ShaderPairCounter, 16> shader_pairs = {};
  std::array<TransformGapCounter, 8> transform_gaps = {};
  std::array<TransformGapCounter, 8> layout_gaps = {};
  std::array<DepthOnlyCounter, 8> depth_only = {};
  uint32_t other_transform_gap_draws = 0;
  uint32_t other_layout_gap_draws = 0;
  uint32_t other_depth_only_draws = 0;
};

void ResetFrame(FrameStats& stats, uint32_t frame_index) {
  stats = {};
  stats.frame_index = frame_index;
}

bool HasVizQuerySideEffect(const DrawEvent& event) { return event.pa_sc_viz_query != 0; }

bool IsNativeNoOutputSkipCandidate(const DrawEvent& event) {
  return !event.output_merger_writes && event.vertex_memexport_mask == 0 &&
         event.pixel_memexport_mask == 0 && !HasVizQuerySideEffect(event);
}

bool HasNativeReplayColorOutput(const DrawEvent& event) {
  return event.output_merger_writes && ((event.normalized_color_mask & 0x0F) != 0);
}

bool NativeDepthControlStencilEnable(uint32_t normalized_depthcontrol) {
  return (normalized_depthcontrol & 0x1) != 0;
}

bool NativeDepthControlZEnable(uint32_t normalized_depthcontrol) {
  return (normalized_depthcontrol & 0x2) != 0;
}

bool NativeDepthControlZWriteEnable(uint32_t normalized_depthcontrol) {
  return (normalized_depthcontrol & 0x4) != 0;
}

uint32_t NativeDepthControlZFunc(uint32_t normalized_depthcontrol) {
  return (normalized_depthcontrol >> 4) & 0x7;
}

constexpr uint32_t kGuestPhysicalMemoryBytes = 0x20000000;
constexpr uint64_t kFnv1a64Offset = 14695981039346656037ull;
constexpr uint64_t kFnv1a64Prime = 1099511628211ull;
constexpr uint32_t kXenosFormat8888 = 6;
constexpr uint32_t kXenosFormatDxt45 = 20;
constexpr uint32_t kXenosDimension2DOrStacked = 1;
constexpr uint32_t kXenosPrimitiveTriangleList = 4;
constexpr uint32_t kXenosPrimitiveTriangleStrip = 6;
constexpr uint32_t kXenosVertexFormat32Float = 36;
constexpr uint32_t kXenosVertexFormat32_32Float = 37;
constexpr uint32_t kXenosVertexFormat32_32_32_32Float = 38;
constexpr uint32_t kXenosVertexFormat32_32_32Float = 57;
constexpr uint32_t kBc3BlockWidth = 4;
constexpr uint32_t kBc3BlockHeight = 4;
constexpr uint32_t kBc3BytesPerBlock = 16;
constexpr uint32_t kNativeGpuReplayDrawLogLimit = 32;
constexpr uint32_t kNativeGpuReplayProjectedRejectLogLimit = 32;
constexpr uint32_t kNativeGpuReplayPassLogInitial = 8;
constexpr uint32_t kNativeGpuReplayPassLogInterval = 60;
constexpr uint32_t kNativeGpuReplayProjectionHeuristicConstantLimit = 8;
constexpr float kNativeGpuReplayWidth = 1280.0f;
constexpr float kNativeGpuReplayHeight = 720.0f;
constexpr uint32_t kTextureTileWidthHeightBlocks = 32;
constexpr uint32_t kTextureSubresourceAlignmentBytes = 4096;

uint64_t HashBytes(const uint8_t* data, uint32_t size) {
  uint64_t hash = kFnv1a64Offset;
  for (uint32_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= kFnv1a64Prime;
  }
  return hash;
}

uint64_t HashPhysicalMemorySample(const rex::memory::Memory* memory, uint32_t address,
                                  uint32_t requested_size, uint32_t& bytes_hashed) {
  bytes_hashed = 0;
  if (!memory || requested_size == 0) {
    return 0;
  }

  const uint32_t physical_offset = address & (kGuestPhysicalMemoryBytes - 1);
  const uint32_t max_contiguous = kGuestPhysicalMemoryBytes - physical_offset;
  bytes_hashed = std::min(requested_size, max_contiguous);
  if (bytes_hashed == 0) {
    return 0;
  }

  const uint8_t* data = memory->TranslatePhysical<const uint8_t*>(address);
  return HashBytes(data, bytes_hashed);
}

bool CopyPhysicalMemorySample(const rex::memory::Memory* memory, uint32_t address, uint32_t size,
                              std::vector<uint8_t>& output) {
  output.clear();
  if (!memory || size == 0) {
    return false;
  }

  const uint32_t physical_offset = address & (kGuestPhysicalMemoryBytes - 1);
  const uint32_t max_contiguous = kGuestPhysicalMemoryBytes - physical_offset;
  if (size > max_contiguous) {
    return false;
  }

  const uint8_t* data = memory->TranslatePhysical<const uint8_t*>(address);
  output.assign(data, data + size);
  return true;
}

uint32_t ReadBe32(const uint8_t* data) {
  return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) |
         uint32_t(data[3]);
}

uint32_t ReadU32WithEndian(const uint8_t* data, uint32_t endian) {
  uint32_t bits = 0;
  if (endian == 2) {
    bits = ReadBe32(data);
  } else {
    std::memcpy(&bits, data, sizeof(bits));
    if (endian == 1) {
      bits = ((bits << 8) & 0xFF00FF00u) | ((bits >> 8) & 0x00FF00FFu);
    } else if (endian == 3) {
      bits = (bits << 16) | (bits >> 16);
    }
  }
  return bits;
}

float ReadF32(const uint8_t* data, uint32_t endian) {
  const uint32_t bits = ReadU32WithEndian(data, endian);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint32_t NativeReplayFloatFormatComponentCount(uint32_t data_format) {
  switch (data_format) {
    case kXenosVertexFormat32Float:
      return 1;
    case kXenosVertexFormat32_32Float:
      return 2;
    case kXenosVertexFormat32_32_32Float:
      return 3;
    case kXenosVertexFormat32_32_32_32Float:
      return 4;
    default:
      return 0;
  }
}

bool DecodeNativeReplayFloatAttribute(const uint8_t* vertex, uint32_t vertex_stride_bytes,
                                      uint32_t endian,
                                      const VertexAttributeSummary& attribute,
                                      std::array<float, 4>& values) {
  values = {0.0f, 0.0f, 0.0f, 1.0f};
  if (attribute.offset_words < 0) {
    return false;
  }

  const uint32_t component_count = NativeReplayFloatFormatComponentCount(attribute.data_format);
  if (component_count == 0) {
    return false;
  }

  const uint32_t byte_offset = static_cast<uint32_t>(attribute.offset_words) * 4;
  const uint32_t byte_count = component_count * 4;
  if (byte_offset > vertex_stride_bytes || byte_count > vertex_stride_bytes - byte_offset) {
    return false;
  }

  std::array<float, 4> source = {0.0f, 0.0f, 0.0f, 1.0f};
  for (uint32_t component = 0; component < component_count; ++component) {
    source[component] = ReadF32(vertex + byte_offset + component * 4, endian);
  }

  for (uint32_t component = 0; component < 4; ++component) {
    const uint32_t swizzle = (attribute.result_swizzle >> (component * 3)) & 0x7;
    if (swizzle <= 3) {
      values[component] = source[swizzle];
    } else if (swizzle == 4) {
      values[component] = 0.0f;
    } else if (swizzle == 5) {
      values[component] = 1.0f;
    }
  }
  return true;
}

const VertexAttributeSummary* FindNativeReplayAttributeByResult(
    const VertexFetchSummary& fetch, uint32_t result_storage_index, uint32_t min_components) {
  const uint32_t attribute_count =
      std::min(fetch.attribute_summary_count,
               rex::graphics::native_render::kMaxVertexAttributeSummariesPerFetch);
  for (uint32_t i = 0; i < attribute_count; ++i) {
    const VertexAttributeSummary& attribute = fetch.attributes[i];
    if (attribute.result_storage_target != 1 ||
        attribute.result_storage_index != result_storage_index ||
        NativeReplayFloatFormatComponentCount(attribute.data_format) < min_components) {
      continue;
    }
    return &attribute;
  }
  return nullptr;
}

const VertexAttributeSummary* FindNativeReplayPositionAttribute(const VertexFetchSummary& fetch) {
  if (const VertexAttributeSummary* attribute = FindNativeReplayAttributeByResult(fetch, 1, 3)) {
    return attribute;
  }

  const uint32_t attribute_count =
      std::min(fetch.attribute_summary_count,
               rex::graphics::native_render::kMaxVertexAttributeSummariesPerFetch);
  for (uint32_t i = 0; i < attribute_count; ++i) {
    const VertexAttributeSummary& attribute = fetch.attributes[i];
    if (attribute.offset_words == 0 &&
        NativeReplayFloatFormatComponentCount(attribute.data_format) >= 3) {
      return &attribute;
    }
  }
  return nullptr;
}

const VertexAttributeSummary* FindNativeReplayTexcoordAttribute(const VertexFetchSummary& fetch) {
  const uint32_t attribute_count =
      std::min(fetch.attribute_summary_count,
               rex::graphics::native_render::kMaxVertexAttributeSummariesPerFetch);
  for (uint32_t i = 0; i < attribute_count; ++i) {
    const VertexAttributeSummary& attribute = fetch.attributes[i];
    if (attribute.offset_words > 0 &&
        NativeReplayFloatFormatComponentCount(attribute.data_format) == 2) {
      return &attribute;
    }
  }

  if (const VertexAttributeSummary* attribute = FindNativeReplayAttributeByResult(fetch, 0, 2)) {
    return attribute;
  }

  for (uint32_t i = 0; i < attribute_count; ++i) {
    const VertexAttributeSummary& attribute = fetch.attributes[i];
    if (attribute.offset_words != 0 &&
        NativeReplayFloatFormatComponentCount(attribute.data_format) >= 2) {
      return &attribute;
    }
  }
  return nullptr;
}

bool CanDecodeNativeReplayTexturedVertexFetch(const VertexFetchSummary& fetch) {
  if (FindNativeReplayPositionAttribute(fetch) && FindNativeReplayTexcoordAttribute(fetch)) {
    return true;
  }
  return fetch.stride_words == 6 && fetch.attribute_count == 3;
}

bool IsNativeReplayScreenSpaceTexturedVertexFetch(const VertexFetchSummary& fetch) {
  return fetch.stride_words == 6 && fetch.attribute_count == 3;
}

bool DecodeNativeReplayTexturedVertex(const uint8_t* vertex, uint32_t vertex_stride_bytes,
                                      const VertexFetchSummary& fetch,
                                      gpu_replay::ReplayVertex& replay_vertex) {
  const VertexAttributeSummary* position_attribute = FindNativeReplayPositionAttribute(fetch);
  const VertexAttributeSummary* texcoord_attribute = FindNativeReplayTexcoordAttribute(fetch);
  if (position_attribute && texcoord_attribute) {
    std::array<float, 4> position = {};
    std::array<float, 4> texcoord = {};
    if (DecodeNativeReplayFloatAttribute(vertex, vertex_stride_bytes, fetch.endian,
                                         *position_attribute, position) &&
        DecodeNativeReplayFloatAttribute(vertex, vertex_stride_bytes, fetch.endian,
                                         *texcoord_attribute, texcoord)) {
      replay_vertex.x = position[0];
      replay_vertex.y = position[1];
      replay_vertex.z = position[2];
      replay_vertex.w = position[3];
      replay_vertex.u = texcoord[0];
      replay_vertex.v = texcoord[1];
      return true;
    }
  }

  if (fetch.stride_words == 6 && vertex_stride_bytes >= 24) {
    replay_vertex.x = ReadF32(vertex + 0, fetch.endian);
    replay_vertex.y = ReadF32(vertex + 4, fetch.endian);
    replay_vertex.z = ReadF32(vertex + 8, fetch.endian);
    replay_vertex.w = ReadF32(vertex + 12, fetch.endian);
    replay_vertex.u = ReadF32(vertex + 16, fetch.endian);
    replay_vertex.v = ReadF32(vertex + 20, fetch.endian);
    return true;
  }

  return false;
}

void DecodeNativeReplaySharedShaderSkinInputs(const uint8_t* vertex,
                                              uint32_t vertex_stride_bytes, uint32_t endian,
                                              uint64_t vertex_shader_hash,
                                              gpu_replay::ReplayVertex& replay_vertex) {
  replay_vertex.has_shared_shader_skin = false;
  replay_vertex.shared_shader_weight0 = 1.0f;
  replay_vertex.shared_shader_weight1 = 0.0f;
  replay_vertex.shared_shader_constant_offsets = {};

  if (vertex_shader_hash == 0x45C4DDDAAA10F75Full) {
    replay_vertex.has_shared_shader_skin = true;
    return;
  }

  if (vertex_shader_hash != 0xED8D12865D27DEBFull || vertex_stride_bytes < 24) {
    return;
  }

  const float weight0 = ReadF32(vertex + 12, endian);
  const float weight1 = ReadF32(vertex + 16, endian);
  if (!std::isfinite(weight0) || !std::isfinite(weight1)) {
    return;
  }

  const uint32_t packed_indices = ReadU32WithEndian(vertex + 20, endian);
  replay_vertex.shared_shader_weight0 = weight0;
  replay_vertex.shared_shader_weight1 = weight1;
  replay_vertex.shared_shader_constant_offsets = {
      (packed_indices >> 24) & 0xFFu,
      (packed_indices >> 16) & 0xFFu,
      (packed_indices >> 8) & 0xFFu,
      packed_indices & 0xFFu,
  };
  replay_vertex.has_shared_shader_skin = true;
}

std::array<float, 4> TransformNativeReplayPosition(
    const gpu_replay::ReplayVertex& vertex,
    const std::array<std::array<float, 4>, 4>& rows, bool column_major) {
  const std::array<float, 4> source = {vertex.x, vertex.y, vertex.z, 1.0f};
  std::array<float, 4> output = {};
  if (column_major) {
    for (uint32_t component = 0; component < 4; ++component) {
      for (uint32_t column = 0; column < 4; ++column) {
        output[component] += rows[column][component] * source[column];
      }
    }
  } else {
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t component = 0; component < 4; ++component) {
        output[row] += rows[row][component] * source[component];
      }
    }
  }
  return output;
}

std::optional<std::array<float, 3>> EvaluateNativeReplaySharedShaderBone(
    const gpu_replay::ReplayVertex& vertex, const ProjectionCandidate& projection,
    uint32_t constant_offset) {
  const std::array<float, 4> source = {vertex.x, vertex.y, vertex.w, vertex.z};
  std::array<float, 3> output = {};
  for (uint32_t row = 0; row < 3; ++row) {
    const uint32_t constant_index = 6u - row + constant_offset;
    if (constant_index >= projection.shared_shader_constant_present.size() ||
        !projection.shared_shader_constant_present[constant_index]) {
      return std::nullopt;
    }

    const auto& constant = projection.shared_shader_constants[constant_index];
    const std::array<float, 4> swizzled_constant = {
        constant[3],
        constant[2],
        constant[0],
        constant[1],
    };
    for (uint32_t component = 0; component < 4; ++component) {
      output[row] += swizzled_constant[component] * source[component];
    }
    if (!std::isfinite(output[row])) {
      return std::nullopt;
    }
  }
  return output;
}

std::optional<std::array<float, 3>> EvaluateNativeReplaySharedShaderSkin(
    const gpu_replay::ReplayVertex& vertex, const ProjectionCandidate& projection) {
  if (!vertex.has_shared_shader_skin) {
    return std::nullopt;
  }

  const float weight0 = vertex.shared_shader_weight0;
  const float weight1 = vertex.shared_shader_weight1;
  const float weight2 = 1.0f - weight0 - weight1;
  if (!std::isfinite(weight0) || !std::isfinite(weight1) || !std::isfinite(weight2) ||
      std::max({std::abs(weight0), std::abs(weight1), std::abs(weight2)}) > 8.0f) {
    return std::nullopt;
  }

  std::array<float, 3> output = {};
  const auto add_weighted_bone = [&](uint32_t offset, float weight) -> bool {
    if (std::abs(weight) <= 1e-5f) {
      return true;
    }
    const std::optional<std::array<float, 3>> bone =
        EvaluateNativeReplaySharedShaderBone(vertex, projection, offset);
    if (!bone) {
      return false;
    }
    for (uint32_t axis = 0; axis < 3; ++axis) {
      output[axis] += (*bone)[axis] * weight;
    }
    return true;
  };

  if (!add_weighted_bone(vertex.shared_shader_constant_offsets[0], weight0) ||
      !add_weighted_bone(vertex.shared_shader_constant_offsets[1], weight1) ||
      !add_weighted_bone(vertex.shared_shader_constant_offsets[2], weight2)) {
    return std::nullopt;
  }
  return output;
}

std::array<float, 4> TransformNativeReplayPositionWithCandidate(
    const gpu_replay::ReplayVertex& vertex, const ProjectionCandidate& projection) {
  gpu_replay::ReplayVertex projected_vertex = vertex;
  if (projection.use_shared_shader_skin) {
    const std::optional<std::array<float, 3>> output =
        EvaluateNativeReplaySharedShaderSkin(vertex, projection);
    if (!output) {
      const float nan = std::numeric_limits<float>::quiet_NaN();
      return {nan, nan, nan, nan};
    }
    projected_vertex.x = (*output)[0];
    projected_vertex.y = (*output)[1];
    projected_vertex.z = (*output)[2];
    projected_vertex.w = 1.0f;
  } else if (projection.has_upstream_transform) {
    const std::array<float, 4> source = {vertex.x, vertex.y, vertex.w, vertex.z};
    std::array<float, 3> output = {};
    for (uint32_t row = 0; row < 3; ++row) {
      for (uint32_t component = 0; component < 4; ++component) {
        output[row] += projection.upstream_rows[row][component] * source[component];
      }
    }
    projected_vertex.x = output[0];
    projected_vertex.y = output[1];
    projected_vertex.z = output[2];
    projected_vertex.w = 1.0f;
  }
  return TransformNativeReplayPosition(projected_vertex, projection.rows,
                                       projection.column_major);
}

ProjectionCandidate ScoreNativeReplayProjectionCandidate(
    ProjectionCandidate candidate, const std::vector<gpu_replay::ReplayVertex>& vertices) {
  if (vertices.size() < 3) {
    return candidate;
  }

  const size_t sample_stride = std::max<size_t>(1, vertices.size() / 256);
  uint32_t sampled_count = 0;
  uint32_t valid_count = 0;
  uint32_t inside_count = 0;
  std::array<float, 3> mins = {std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max()};
  std::array<float, 3> maxs = {-std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max()};

  for (size_t vertex_index = 0; vertex_index < vertices.size() && sampled_count < 256;
       vertex_index += sample_stride, ++sampled_count) {
    const std::array<float, 4> clip =
        TransformNativeReplayPositionWithCandidate(vertices[vertex_index], candidate);
    if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[2]) ||
        !std::isfinite(clip[3]) || std::abs(clip[3]) < 1e-6f) {
      continue;
    }

    const std::array<float, 3> ndc = {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
    if (!std::isfinite(ndc[0]) || !std::isfinite(ndc[1]) || !std::isfinite(ndc[2])) {
      continue;
    }
    if (std::max({std::abs(ndc[0]), std::abs(ndc[1]), std::abs(ndc[2])}) >= 1000.0f) {
      continue;
    }

    ++valid_count;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      mins[axis] = std::min(mins[axis], ndc[axis]);
      maxs[axis] = std::max(maxs[axis], ndc[axis]);
    }
    if (std::abs(ndc[0]) <= 2.0f && std::abs(ndc[1]) <= 2.0f && ndc[2] >= -10.0f &&
        ndc[2] <= 10.0f) {
      ++inside_count;
    }
  }

  if (sampled_count == 0 || valid_count == 0) {
    return candidate;
  }

  const float finite_ratio = float(valid_count) / float(sampled_count);
  const float inside_ratio = float(inside_count) / float(valid_count);
  const float extent_x = maxs[0] - mins[0];
  const float extent_y = maxs[1] - mins[1];
  const float extent_z = maxs[2] - mins[2];
  const float xy_extent = extent_x + extent_y;
  const float xy_area = extent_x * extent_y;
  const float score = finite_ratio + inside_ratio + std::min(xy_extent, 4.0f) * 0.25f +
                      std::min(xy_area, 4.0f) * 0.10f +
                      std::min(extent_z, 10.0f) * 0.005f;

  candidate.valid = finite_ratio >= 0.5f && inside_ratio >= 0.25f;
  candidate.finite_ratio = finite_ratio;
  candidate.inside_ratio = inside_ratio;
  candidate.score = score;
  candidate.ndc_mins = mins;
  candidate.ndc_maxs = maxs;
  return candidate;
}

ProjectionCandidate ScoreNativeReplayProjectionRows(
    const std::vector<gpu_replay::ReplayVertex>& vertices,
    const std::array<std::array<float, 4>, 4>& rows,
    const std::array<uint32_t, 4>& constant_indices, bool column_major, const char* source) {
  ProjectionCandidate candidate;
  candidate.column_major = column_major;
  candidate.source = source;
  candidate.constant_indices = constant_indices;
  candidate.rows = rows;
  return ScoreNativeReplayProjectionCandidate(candidate, vertices);
}

const rex::graphics::native_render::FloatConstantSummary* FindNativeReplayFloatConstant(
    const DrawEvent& event, uint32_t constant_index) {
  const uint32_t constant_count =
      std::min(event.vertex_float_constant_summary_count,
               rex::graphics::native_render::kMaxFloatConstantSummariesPerDraw);
  for (uint32_t i = 0; i < constant_count; ++i) {
    if (event.vertex_float_constants[i].constant_index == constant_index) {
      return &event.vertex_float_constants[i];
    }
  }
  return nullptr;
}

const rex::graphics::native_render::FloatConstantSummary* FindNativeReplayPixelFloatConstant(
    const DrawEvent& event, uint32_t constant_index) {
  const uint32_t constant_count =
      std::min(event.pixel_float_constant_summary_count,
               rex::graphics::native_render::kMaxFloatConstantSummariesPerDraw);
  for (uint32_t i = 0; i < constant_count; ++i) {
    if (event.pixel_float_constants[i].constant_index == constant_index) {
      return &event.pixel_float_constants[i];
    }
  }
  return nullptr;
}

bool BuildNativeReplaySwizzledProjectionRows(
    const DrawEvent& event, const std::array<uint32_t, 4>& constant_indices,
    bool negate_y_row, std::array<std::array<float, 4>, 4>& rows) {
  rows = {};
  for (uint32_t row = 0; row < 4; ++row) {
    const auto* constant = FindNativeReplayFloatConstant(event, constant_indices[row]);
    if (!constant) {
      return false;
    }
    const float sign = negate_y_row && row == 1 ? -1.0f : 1.0f;
    rows[row] = {sign * constant->values[2], sign * constant->values[1],
                 sign * constant->values[0], sign * constant->values[3]};
    for (float value : rows[row]) {
      if (!std::isfinite(value)) {
        return false;
      }
    }
  }
  return true;
}

bool BuildNativeReplayDirectProjectionRows(
    const DrawEvent& event, const std::array<uint32_t, 4>& constant_indices,
    bool negate_y_row, std::array<std::array<float, 4>, 4>& rows) {
  rows = {};
  for (uint32_t row = 0; row < 4; ++row) {
    const auto* constant = FindNativeReplayFloatConstant(event, constant_indices[row]);
    if (!constant) {
      return false;
    }
    const float sign = negate_y_row && row == 1 ? -1.0f : 1.0f;
    rows[row] = {sign * constant->values[0], sign * constant->values[1],
                 sign * constant->values[2], sign * constant->values[3]};
    for (float value : rows[row]) {
      if (!std::isfinite(value)) {
        return false;
      }
    }
  }
  return true;
}

bool BuildNativeReplayBone0UpstreamRows(const DrawEvent& event,
                                        std::array<uint32_t, 3>& constant_indices,
                                        std::array<std::array<float, 4>, 3>& rows) {
  constant_indices = {6, 5, 4};
  for (uint32_t row = 0; row < 3; ++row) {
    const auto* constant = FindNativeReplayFloatConstant(event, constant_indices[row]);
    if (!constant) {
      return false;
    }
    rows[row] = {constant->values[3], constant->values[2], constant->values[0],
                 constant->values[1]};
    for (float value : rows[row]) {
      if (!std::isfinite(value)) {
        return false;
      }
    }
  }
  return true;
}

bool CopyNativeReplaySharedShaderConstants(const DrawEvent& event,
                                           ProjectionCandidate& candidate) {
  candidate.shared_shader_constant_present.fill(false);
  candidate.shared_shader_constants = {};

  const uint32_t constant_count =
      std::min(event.vertex_float_constant_summary_count,
               rex::graphics::native_render::kMaxFloatConstantSummariesPerDraw);
  for (uint32_t i = 0; i < constant_count; ++i) {
    const auto& constant = event.vertex_float_constants[i];
    if (constant.constant_index >= candidate.shared_shader_constant_present.size()) {
      continue;
    }
    candidate.shared_shader_constant_present[constant.constant_index] = true;
    for (uint32_t component = 0; component < 4; ++component) {
      candidate.shared_shader_constants[constant.constant_index][component] =
          constant.values[component];
      if (!std::isfinite(constant.values[component])) {
        return false;
      }
    }
  }
  return true;
}

ProjectionCandidate FindNativeReplayShaderFinalProjectionCandidate(
    const DrawEvent& event, const std::vector<gpu_replay::ReplayVertex>& vertices,
    bool include_bone0_upstream, bool include_shared_shader_skin) {
  std::array<uint32_t, 4> constants = {};
  bool negate_y_row = false;
  bool direct_projection_rows = false;
  bool supports_bone0_upstream = false;
  bool supports_shared_shader_skin = false;
  const char* source = nullptr;
  switch (event.vertex_shader_hash) {
    case 0xED8D12865D27DEBFull:
    case 0x45C4DDDAAA10F75Full:
      constants = {0, 1, 2, 3};
      negate_y_row = true;
      supports_bone0_upstream = true;
      supports_shared_shader_skin = true;
      source = include_shared_shader_skin ? "shader-skinned-c4-c6-c0-c3"
                                          : include_bone0_upstream ? "shader-bone0-c4-c6-c0-c3"
                                                                   : "shader-final-c0-c3";
      break;
    case 0x1A2E173CABDD3E80ull:
      constants = {3, 4, 5, 6};
      source = "shader-final-c3-c6";
      break;
    case 0xD5CCD0C915DDCC0Bull:
      constants = {7, 8, 9, 10};
      direct_projection_rows = true;
      source = "shader-direct-c7-c10";
      break;
    default:
      return {};
  }
  include_bone0_upstream = include_bone0_upstream && supports_bone0_upstream;
  include_shared_shader_skin = include_shared_shader_skin && supports_shared_shader_skin;

  std::array<std::array<float, 4>, 4> rows = {};
  const bool built_rows =
      direct_projection_rows
          ? BuildNativeReplayDirectProjectionRows(event, constants, negate_y_row, rows)
          : BuildNativeReplaySwizzledProjectionRows(event, constants, negate_y_row, rows);
  if (!built_rows) {
    return {};
  }
  ProjectionCandidate candidate;
  candidate.column_major = false;
  candidate.source = source;
  candidate.constant_indices = constants;
  candidate.rows = rows;
  if (include_bone0_upstream &&
      !BuildNativeReplayBone0UpstreamRows(event, candidate.upstream_constant_indices,
                                          candidate.upstream_rows)) {
    return {};
  }
  candidate.has_upstream_transform = include_bone0_upstream;
  if (include_shared_shader_skin && !CopyNativeReplaySharedShaderConstants(event, candidate)) {
    return {};
  }
  candidate.use_shared_shader_skin = include_shared_shader_skin;
  candidate = ScoreNativeReplayProjectionCandidate(candidate, vertices);
  if (!candidate.valid && event.vertex_shader_hash == 0xD5CCD0C915DDCC0Bull &&
      candidate.finite_ratio >= 0.5f) {
    candidate.valid = true;
  }
  if (!candidate.valid &&
      REXCVAR_GET(sw2e_native_renderer_gpu_replay_normalize_projected_gaps) &&
      candidate.finite_ratio >= 0.5f) {
    candidate.valid = true;
  }
  return candidate;
}

ProjectionCandidate FindNativeReplayHeuristicProjectionCandidate(
    const DrawEvent& event, const std::vector<gpu_replay::ReplayVertex>& vertices) {
  ProjectionCandidate best;
  const uint32_t constant_count =
      std::min(event.vertex_float_constant_summary_count,
               kNativeGpuReplayProjectionHeuristicConstantLimit);
  if (constant_count < 4 || vertices.size() < 3) {
    return best;
  }

  for (uint32_t a = 0; a + 3 < constant_count; ++a) {
    for (uint32_t b = a + 1; b + 2 < constant_count; ++b) {
      for (uint32_t c = b + 1; c + 1 < constant_count; ++c) {
        for (uint32_t d = c + 1; d < constant_count; ++d) {
          const std::array<uint32_t, 4> constants = {a, b, c, d};
          std::array<std::array<float, 4>, 4> rows = {};
          std::array<uint32_t, 4> constant_indices = {};
          bool constants_finite = true;
          for (uint32_t row = 0; row < 4; ++row) {
            const auto& constant = event.vertex_float_constants[constants[row]];
            constant_indices[row] = constant.constant_index;
            for (uint32_t component = 0; component < 4; ++component) {
              rows[row][component] = constant.values[component];
              constants_finite = constants_finite && std::isfinite(rows[row][component]);
            }
          }
          if (!constants_finite) {
            continue;
          }

          for (bool column_major : {false, true}) {
            ProjectionCandidate candidate = ScoreNativeReplayProjectionRows(
                vertices, rows, constant_indices, column_major, "heuristic");
            if ((candidate.valid && (!best.valid || candidate.score > best.score)) ||
                (!best.valid && !candidate.valid && candidate.score > best.score)) {
              best = candidate;
            }
          }
        }
      }
    }
  }

  if (best.finite_ratio < 0.5f || best.inside_ratio < 0.25f) {
    best.valid = false;
  }
  return best;
}

ProjectionCandidate FindNativeReplayProjectionCandidate(
    const DrawEvent& event, const std::vector<gpu_replay::ReplayVertex>& vertices) {
  const std::string strategy =
      std::string(REXCVAR_GET(sw2e_native_renderer_gpu_replay_projection_strategy));
  const bool use_shader_final =
      strategy == "shader-final" || strategy == "shader-final-or-heuristic";
  const bool use_shader_bone0 = strategy == "shader-bone0-final";
  const bool use_shader_skinned = strategy == "shader-skinned-final";
  const bool use_heuristic = strategy.empty() || strategy == "heuristic" ||
                             strategy == "shader-final-or-heuristic";

  ProjectionCandidate best;
  if (use_shader_final || use_shader_bone0 || use_shader_skinned) {
    best = FindNativeReplayShaderFinalProjectionCandidate(event, vertices, use_shader_bone0,
                                                          use_shader_skinned);
  }
  if (use_heuristic) {
    ProjectionCandidate heuristic = FindNativeReplayHeuristicProjectionCandidate(event, vertices);
    if (!best.valid || (heuristic.valid && heuristic.score > best.score)) {
      best = heuristic;
    }
  }
  return best;
}

bool ProjectNativeReplayVertex(gpu_replay::ReplayVertex& vertex,
                               const ProjectionCandidate& projection) {
  const std::array<float, 4> clip =
      TransformNativeReplayPositionWithCandidate(vertex, projection);
  if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) || !std::isfinite(clip[2]) ||
      !std::isfinite(clip[3]) || std::abs(clip[3]) < 1e-6f) {
    return false;
  }

  vertex.x = clip[0] / clip[3];
  vertex.y = clip[1] / clip[3];
  vertex.z = 0.5f;
  vertex.w = 1.0f;
  return std::isfinite(vertex.x) && std::isfinite(vertex.y);
}

bool FitReplayVerticesForDebug(std::vector<gpu_replay::ReplayVertex>& vertices) {
  std::array<float, 3> mins = {std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max()};
  std::array<float, 3> maxs = {-std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max()};
  uint32_t valid_count = 0;
  for (const auto& vertex : vertices) {
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
      continue;
    }
    const std::array<float, 3> position = {vertex.x, vertex.y, vertex.z};
    for (uint32_t axis = 0; axis < 3; ++axis) {
      mins[axis] = std::min(mins[axis], position[axis]);
      maxs[axis] = std::max(maxs[axis], position[axis]);
    }
    ++valid_count;
  }

  if (valid_count < 3) {
    return false;
  }

  uint32_t axis_x = 0;
  uint32_t axis_y = 1;
  float best_area = -1.0f;
  for (const auto axes : {std::array<uint32_t, 2>{0, 1}, std::array<uint32_t, 2>{0, 2},
                          std::array<uint32_t, 2>{1, 2}}) {
    const float extent_a = maxs[axes[0]] - mins[axes[0]];
    const float extent_b = maxs[axes[1]] - mins[axes[1]];
    const float area = extent_a * extent_b;
    if (std::isfinite(area) && area > best_area) {
      best_area = area;
      axis_x = axes[0];
      axis_y = axes[1];
    }
  }

  const float extent_x = maxs[axis_x] - mins[axis_x];
  const float extent_y = maxs[axis_y] - mins[axis_y];
  const float max_extent = std::max(extent_x, extent_y);
  if (!std::isfinite(max_extent) || max_extent < 1e-6f) {
    return false;
  }

  const float center_x = (mins[axis_x] + maxs[axis_x]) * 0.5f;
  const float center_y = (mins[axis_y] + maxs[axis_y]) * 0.5f;
  const float scale = 1.65f / max_extent;
  for (auto& vertex : vertices) {
    const std::array<float, 3> position = {vertex.x, vertex.y, vertex.z};
    vertex.x = (position[axis_x] - center_x) * scale;
    vertex.y = (position[axis_y] - center_y) * scale;
    vertex.z = 0.5f;
    vertex.w = 1.0f;
  }
  return true;
}

bool FitReplayProjectedVerticesXYForDebug(std::vector<gpu_replay::ReplayVertex>& vertices) {
  std::array<float, 2> mins = {std::numeric_limits<float>::max(),
                               std::numeric_limits<float>::max()};
  std::array<float, 2> maxs = {-std::numeric_limits<float>::max(),
                               -std::numeric_limits<float>::max()};
  uint32_t valid_count = 0;
  for (const auto& vertex : vertices) {
    if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y)) {
      continue;
    }
    mins[0] = std::min(mins[0], vertex.x);
    mins[1] = std::min(mins[1], vertex.y);
    maxs[0] = std::max(maxs[0], vertex.x);
    maxs[1] = std::max(maxs[1], vertex.y);
    ++valid_count;
  }

  if (valid_count < 3) {
    return false;
  }

  const float extent_x = maxs[0] - mins[0];
  const float extent_y = maxs[1] - mins[1];
  const float max_extent = std::max(extent_x, extent_y);
  if (!std::isfinite(max_extent) || max_extent < 1e-6f) {
    return false;
  }

  const float center_x = (mins[0] + maxs[0]) * 0.5f;
  const float center_y = (mins[1] + maxs[1]) * 0.5f;
  const float scale = 1.65f / max_extent;
  for (auto& vertex : vertices) {
    vertex.x = (vertex.x - center_x) * scale;
    vertex.y = (vertex.y - center_y) * scale;
    vertex.z = std::isfinite(vertex.z) ? std::clamp(vertex.z, 0.0f, 1.0f) : 0.5f;
    vertex.w = 1.0f;
  }
  return true;
}

uint32_t IndexElementSizeBytes(uint32_t index_format) { return index_format == 0 ? 2u : 4u; }

uint32_t IndexBytesNeeded(const DrawEvent& event) {
  if (!event.indexed || event.index_count == 0 || event.index_length == 0) {
    return 0;
  }

  const uint64_t needed = uint64_t(event.index_count) * IndexElementSizeBytes(event.index_format);
  const uint64_t bounded = std::min<uint64_t>(needed, event.index_length);
  return static_cast<uint32_t>(std::min<uint64_t>(bounded, std::numeric_limits<uint32_t>::max()));
}

uint16_t ReadLe16(const uint8_t* data) {
  uint16_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

uint32_t ReadLe32(const uint8_t* data) {
  uint32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

uint16_t SwapIndex16(uint16_t value, uint32_t endian) {
  if (endian == 1) {
    return static_cast<uint16_t>((value << 8) | (value >> 8));
  }
  return value;
}

uint32_t SwapIndex32(uint32_t value, uint32_t endian) {
  switch (endian) {
    case 1:
      return ((value << 8) & 0xFF00FF00u) | ((value >> 8) & 0x00FF00FFu);
    case 2:
      return (value << 24) | ((value << 8) & 0x00FF0000u) |
             ((value >> 8) & 0x0000FF00u) | (value >> 24);
    case 3:
      return (value << 16) | (value >> 16);
    default:
      return value;
  }
}

bool IsNativeReplayRestartIndex(uint32_t index, uint32_t index_format) {
  return index_format == 0 ? index == 0xFFFFu : index == 0x00FFFFFFu;
}

bool DecodeNativeReplayIndices(const DrawEvent& event, const rex::memory::Memory* memory,
                               std::vector<uint32_t>& indices, uint32_t& max_index) {
  indices.clear();
  max_index = 0;
  const uint32_t index_bytes_needed = IndexBytesNeeded(event);
  if (!memory || index_bytes_needed == 0) {
    return false;
  }

  std::vector<uint8_t> index_bytes;
  if (!CopyPhysicalMemorySample(memory, event.index_guest_base, index_bytes_needed, index_bytes)) {
    return false;
  }

  indices.reserve(event.index_count);
  const uint32_t element_size = IndexElementSizeBytes(event.index_format);
  for (uint32_t i = 0; i < event.index_count; ++i) {
    const uint8_t* element = index_bytes.data() + size_t(i) * element_size;
    uint32_t index = 0;
    if (event.index_format == 0) {
      index = SwapIndex16(ReadLe16(element), event.index_endianness);
    } else {
      index = SwapIndex32(ReadLe32(element), event.index_endianness) & 0x00FFFFFFu;
    }
    indices.push_back(index);
    if (!IsNativeReplayRestartIndex(index, event.index_format)) {
      max_index = std::max(max_index, index);
    }
  }
  return true;
}

bool ExpandNativeReplayTriangleStripIndices(const std::vector<uint32_t>& strip_indices,
                                            std::vector<uint32_t>& triangle_indices,
                                            uint32_t index_format = 0) {
  triangle_indices.clear();
  if (strip_indices.size() < 3) {
    return false;
  }

  triangle_indices.reserve((strip_indices.size() - 2) * 3);
  size_t segment_start = 0;
  for (size_t i = 0; i <= strip_indices.size(); ++i) {
    if (i != strip_indices.size() &&
        !IsNativeReplayRestartIndex(strip_indices[i], index_format)) {
      continue;
    }

    for (size_t j = segment_start; j + 2 < i; ++j) {
      const uint32_t a = strip_indices[j + 0];
      const uint32_t b = strip_indices[j + 1];
      const uint32_t c = strip_indices[j + 2];
      if (a == b || a == c || b == c) {
        continue;
      }

      if (((j - segment_start) & 1) == 0) {
        triangle_indices.push_back(a);
        triangle_indices.push_back(b);
        triangle_indices.push_back(c);
      } else {
        triangle_indices.push_back(b);
        triangle_indices.push_back(a);
        triangle_indices.push_back(c);
      }
    }
    segment_start = i + 1;
  }

  return !triangle_indices.empty();
}

bool BuildNativeReplayTriangleStripIndices(const DrawEvent& event,
                                           std::vector<uint32_t>& triangle_indices) {
  std::vector<uint32_t> strip_indices;
  strip_indices.reserve(event.index_count);
  for (uint32_t i = 0; i < event.index_count; ++i) {
    strip_indices.push_back(i);
  }
  return ExpandNativeReplayTriangleStripIndices(strip_indices, triangle_indices);
}

std::array<float, 4> NativeGpuReplaySolidColor(const DrawEvent& event) {
  if (event.pixel_float_constant_summary_count != 0) {
    const auto& constant = event.pixel_float_constants[0];
    return {constant.values[0], constant.values[1], constant.values[2], constant.values[3]};
  }
  return {0.0f, 0.0f, 0.0f, 1.0f};
}

void MixHash(uint64_t& target, uint64_t hash) {
  if (!hash) {
    return;
  }
  target ^= hash + 0x9E3779B97F4A7C15ull + (target << 6) + (target >> 2);
}

uint32_t FloatBits(float value) {
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

void WriteJsonFloat(FILE* file, float value) {
  if (std::isfinite(value)) {
    std::fprintf(file, "%.9g", static_cast<double>(value));
    return;
  }
  std::fputs("null", file);
}

uint64_t TransformGapAttributeSignature(const VertexFetchSummary& fetch) {
  uint64_t signature = 0;
  const uint32_t attribute_count =
      std::min(fetch.attribute_summary_count,
               rex::graphics::native_render::kMaxVertexAttributeSummariesPerFetch);
  MixHash(signature, fetch.stride_words);
  MixHash(signature, attribute_count);
  for (uint32_t i = 0; i < attribute_count; ++i) {
    const VertexAttributeSummary& attribute = fetch.attributes[i];
    MixHash(signature, attribute.data_format);
    MixHash(signature, static_cast<uint32_t>(attribute.offset_words));
    MixHash(signature, static_cast<uint32_t>(attribute.exp_adjust));
    MixHash(signature, attribute.is_signed ? 1 : 2);
    MixHash(signature, attribute.is_integer ? 3 : 4);
    MixHash(signature, attribute.result_storage_target);
    MixHash(signature, attribute.result_storage_index);
    MixHash(signature, attribute.result_write_mask);
    MixHash(signature, attribute.result_used_components);
    MixHash(signature, attribute.result_swizzle);
  }
  return signature;
}

const VertexFetchSummary* FirstUsefulVertexFetch(const DrawEvent& event) {
  for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
    const auto& candidate = event.vertex_fetches[i];
    if (candidate.attribute_summary_count != 0) {
      return &candidate;
    }
  }
  return event.vertex_fetch_summary_count != 0 ? &event.vertex_fetches[0] : nullptr;
}

uint64_t SampleKey(uint32_t address, uint32_t size, uint64_t hash, char kind) {
  uint64_t key = hash;
  key ^= uint64_t(address) << 32;
  key ^= uint64_t(size) << 8;
  key ^= static_cast<uint8_t>(kind);
  return key;
}

std::filesystem::path SamplePath(char kind, uint32_t frame_index, uint32_t draw_index,
                                 uint32_t fetch_constant, uint32_t address, uint32_t size,
                                 uint64_t hash) {
  char name[160];
  std::snprintf(name, sizeof(name),
                "%c_f%06u_d%04u_c%03u_a%08x_s%05u_h%016llx.bin", kind, frame_index,
                draw_index, fetch_constant, address, size,
                static_cast<unsigned long long>(hash));

  return rex::to_path(REXCVAR_GET(sw2e_native_renderer_sample_root)) / name;
}

uint32_t AlignUp(uint32_t value, uint32_t alignment) {
  return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
}

uint64_t AlignUp64(uint64_t value, uint64_t alignment) {
  return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
}

struct TextureDumpPlan {
  uint32_t requested_size = 0;
  uint32_t expected_full_size = 0;
  uint32_t row_pitch_bytes = 0;
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  uint32_t array_size = 0;
  uint32_t block_bytes = 0;
  bool known_pc_format = false;
  bool requested_full = false;
};

TextureDumpPlan BuildTextureDumpPlan(const TextureFetchSummary& fetch, uint32_t sample_limit) {
  TextureDumpPlan plan;
  plan.requested_size = sample_limit;

  if (fetch.dimension != kXenosDimension2DOrStacked || fetch.width == 0 || fetch.height == 0) {
    return plan;
  }

  const uint32_t pitch_texels = fetch.pitch ? (fetch.pitch << 5) : fetch.width;
  uint64_t row_pitch_bytes64 = 0;
  uint64_t full_size = 0;
  if (fetch.format == kXenosFormatDxt45 && fetch.tiled == 0) {
    plan.known_pc_format = true;
    plan.block_bytes = kBc3BytesPerBlock;
    plan.width_blocks = AlignUp(fetch.width, kBc3BlockWidth) / kBc3BlockWidth;
    plan.height_blocks = AlignUp(fetch.height, kBc3BlockHeight) / kBc3BlockHeight;
    plan.array_size = std::max(fetch.depth, uint32_t(1));

    const uint32_t row_pitch_blocks =
        AlignUp(AlignUp(std::max(pitch_texels, fetch.width), kBc3BlockWidth) / kBc3BlockWidth,
                kTextureTileWidthHeightBlocks);
    row_pitch_bytes64 = uint64_t(row_pitch_blocks) * kBc3BytesPerBlock;
    const uint64_t one_slice_data_extent =
        row_pitch_bytes64 * (plan.height_blocks - 1) +
        uint64_t(plan.width_blocks) * kBc3BytesPerBlock;
    const uint64_t slice_stride =
        AlignUp64(row_pitch_bytes64 * AlignUp(plan.height_blocks, kTextureTileWidthHeightBlocks),
                  kTextureSubresourceAlignmentBytes);
    full_size = slice_stride * (plan.array_size - 1) + one_slice_data_extent;
  } else if (fetch.format == kXenosFormat8888) {
    plan.known_pc_format = true;
    plan.block_bytes = 4;
    plan.width_blocks = fetch.width;
    plan.height_blocks = fetch.height;
    plan.array_size = std::max(fetch.depth, uint32_t(1));

    const uint32_t row_pitch_texels =
        fetch.tiled ? AlignUp(std::max(pitch_texels, fetch.width), kTextureTileWidthHeightBlocks)
                    : std::max(pitch_texels, fetch.width);
    row_pitch_bytes64 = uint64_t(row_pitch_texels) * plan.block_bytes;
    const uint64_t one_slice_data_extent =
        row_pitch_bytes64 * (plan.height_blocks - 1) +
        uint64_t(plan.width_blocks) * plan.block_bytes;
    const uint64_t slice_stride =
        fetch.tiled ? AlignUp64(row_pitch_bytes64 *
                                    AlignUp(plan.height_blocks, kTextureTileWidthHeightBlocks),
                                kTextureSubresourceAlignmentBytes)
                    : row_pitch_bytes64 * plan.height_blocks;
    full_size = fetch.tiled ? slice_stride * plan.array_size
                            : slice_stride * (plan.array_size - 1) + one_slice_data_extent;
  } else {
    return plan;
  }

  if (row_pitch_bytes64 > UINT32_MAX || full_size > UINT32_MAX) {
    return plan;
  }

  plan.row_pitch_bytes = static_cast<uint32_t>(row_pitch_bytes64);
  plan.expected_full_size = static_cast<uint32_t>(full_size);

  const int32_t max_full_texture_bytes =
      REXCVAR_GET(sw2e_native_renderer_full_texture_max_bytes);
  if (REXCVAR_GET(sw2e_native_renderer_dump_full_textures) && max_full_texture_bytes > 0 &&
      plan.expected_full_size <= static_cast<uint32_t>(max_full_texture_bytes)) {
    plan.requested_size = plan.expected_full_size;
    plan.requested_full = true;
  }

  return plan;
}

uint64_t NativeGpuReplayTextureBytesKey(const TextureFetchSummary& fetch,
                                        const TextureDumpPlan& plan) {
  uint64_t key = 0;
  MixHash(key, fetch.base_address_bytes);
  MixHash(key, fetch.width);
  MixHash(key, fetch.height);
  MixHash(key, fetch.depth);
  MixHash(key, fetch.pitch);
  MixHash(key, fetch.format);
  MixHash(key, fetch.dimension);
  MixHash(key, fetch.endian);
  MixHash(key, fetch.tiled);
  MixHash(key, plan.expected_full_size);
  MixHash(key, plan.row_pitch_bytes);
  return key;
}

const char* NativeGpuReplayDrawKindName(gpu_replay::ReplayDrawKind kind) {
  switch (kind) {
    case gpu_replay::ReplayDrawKind::kTexturedTriangles:
      return "textured";
    case gpu_replay::ReplayDrawKind::kSolidTriangles:
      return "solid";
    case gpu_replay::ReplayDrawKind::kProjectedTexturedTriangles:
      return "projected";
    case gpu_replay::ReplayDrawKind::kDepthOnlyTriangles:
      return "depth_only";
  }
  return "unknown";
}

uint32_t NativeGpuReplayDrawScore(const gpu_replay::ReplayDraw& draw) {
  const uint32_t kind_score =
      draw.kind == gpu_replay::ReplayDrawKind::kProjectedTexturedTriangles
          ? 100000u
          : (draw.kind == gpu_replay::ReplayDrawKind::kTexturedTriangles
                 ? 1000u
                 : (draw.kind == gpu_replay::ReplayDrawKind::kDepthOnlyTriangles ? 10u : 0u));
  const uint32_t vertex_score =
      std::min<uint32_t>(static_cast<uint32_t>(draw.vertices.size()), 100000u);
  const uint32_t index_score =
      std::min<uint32_t>(static_cast<uint32_t>(draw.indices.size()), 100000u);
  return kind_score + vertex_score + index_score;
}

uint32_t NativeGpuReplayPassScore(const std::vector<gpu_replay::ReplayDraw>& draws) {
  uint32_t score = static_cast<uint32_t>(draws.size());
  for (const auto& draw : draws) {
    score = std::min<uint32_t>(score + NativeGpuReplayDrawScore(draw), UINT32_MAX - 1);
  }
  return score;
}

bool NativeGpuReplayDrawHasColorOutput(const gpu_replay::ReplayDraw& draw) {
  return draw.kind != gpu_replay::ReplayDrawKind::kDepthOnlyTriangles &&
         (draw.rt0_write_mask & 0x0F) != 0;
}

bool NativeGpuReplayPassHasColorOutput(const std::vector<gpu_replay::ReplayDraw>& draws) {
  return std::any_of(draws.begin(), draws.end(), NativeGpuReplayDrawHasColorOutput);
}

std::optional<uint64_t> ParseNativeGpuReplayHashFilter(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }
  if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  uint64_t value = 0;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, value, 16);
  if (result.ec != std::errc() || result.ptr != end) {
    return std::nullopt;
  }
  return value;
}

bool NativeGpuReplayHashFilterMatches(std::string_view filter_text, uint64_t actual_hash) {
  if (filter_text.empty()) {
    return true;
  }
  const std::optional<uint64_t> filter = ParseNativeGpuReplayHashFilter(filter_text);
  return filter && *filter == actual_hash;
}

const VertexFetchSummary* FindNativeReplayTexturedVertexFetch(const DrawEvent& event) {
  for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
    const auto& candidate = event.vertex_fetches[i];
    if (IsNativeReplayScreenSpaceTexturedVertexFetch(candidate) &&
        CanDecodeNativeReplayTexturedVertexFetch(candidate) &&
        (event.indexed || candidate.size_bytes >= event.index_count * candidate.stride_words * 4)) {
      return &candidate;
    }
  }
  return nullptr;
}

const VertexFetchSummary* FindDecodableNativeReplayTexturedVertexFetch(const DrawEvent& event) {
  for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
    const auto& candidate = event.vertex_fetches[i];
    if (CanDecodeNativeReplayTexturedVertexFetch(candidate) &&
        (event.indexed || candidate.size_bytes >= event.index_count * candidate.stride_words * 4)) {
      return &candidate;
    }
  }
  return nullptr;
}

const VertexFetchSummary* FindNativeReplayConstantSelectorVertexFetch(const DrawEvent& event) {
  if (event.vertex_shader_hash != 0xDE7F9AF93C668314ull ||
      event.pixel_shader_hash != 0x8CBAD34FCE165328ull || event.indexed ||
      event.vertex_fetch_summary_count == 0 || event.index_count < 3) {
    return nullptr;
  }
  for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
    const auto& candidate = event.vertex_fetches[i];
    if (candidate.stride_words == 1 && candidate.attribute_count == 1 &&
        candidate.size_bytes >= event.index_count * sizeof(float)) {
      return &candidate;
    }
  }
  return nullptr;
}

bool CanReplayNativeConstantScreenQuadDraw(const DrawEvent& event) {
  return FindNativeReplayConstantSelectorVertexFetch(event) != nullptr;
}

bool IsNativeReplayTexturedTriangleShape(const DrawEvent& event) {
  if (event.index_count < 3 || event.vertex_fetch_summary_count == 0 ||
      event.texture_fetch_summary_count == 0) {
    return false;
  }
  if (event.indexed && IndexBytesNeeded(event) == 0) {
    return false;
  }
  if (event.primitive_type == kXenosPrimitiveTriangleList) {
    return (event.index_count % 3) == 0;
  }
  if (event.primitive_type == kXenosPrimitiveTriangleStrip) {
    return true;
  }
  return false;
}

bool IsPrioritySampleDraw(const DrawEvent& event) {
  if (event.indexed || event.primitive_type == kXenosPrimitiveTriangleStrip ||
      event.texture_fetch_summary_count > 1) {
    return true;
  }
  for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
    if (event.vertex_fetches[i].stride_words >= 8) {
      return true;
    }
  }
  return false;
}

const TextureFetchSummary* FindNativeReplayTextureFetch(const DrawEvent& event,
                                                        std::optional<TextureDumpPlan>& plan_out) {
  plan_out.reset();
  for (uint32_t i = 0; i < event.texture_fetch_summary_count; ++i) {
    const auto& candidate = event.texture_fetches[i];
    TextureDumpPlan plan = BuildTextureDumpPlan(candidate, 0);
    if (!plan.known_pc_format || plan.expected_full_size == 0) {
      continue;
    }
    const int32_t max_texture_bytes = REXCVAR_GET(sw2e_native_renderer_full_texture_max_bytes);
    if (max_texture_bytes <= 0 ||
        plan.expected_full_size > static_cast<uint32_t>(max_texture_bytes)) {
      continue;
    }
    plan_out = plan;
    return &candidate;
  }
  return nullptr;
}

const VertexFetchSummary* FindNativeReplaySolidVertexFetch(const DrawEvent& event) {
  for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
    const auto& candidate = event.vertex_fetches[i];
    if ((candidate.stride_words == 2 || candidate.stride_words == 7) &&
        candidate.size_bytes >= event.index_count * candidate.stride_words * 4) {
      return &candidate;
    }
  }
  return nullptr;
}

enum class NativeReplaySupport {
  kNoOutputSkipCandidate,
  kSupportedTextured,
  kSupportedIndexedTextured,
  kSupportedSolid,
  kSupportedDepthOnly,
  kSupportedProjectedTransform,
  kDepthOnly,
  kUnsupportedIndexed,
  kUnsupportedShape,
  kUnsupportedLayout,
  kUnsupportedTexture,
  kUnsupportedTransform,
};

const char* NativeReplaySupportName(NativeReplaySupport support) {
  switch (support) {
    case NativeReplaySupport::kNoOutputSkipCandidate:
      return "no_output_skip";
    case NativeReplaySupport::kSupportedTextured:
      return "supported_textured";
    case NativeReplaySupport::kSupportedIndexedTextured:
      return "supported_indexed_textured";
    case NativeReplaySupport::kSupportedSolid:
      return "supported_solid";
    case NativeReplaySupport::kSupportedDepthOnly:
      return "supported_depth_only";
    case NativeReplaySupport::kSupportedProjectedTransform:
      return "supported_projected_transform";
    case NativeReplaySupport::kDepthOnly:
      return "depth_only";
    case NativeReplaySupport::kUnsupportedIndexed:
      return "unsupported_indexed";
    case NativeReplaySupport::kUnsupportedShape:
      return "unsupported_shape";
    case NativeReplaySupport::kUnsupportedLayout:
      return "unsupported_layout";
    case NativeReplaySupport::kUnsupportedTexture:
      return "unsupported_texture";
    case NativeReplaySupport::kUnsupportedTransform:
      return "unsupported_transform";
    default:
      return "unknown";
  }
}

bool IsNativeReplayGapSupport(NativeReplaySupport support) {
  return support == NativeReplaySupport::kUnsupportedLayout ||
         support == NativeReplaySupport::kUnsupportedTransform;
}

bool IsNativeReplaySolidShape(const DrawEvent& event) {
  return (event.primitive_type == 8 && event.index_count == 3) ||
         (event.primitive_type == 6 && event.index_count == 4);
}

bool CanReplayNativeDepthOnlyDraw(const DrawEvent& event) {
  return event.output_merger_writes && !HasNativeReplayColorOutput(event) &&
         event.vertex_memexport_mask == 0 && event.pixel_memexport_mask == 0 &&
         !HasVizQuerySideEffect(event) && !event.indexed && IsNativeReplaySolidShape(event) &&
         event.texture_fetch_summary_count == 0 && event.vertex_fetch_summary_count != 0 &&
         FindNativeReplaySolidVertexFetch(event);
}

bool CanReplayNativeD5ProjectedTransformDraw(const DrawEvent& event) {
  if (event.vertex_shader_hash != 0xD5CCD0C915DDCC0Bull ||
      event.pixel_shader_hash != 0x7B81C162CBA6D195ull ||
      !HasNativeReplayColorOutput(event) || event.vertex_memexport_mask != 0 ||
      event.pixel_memexport_mask != 0 || HasVizQuerySideEffect(event) ||
      !IsNativeReplayTexturedTriangleShape(event)) {
    return false;
  }

  const VertexFetchSummary* vertex_fetch = FindDecodableNativeReplayTexturedVertexFetch(event);
  if (!vertex_fetch || IsNativeReplayScreenSpaceTexturedVertexFetch(*vertex_fetch)) {
    return false;
  }

  std::optional<TextureDumpPlan> texture_plan;
  return FindNativeReplayTextureFetch(event, texture_plan) != nullptr;
}

NativeReplaySupport ClassifyNativeReplaySupport(const DrawEvent& event) {
  if (IsNativeNoOutputSkipCandidate(event)) {
    return NativeReplaySupport::kNoOutputSkipCandidate;
  }
  if (!HasNativeReplayColorOutput(event)) {
    if (CanReplayNativeDepthOnlyDraw(event)) {
      return NativeReplaySupport::kSupportedDepthOnly;
    }
    return NativeReplaySupport::kDepthOnly;
  }
  if (IsNativeReplayTexturedTriangleShape(event)) {
    if (CanReplayNativeConstantScreenQuadDraw(event)) {
      std::optional<TextureDumpPlan> texture_plan;
      if (!FindNativeReplayTextureFetch(event, texture_plan)) {
        return NativeReplaySupport::kUnsupportedTexture;
      }
      return NativeReplaySupport::kSupportedTextured;
    }
    if (CanReplayNativeD5ProjectedTransformDraw(event)) {
      return NativeReplaySupport::kSupportedProjectedTransform;
    }
    if (!FindNativeReplayTexturedVertexFetch(event)) {
      if (FindDecodableNativeReplayTexturedVertexFetch(event)) {
        return NativeReplaySupport::kUnsupportedTransform;
      }
      return NativeReplaySupport::kUnsupportedLayout;
    }
    std::optional<TextureDumpPlan> texture_plan;
    if (!FindNativeReplayTextureFetch(event, texture_plan)) {
      return NativeReplaySupport::kUnsupportedTexture;
    }
    return event.indexed ? NativeReplaySupport::kSupportedIndexedTextured
                         : NativeReplaySupport::kSupportedTextured;
  }

  if (event.indexed) {
    return NativeReplaySupport::kUnsupportedIndexed;
  }

  if (IsNativeReplaySolidShape(event) && event.texture_fetch_summary_count == 0 &&
      event.vertex_fetch_summary_count != 0) {
    if (!FindNativeReplaySolidVertexFetch(event)) {
      return NativeReplaySupport::kUnsupportedLayout;
    }
    return NativeReplaySupport::kSupportedSolid;
  }

  return NativeReplaySupport::kUnsupportedShape;
}

class Sidecar final : public EventSink {
 public:
  ~Sidecar() override { CloseSampleMetadata(); }

  void SetNativeWindowHandle(void* native_window_handle) {
    native_window_handle_ = native_window_handle;
    if (REXCVAR_GET(sw2e_native_renderer_gpu_replay) &&
        REXCVAR_GET(sw2e_native_renderer_gpu_replay_live_present) && native_window_handle_) {
      gpu_replay::InitializeMenuReplayD3D11Child(native_window_handle_, 1280, 720);
    }
  }

  void Reset(uint32_t frame_index) {
    swap_count_ = 0;
    dumped_sample_keys_.clear();
    native_gpu_replay_draw_keys_.clear();
    native_gpu_replay_texture_bytes_cache_.clear();
    native_gpu_replay_draws_.clear();
    native_gpu_replay_last_completed_draws_.clear();
    native_gpu_replay_best_completed_draws_.clear();
    native_gpu_replay_best_completed_score_ = 0;
    native_gpu_replay_flushed_ = false;
    native_gpu_replay_live_present_count_ = 0;
    native_gpu_replay_captured_draw_log_count_ = 0;
    native_gpu_replay_projected_reject_log_count_ = 0;
    native_gpu_replay_completed_pass_count_ = 0;
    suppress_current_backend_swap_ = false;
    CloseSampleMetadata();
    ResetFrame(stats_, frame_index);
    if (REXCVAR_GET(sw2e_native_renderer_gpu_replay)) {
      REXLOG_INFO(
          "SW2E native GPU replay enabled: draw_limit={} solid={} transform_gaps={} "
          "transform_gaps_only={} transform_gap_min_vertices={} projected_min_indices={} "
          "projected_vs_filter={} projected_ps_filter={} projection_strategy={} debug_fit={} "
          "normalize_projected={} live_present={} output={}",
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_draw_limit),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_include_solid_geometry),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_include_transform_gaps),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_transform_gaps_only),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_transform_gap_min_vertices),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_projected_min_indices),
          std::string(REXCVAR_GET(
              sw2e_native_renderer_gpu_replay_projected_vertex_shader_filter)),
          std::string(REXCVAR_GET(
              sw2e_native_renderer_gpu_replay_projected_pixel_shader_filter)),
          std::string(REXCVAR_GET(sw2e_native_renderer_gpu_replay_projection_strategy)),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_debug_fit_projected_gaps),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_normalize_projected_gaps),
          REXCVAR_GET(sw2e_native_renderer_gpu_replay_live_present),
          std::string(REXCVAR_GET(sw2e_native_renderer_gpu_replay_path)));
    }
  }

  void CloseSampleMetadata() {
    if (!sample_metadata_file_) {
      return;
    }

    std::fflush(sample_metadata_file_);
    std::fclose(sample_metadata_file_);
    sample_metadata_file_ = nullptr;
    sample_metadata_root_.clear();
  }

  void FlushNativeGpuReplay() {
    if (native_gpu_replay_flushed_ || !REXCVAR_GET(sw2e_native_renderer_gpu_replay)) {
      return;
    }

    native_gpu_replay_flushed_ = true;
    const std::filesystem::path output_path =
        rex::to_path(REXCVAR_GET(sw2e_native_renderer_gpu_replay_path));
    if (output_path.empty()) {
      REXLOG_WARN("SW2E native GPU replay skipped because output path is empty");
      return;
    }

    const std::vector<gpu_replay::ReplayDraw>& replay_draws =
        !native_gpu_replay_best_completed_draws_.empty()
            ? native_gpu_replay_best_completed_draws_
            : (!native_gpu_replay_last_completed_draws_.empty() ? native_gpu_replay_last_completed_draws_
                                                                : native_gpu_replay_draws_);

    if (replay_draws.empty()) {
      REXLOG_WARN("SW2E native GPU replay skipped because no supported menu draws were captured");
      return;
    }

    gpu_replay::RenderMenuReplayD3D11(replay_draws, output_path, 1280, 720);
  }

  void OnNativeRenderDraw(const DrawEvent& event, const DrawEventContext& context) override {
    if (stats_.draws == 0 && stats_.frame_index != event.frame_index) {
      ResetFrame(stats_, event.frame_index);
    }

    ++stats_.draws;
    stats_.indexed_draws += event.indexed ? 1 : 0;
    stats_.vertex_fetch_draws += event.vertex_fetch_summary_count ? 1 : 0;
    stats_.texture_fetch_draws += event.texture_fetch_summary_count ? 1 : 0;
    stats_.memexport_draws +=
        (event.vertex_memexport_mask != 0 || event.pixel_memexport_mask != 0) ? 1 : 0;
    stats_.output_merger_write_draws += event.output_merger_writes ? 1 : 0;
    stats_.viz_query_draws += HasVizQuerySideEffect(event) ? 1 : 0;
    const bool no_output_skip_candidate = IsNativeNoOutputSkipCandidate(event);
    stats_.no_output_skip_candidate_draws += no_output_skip_candidate ? 1 : 0;
    stats_.no_output_point_list_draws +=
        no_output_skip_candidate && event.primitive_type == 1 ? 1 : 0;
    stats_.rasterized_no_output_draws +=
        no_output_skip_candidate && event.rasterization_potentially_done ? 1 : 0;
    const NativeReplaySupport replay_support = ClassifyNativeReplaySupport(event);
    switch (replay_support) {
      case NativeReplaySupport::kSupportedTextured:
        ++stats_.native_replay_supported_draws;
        ++stats_.native_replay_supported_textured_draws;
        break;
      case NativeReplaySupport::kSupportedIndexedTextured:
        ++stats_.native_replay_supported_draws;
        ++stats_.native_replay_supported_textured_draws;
        ++stats_.native_replay_supported_indexed_draws;
        break;
      case NativeReplaySupport::kSupportedSolid:
        ++stats_.native_replay_supported_draws;
        ++stats_.native_replay_supported_solid_draws;
        break;
      case NativeReplaySupport::kSupportedDepthOnly:
        ++stats_.native_replay_supported_draws;
        ++stats_.native_replay_supported_depth_only_draws;
        ++stats_.native_replay_depth_only_draws;
        CountDepthOnly(event);
        break;
      case NativeReplaySupport::kSupportedProjectedTransform:
        ++stats_.native_replay_supported_draws;
        ++stats_.native_replay_supported_projected_draws;
        break;
      case NativeReplaySupport::kDepthOnly:
        ++stats_.native_replay_depth_only_draws;
        CountDepthOnly(event);
        break;
      case NativeReplaySupport::kUnsupportedIndexed:
        ++stats_.native_replay_unsupported_indexed_draws;
        break;
      case NativeReplaySupport::kUnsupportedShape:
        ++stats_.native_replay_unsupported_shape_draws;
        break;
      case NativeReplaySupport::kUnsupportedLayout:
        ++stats_.native_replay_unsupported_layout_draws;
        break;
      case NativeReplaySupport::kUnsupportedTexture:
        ++stats_.native_replay_unsupported_texture_draws;
        break;
      case NativeReplaySupport::kUnsupportedTransform:
        ++stats_.native_replay_unsupported_transform_draws;
        break;
      case NativeReplaySupport::kNoOutputSkipCandidate:
        break;
    }
    if (replay_support == NativeReplaySupport::kUnsupportedTransform) {
      CountTransformGap(event);
    } else if (replay_support == NativeReplaySupport::kUnsupportedLayout) {
      CountLayoutGap(event);
    }
    stats_.vertex_fetch_count += event.vertex_fetch_summary_count;
    stats_.texture_fetch_count += event.texture_fetch_summary_count;

    const uint32_t index_bytes_needed = IndexBytesNeeded(event);
    if (index_bytes_needed != 0) {
      const uint64_t end64 = uint64_t(event.index_guest_base) + index_bytes_needed;
      const uint32_t end = end64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end64);
      ++stats_.indexed_sample_draws;
      stats_.min_index_addr = std::min(stats_.min_index_addr, event.index_guest_base);
      stats_.max_index_end = std::max(stats_.max_index_end, end);
    }

    for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
      const auto& fetch = event.vertex_fetches[i];
      const uint64_t end64 = uint64_t(fetch.address_bytes) + fetch.size_bytes;
      const uint32_t end = end64 > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end64);
      stats_.min_vertex_fetch_addr = std::min(stats_.min_vertex_fetch_addr, fetch.address_bytes);
      stats_.max_vertex_fetch_end = std::max(stats_.max_vertex_fetch_end, end);
    }

    HashGuestMemorySamples(event, context, replay_support);
    CaptureNativeGpuReplayDraw(event, context);
    CountShaderPair(event.vertex_shader_hash, event.pixel_shader_hash);
  }

  void OnNativeRenderSwap(const SwapEvent& event) override {
    ++swap_count_;
    suppress_current_backend_swap_ = false;

    const int32_t interval = REXCVAR_GET(sw2e_native_renderer_log_interval);
    if (swap_count_ <= 8 || (interval > 0 && (swap_count_ % static_cast<uint32_t>(interval)) == 0) ||
        stats_.texture_fetch_draws != 0) {
      LogFrame(event);
    }

    if (REXCVAR_GET(sw2e_native_renderer_gpu_replay_complete_on_swap)) {
      CompleteNativeGpuReplayPass("swap");
    }

    ResetFrame(stats_, event.frame_index + 1);
  }

  bool ShouldSuppressBackendSwap(const SwapEvent& event) override {
    (void)event;
    return suppress_current_backend_swap_;
  }

 private:
  void CaptureNativeGpuReplayDraw(const DrawEvent& event, const DrawEventContext& context) {
    if (!REXCVAR_GET(sw2e_native_renderer_gpu_replay) || !context.memory) {
      return;
    }

    const int32_t draw_limit = REXCVAR_GET(sw2e_native_renderer_gpu_replay_draw_limit);
    const bool transform_gaps_only =
        REXCVAR_GET(sw2e_native_renderer_gpu_replay_transform_gaps_only);
    const bool has_draw_limit = draw_limit > 0;
    if (!transform_gaps_only && has_draw_limit &&
        native_gpu_replay_draws_.size() >= static_cast<size_t>(draw_limit)) {
      return;
    }

    bool captured = false;
    if (transform_gaps_only) {
      captured = CaptureNativeGpuReplayProjectedTransformGapDraw(event, context);
    } else {
      captured = CaptureNativeGpuReplayTexturedDraw(event, context);
      if (!captured) {
        captured = CaptureNativeGpuReplayConstantScreenQuadDraw(event, context);
      }
      if (!captured) {
        captured = CaptureNativeGpuReplayDepthOnlyDraw(event, context);
      }
      if (!captured && REXCVAR_GET(sw2e_native_renderer_gpu_replay_include_solid_geometry)) {
        captured = CaptureNativeGpuReplaySolidDraw(event, context);
      }
      if (!captured && REXCVAR_GET(sw2e_native_renderer_gpu_replay_include_transform_gaps)) {
        captured = CaptureNativeGpuReplayProjectedTransformGapDraw(event, context);
      }
    }
    if (!captured) {
      return;
    }

    if (transform_gaps_only) {
      if (!has_draw_limit) {
        return;
      }
      PruneNativeGpuReplayDrawsToLimit(static_cast<size_t>(draw_limit));
    } else if (has_draw_limit &&
               native_gpu_replay_draws_.size() >= static_cast<size_t>(draw_limit)) {
      CompleteNativeGpuReplayPass("draw-limit");
    }
  }

  std::shared_ptr<const std::vector<uint8_t>> CaptureNativeGpuReplayTextureBytes(
      const TextureFetchSummary& texture_fetch, const TextureDumpPlan& texture_plan,
      const rex::memory::Memory* memory) {
    const uint64_t key = NativeGpuReplayTextureBytesKey(texture_fetch, texture_plan);
    auto existing = native_gpu_replay_texture_bytes_cache_.find(key);
    if (existing != native_gpu_replay_texture_bytes_cache_.end()) {
      return existing->second;
    }

    std::vector<uint8_t> texture_bytes;
    if (!CopyPhysicalMemorySample(memory, texture_fetch.base_address_bytes,
                                  texture_plan.expected_full_size, texture_bytes)) {
      return {};
    }

    auto texture_bytes_ref =
        std::make_shared<const std::vector<uint8_t>>(std::move(texture_bytes));
    native_gpu_replay_texture_bytes_cache_.emplace(key, texture_bytes_ref);
    return texture_bytes_ref;
  }

  bool CaptureNativeGpuReplayTexturedDraw(const DrawEvent& event,
                                          const DrawEventContext& context) {
    if (!HasNativeReplayColorOutput(event)) {
      return false;
    }
    if (!IsNativeReplayTexturedTriangleShape(event)) {
      return false;
    }

    const VertexFetchSummary* vertex_fetch = FindNativeReplayTexturedVertexFetch(event);
    if (!vertex_fetch) {
      return false;
    }

    std::optional<TextureDumpPlan> texture_plan;
    const TextureFetchSummary* texture_fetch = FindNativeReplayTextureFetch(event, texture_plan);
    if (!texture_fetch || !texture_plan) {
      return false;
    }

    const uint64_t key = (uint64_t(event.frame_index) << 32) | event.draw_index;
    if (!native_gpu_replay_draw_keys_.insert(key).second) {
      return false;
    }

    std::vector<uint32_t> indices;
    uint32_t max_index = 0;
    if (event.indexed) {
      if (!DecodeNativeReplayIndices(event, context.memory, indices, max_index)) {
        native_gpu_replay_draw_keys_.erase(key);
        return false;
      }
    } else {
      max_index = event.index_count - 1;
    }

    if (event.primitive_type == kXenosPrimitiveTriangleStrip) {
      std::vector<uint32_t> triangle_indices;
      const bool expanded = event.indexed
                                ? ExpandNativeReplayTriangleStripIndices(indices, triangle_indices,
                                                                         event.index_format)
                                : BuildNativeReplayTriangleStripIndices(event, triangle_indices);
      if (!expanded) {
        native_gpu_replay_draw_keys_.erase(key);
        return false;
      }
      indices = std::move(triangle_indices);
    }

    const uint32_t vertex_count = event.indexed ? (max_index + 1) : event.index_count;
    std::vector<uint8_t> vertex_bytes;
    const uint32_t vertex_stride_bytes = vertex_fetch->stride_words * 4;
    const uint64_t vertex_bytes_needed64 = uint64_t(vertex_count) * vertex_stride_bytes;
    if (vertex_count == 0 || vertex_bytes_needed64 > UINT32_MAX ||
        vertex_bytes_needed64 > vertex_fetch->size_bytes) {
      native_gpu_replay_draw_keys_.erase(key);
      return false;
    }
    const uint32_t vertex_bytes_needed = static_cast<uint32_t>(vertex_bytes_needed64);
    if (!CopyPhysicalMemorySample(context.memory, vertex_fetch->address_bytes, vertex_bytes_needed,
                                  vertex_bytes)) {
      native_gpu_replay_draw_keys_.erase(key);
      return false;
    }

    std::shared_ptr<const std::vector<uint8_t>> texture_bytes_ref =
        CaptureNativeGpuReplayTextureBytes(*texture_fetch, *texture_plan, context.memory);
    if (!texture_bytes_ref) {
      native_gpu_replay_draw_keys_.erase(key);
      return false;
    }

    gpu_replay::ReplayDraw replay_draw;
    replay_draw.kind = gpu_replay::ReplayDrawKind::kTexturedTriangles;
    replay_draw.frame = event.frame_index;
    replay_draw.draw = event.draw_index;
    replay_draw.vertex_shader_hash = event.vertex_shader_hash;
    replay_draw.pixel_shader_hash = event.pixel_shader_hash;
    replay_draw.vertex_stride_words = vertex_fetch->stride_words;
    replay_draw.rt0_blendcontrol = event.rt_blendcontrol[0];
    replay_draw.normalized_depthcontrol = event.normalized_depthcontrol;
    replay_draw.rt0_write_mask = static_cast<uint8_t>(event.normalized_color_mask & 0x0F);
    replay_draw.indices = std::move(indices);
    replay_draw.vertices.reserve(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
      const uint8_t* vertex = vertex_bytes.data() + size_t(i) * vertex_stride_bytes;
      gpu_replay::ReplayVertex replay_vertex;
      if (!DecodeNativeReplayTexturedVertex(vertex, vertex_stride_bytes, *vertex_fetch,
                                            replay_vertex)) {
        native_gpu_replay_draw_keys_.erase(key);
        return false;
      }
      replay_draw.vertices.push_back(replay_vertex);
    }

    replay_draw.texture.source_base_address = texture_fetch->base_address_bytes;
    replay_draw.texture.format = texture_fetch->format;
    replay_draw.texture.tiled = texture_fetch->tiled;
    replay_draw.texture.width = texture_fetch->width;
    replay_draw.texture.height = texture_fetch->height;
    replay_draw.texture.width_blocks = texture_plan->width_blocks;
    replay_draw.texture.height_blocks = texture_plan->height_blocks;
    replay_draw.texture.row_pitch_bytes = texture_plan->row_pitch_bytes;
    replay_draw.texture.endian = texture_fetch->endian;
    replay_draw.texture.bytes_ref = std::move(texture_bytes_ref);
    QueueNativeGpuReplayDraw(
        std::move(replay_draw),
        event.primitive_type == kXenosPrimitiveTriangleStrip
            ? (event.indexed ? "indexed textured strip" : "textured strip")
            : (event.indexed ? "indexed textured" : "textured"));
    return true;
  }

  bool CaptureNativeGpuReplayConstantScreenQuadDraw(const DrawEvent& event,
                                                    const DrawEventContext& context) {
    if (!HasNativeReplayColorOutput(event) || !IsNativeReplayTexturedTriangleShape(event)) {
      return false;
    }

    const VertexFetchSummary* vertex_fetch =
        FindNativeReplayConstantSelectorVertexFetch(event);
    if (!vertex_fetch) {
      return false;
    }

    std::optional<TextureDumpPlan> texture_plan;
    const TextureFetchSummary* texture_fetch = FindNativeReplayTextureFetch(event, texture_plan);
    if (!texture_fetch || !texture_plan) {
      return false;
    }

    const uint64_t key = (uint64_t(event.frame_index) << 32) | event.draw_index;
    if (!native_gpu_replay_draw_keys_.insert(key).second) {
      return false;
    }

    std::vector<uint32_t> indices;
    if (event.primitive_type == kXenosPrimitiveTriangleStrip) {
      if (!BuildNativeReplayTriangleStripIndices(event, indices)) {
        native_gpu_replay_draw_keys_.erase(key);
        return false;
      }
    } else if (event.primitive_type == kXenosPrimitiveTriangleList) {
      indices.reserve(event.index_count);
      for (uint32_t i = 0; i < event.index_count; ++i) {
        indices.push_back(i);
      }
    } else {
      native_gpu_replay_draw_keys_.erase(key);
      return false;
    }

    std::vector<uint8_t> vertex_bytes;
    const uint32_t vertex_bytes_needed = event.index_count * sizeof(float);
    if (!CopyPhysicalMemorySample(context.memory, vertex_fetch->address_bytes,
                                  vertex_bytes_needed, vertex_bytes)) {
      native_gpu_replay_draw_keys_.erase(key);
      return false;
    }

    std::shared_ptr<const std::vector<uint8_t>> texture_bytes_ref =
        CaptureNativeGpuReplayTextureBytes(*texture_fetch, *texture_plan, context.memory);
    if (!texture_bytes_ref) {
      native_gpu_replay_draw_keys_.erase(key);
      return false;
    }

    gpu_replay::ReplayDraw replay_draw;
    replay_draw.kind = gpu_replay::ReplayDrawKind::kTexturedTriangles;
    replay_draw.frame = event.frame_index;
    replay_draw.draw = event.draw_index;
    replay_draw.vertex_shader_hash = event.vertex_shader_hash;
    replay_draw.pixel_shader_hash = event.pixel_shader_hash;
    replay_draw.vertex_stride_words = vertex_fetch->stride_words;
    replay_draw.rt0_blendcontrol = event.rt_blendcontrol[0];
    replay_draw.normalized_depthcontrol = event.normalized_depthcontrol;
    replay_draw.rt0_write_mask = static_cast<uint8_t>(event.normalized_color_mask & 0x0F);
    replay_draw.pixel_mode = gpu_replay::ReplayPixelMode::kTextureColorLerpConstant;
    if (const auto* pixel_constant0 = FindNativeReplayPixelFloatConstant(event, 0)) {
      replay_draw.pixel_constant0 = {pixel_constant0->values[0], pixel_constant0->values[1],
                                     pixel_constant0->values[2], pixel_constant0->values[3]};
    }
    replay_draw.indices = std::move(indices);
    replay_draw.vertices.reserve(event.index_count);

    for (uint32_t i = 0; i < event.index_count; ++i) {
      const float selector_value =
          ReadF32(vertex_bytes.data() + size_t(i) * sizeof(float), vertex_fetch->endian);
      if (!std::isfinite(selector_value) || selector_value < 0.0f || selector_value >= 4.0f) {
        native_gpu_replay_draw_keys_.erase(key);
        return false;
      }
      const uint32_t selector = static_cast<uint32_t>(std::floor(selector_value));
      const auto* position_constant = FindNativeReplayFloatConstant(event, 7 + selector);
      const auto* color_constant = FindNativeReplayFloatConstant(event, 11 + selector);
      const auto* texcoord_constant = FindNativeReplayFloatConstant(event, 15 + selector);
      const auto* alpha_constant = FindNativeReplayFloatConstant(event, 19);
      if (!position_constant || !color_constant || !texcoord_constant) {
        native_gpu_replay_draw_keys_.erase(key);
        return false;
      }

      gpu_replay::ReplayVertex replay_vertex;
      replay_vertex.x = position_constant->values[0];
      replay_vertex.y = position_constant->values[1];
      replay_vertex.z = 0.0f;
      replay_vertex.w = 1.0f;
      replay_vertex.u = texcoord_constant->values[0];
      replay_vertex.v = texcoord_constant->values[1];
      replay_vertex.r = color_constant->values[0];
      replay_vertex.g = color_constant->values[1];
      replay_vertex.b = color_constant->values[2];
      replay_vertex.a = alpha_constant ? alpha_constant->values[0] : color_constant->values[3];
      replay_vertex.texture_lerp_factor = 1.0f;
      replay_draw.vertices.push_back(replay_vertex);
    }

    replay_draw.texture.source_base_address = texture_fetch->base_address_bytes;
    replay_draw.texture.format = texture_fetch->format;
    replay_draw.texture.tiled = texture_fetch->tiled;
    replay_draw.texture.width = texture_fetch->width;
    replay_draw.texture.height = texture_fetch->height;
    replay_draw.texture.width_blocks = texture_plan->width_blocks;
    replay_draw.texture.height_blocks = texture_plan->height_blocks;
    replay_draw.texture.row_pitch_bytes = texture_plan->row_pitch_bytes;
    replay_draw.texture.endian = texture_fetch->endian;
    replay_draw.texture.bytes_ref = std::move(texture_bytes_ref);
    QueueNativeGpuReplayDraw(std::move(replay_draw), "constant screen quad");
    return true;
  }

  bool CaptureNativeGpuReplayProjectedTransformGapDraw(const DrawEvent& event,
                                                       const DrawEventContext& context) {
    if (!REXCVAR_GET(sw2e_native_renderer_gpu_replay_include_transform_gaps)) {
      return false;
    }
    if (!NativeGpuReplayHashFilterMatches(
            std::string_view(
                REXCVAR_GET(sw2e_native_renderer_gpu_replay_projected_vertex_shader_filter)),
            event.vertex_shader_hash) ||
        !NativeGpuReplayHashFilterMatches(
            std::string_view(
                REXCVAR_GET(sw2e_native_renderer_gpu_replay_projected_pixel_shader_filter)),
            event.pixel_shader_hash)) {
      return false;
    }
    const NativeReplaySupport replay_support = ClassifyNativeReplaySupport(event);
    if (!HasNativeReplayColorOutput(event) ||
        (replay_support != NativeReplaySupport::kUnsupportedTransform &&
         replay_support != NativeReplaySupport::kSupportedProjectedTransform)) {
      return false;
    }
    if (!IsNativeReplayTexturedTriangleShape(event)) {
      return RejectNativeGpuReplayProjectedTransformGap(event, "shape", replay_support);
    }

    const VertexFetchSummary* vertex_fetch = FindDecodableNativeReplayTexturedVertexFetch(event);
    if (!vertex_fetch) {
      return RejectNativeGpuReplayProjectedTransformGap(event, "vertex_fetch", replay_support);
    }
    if (IsNativeReplayScreenSpaceTexturedVertexFetch(*vertex_fetch)) {
      return RejectNativeGpuReplayProjectedTransformGap(event, "screen_space", replay_support);
    }

    std::optional<TextureDumpPlan> texture_plan;
    const TextureFetchSummary* texture_fetch = FindNativeReplayTextureFetch(event, texture_plan);
    if (!texture_fetch || !texture_plan) {
      return RejectNativeGpuReplayProjectedTransformGap(event, "texture_fetch", replay_support);
    }

    const uint64_t key = (uint64_t(event.frame_index) << 32) | event.draw_index;
    if (!native_gpu_replay_draw_keys_.insert(key).second) {
      return false;
    }

    std::vector<uint32_t> indices;
    uint32_t max_index = 0;
    if (event.indexed) {
      if (!DecodeNativeReplayIndices(event, context.memory, indices, max_index)) {
        native_gpu_replay_draw_keys_.erase(key);
        return RejectNativeGpuReplayProjectedTransformGap(event, "index_decode", replay_support);
      }
    } else {
      max_index = event.index_count - 1;
    }

    if (event.primitive_type == kXenosPrimitiveTriangleStrip) {
      std::vector<uint32_t> triangle_indices;
      const bool expanded = event.indexed
                                ? ExpandNativeReplayTriangleStripIndices(indices, triangle_indices,
                                                                         event.index_format)
                                : BuildNativeReplayTriangleStripIndices(event, triangle_indices);
      if (!expanded) {
        native_gpu_replay_draw_keys_.erase(key);
        return RejectNativeGpuReplayProjectedTransformGap(event, "strip_expand", replay_support);
      }
      indices = std::move(triangle_indices);
    }

    const uint32_t replay_index_count =
        indices.empty() ? event.index_count : static_cast<uint32_t>(indices.size());
    const int32_t min_projected_indices =
        REXCVAR_GET(sw2e_native_renderer_gpu_replay_projected_min_indices);
    if (min_projected_indices > 0 &&
        replay_index_count < static_cast<uint32_t>(min_projected_indices)) {
      native_gpu_replay_draw_keys_.erase(key);
      return RejectNativeGpuReplayProjectedTransformGap(event, "min_indices", replay_support);
    }

    const uint32_t vertex_count = event.indexed ? (max_index + 1) : event.index_count;
    const int32_t min_projected_vertices =
        REXCVAR_GET(sw2e_native_renderer_gpu_replay_transform_gap_min_vertices);
    const uint32_t required_vertex_count =
        replay_support == NativeReplaySupport::kSupportedProjectedTransform
            ? 3u
            : static_cast<uint32_t>(std::max(min_projected_vertices, 3));
    if (vertex_count < required_vertex_count) {
      native_gpu_replay_draw_keys_.erase(key);
      return RejectNativeGpuReplayProjectedTransformGap(event, "min_vertices", replay_support);
    }

    std::vector<uint8_t> vertex_bytes;
    const uint32_t vertex_stride_bytes = vertex_fetch->stride_words * 4;
    const uint64_t vertex_bytes_needed64 = uint64_t(vertex_count) * vertex_stride_bytes;
    if (vertex_count == 0 || vertex_bytes_needed64 > UINT32_MAX ||
        vertex_bytes_needed64 > vertex_fetch->size_bytes) {
      native_gpu_replay_draw_keys_.erase(key);
      return RejectNativeGpuReplayProjectedTransformGap(event, "vertex_size", replay_support);
    }
    const uint32_t vertex_bytes_needed = static_cast<uint32_t>(vertex_bytes_needed64);
    if (!CopyPhysicalMemorySample(context.memory, vertex_fetch->address_bytes, vertex_bytes_needed,
                                  vertex_bytes)) {
      native_gpu_replay_draw_keys_.erase(key);
      return RejectNativeGpuReplayProjectedTransformGap(event, "vertex_copy", replay_support);
    }

    gpu_replay::ReplayDraw replay_draw;
    replay_draw.kind = gpu_replay::ReplayDrawKind::kProjectedTexturedTriangles;
    replay_draw.frame = event.frame_index;
    replay_draw.draw = event.draw_index;
    replay_draw.vertex_shader_hash = event.vertex_shader_hash;
    replay_draw.pixel_shader_hash = event.pixel_shader_hash;
    replay_draw.vertex_stride_words = vertex_fetch->stride_words;
    replay_draw.rt0_blendcontrol = 0x00010001;
    replay_draw.normalized_depthcontrol = event.normalized_depthcontrol;
    replay_draw.rt0_write_mask = 0x0F;
    replay_draw.indices = std::move(indices);
    replay_draw.vertices.reserve(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
      const uint8_t* vertex = vertex_bytes.data() + size_t(i) * vertex_stride_bytes;
      gpu_replay::ReplayVertex replay_vertex;
      if (!DecodeNativeReplayTexturedVertex(vertex, vertex_stride_bytes, *vertex_fetch,
                                            replay_vertex)) {
        native_gpu_replay_draw_keys_.erase(key);
        return RejectNativeGpuReplayProjectedTransformGap(event, "vertex_decode", replay_support);
      }
      DecodeNativeReplaySharedShaderSkinInputs(vertex, vertex_stride_bytes, vertex_fetch->endian,
                                               event.vertex_shader_hash, replay_vertex);
      replay_draw.vertices.push_back(replay_vertex);
    }

    const ProjectionCandidate projection =
        FindNativeReplayProjectionCandidate(event, replay_draw.vertices);
    if (!projection.valid) {
      native_gpu_replay_draw_keys_.erase(key);
      return RejectNativeGpuReplayProjectedTransformGap(event, "projection", replay_support);
    }
    if (REXCVAR_GET(sw2e_native_renderer_gpu_replay_debug_fit_projected_gaps)) {
      if (!FitReplayVerticesForDebug(replay_draw.vertices)) {
        native_gpu_replay_draw_keys_.erase(key);
        return RejectNativeGpuReplayProjectedTransformGap(event, "debug_fit", replay_support);
      }
    } else {
      for (auto& vertex : replay_draw.vertices) {
        if (!ProjectNativeReplayVertex(vertex, projection)) {
          native_gpu_replay_draw_keys_.erase(key);
          return RejectNativeGpuReplayProjectedTransformGap(event, "project_vertex", replay_support);
        }
      }
      if (REXCVAR_GET(sw2e_native_renderer_gpu_replay_normalize_projected_gaps) &&
          !FitReplayProjectedVerticesXYForDebug(replay_draw.vertices)) {
        native_gpu_replay_draw_keys_.erase(key);
        return RejectNativeGpuReplayProjectedTransformGap(event, "projected_fit", replay_support);
      }
    }

    std::shared_ptr<const std::vector<uint8_t>> texture_bytes_ref =
        CaptureNativeGpuReplayTextureBytes(*texture_fetch, *texture_plan, context.memory);
    if (!texture_bytes_ref) {
      native_gpu_replay_draw_keys_.erase(key);
      return RejectNativeGpuReplayProjectedTransformGap(event, "texture_copy", replay_support);
    }

    replay_draw.texture.source_base_address = texture_fetch->base_address_bytes;
    replay_draw.texture.format = texture_fetch->format;
    replay_draw.texture.tiled = texture_fetch->tiled;
    replay_draw.texture.width = texture_fetch->width;
    replay_draw.texture.height = texture_fetch->height;
    replay_draw.texture.width_blocks = texture_plan->width_blocks;
    replay_draw.texture.height_blocks = texture_plan->height_blocks;
    replay_draw.texture.row_pitch_bytes = texture_plan->row_pitch_bytes;
    replay_draw.texture.endian = texture_fetch->endian;
    replay_draw.texture.bytes_ref = std::move(texture_bytes_ref);
    QueueNativeGpuReplayDraw(std::move(replay_draw), "projected transform gap");
    if (native_gpu_replay_captured_draw_log_count_ <= kNativeGpuReplayDrawLogLimit) {
      char upstream_constants[64] = "none";
      if (projection.use_shared_shader_skin) {
        std::snprintf(upstream_constants, sizeof(upstream_constants), "skinned:c4+a0..c6+a0");
      } else if (projection.has_upstream_transform) {
        std::snprintf(upstream_constants, sizeof(upstream_constants), "c%u,c%u,c%u",
                      projection.upstream_constant_indices[0],
                      projection.upstream_constant_indices[1],
                      projection.upstream_constant_indices[2]);
      }
      REXLOG_INFO(
          "SW2E projected transform gap candidate frame {} draw {} constants=c{},c{},c{},c{} "
          "upstream={} source={} orientation={} finite={:.3f} inside={:.3f} "
          "ndc=({:.4f},{:.4f},{:.4f})..({:.4f},{:.4f},{:.4f}) score={:.3f}",
          event.frame_index, event.draw_index, projection.constant_indices[0],
          projection.constant_indices[1], projection.constant_indices[2],
          projection.constant_indices[3], upstream_constants, projection.source,
          projection.column_major ? "column" : "row", projection.finite_ratio,
          projection.inside_ratio, projection.ndc_mins[0], projection.ndc_mins[1],
          projection.ndc_mins[2], projection.ndc_maxs[0], projection.ndc_maxs[1],
          projection.ndc_maxs[2], projection.score);
    }
    return true;
  }

  bool RejectNativeGpuReplayProjectedTransformGap(const DrawEvent& event, const char* reason,
                                                  NativeReplaySupport replay_support) {
    if (native_gpu_replay_projected_reject_log_count_ <
        kNativeGpuReplayProjectedRejectLogLimit) {
      char texture_summary[256] = "none";
      if (event.texture_fetch_summary_count != 0) {
        const auto& tex0 = event.texture_fetches[0];
        std::snprintf(texture_summary, sizeof(texture_summary),
                      "t0=c%u fmt=%u dim=%u tiled=%u %ux%ux%u pitch=%u addr=0x%08x",
                      tex0.fetch_constant, tex0.format, tex0.dimension, tex0.tiled, tex0.width,
                      tex0.height, tex0.depth, tex0.pitch, tex0.base_address_bytes);
        if (event.texture_fetch_summary_count > 1) {
          const auto& tex1 = event.texture_fetches[1];
          const size_t used = std::strlen(texture_summary);
          std::snprintf(texture_summary + used, sizeof(texture_summary) - used,
                        " t1=c%u fmt=%u dim=%u tiled=%u %ux%ux%u pitch=%u addr=0x%08x",
                        tex1.fetch_constant, tex1.format, tex1.dimension, tex1.tiled,
                        tex1.width, tex1.height, tex1.depth, tex1.pitch,
                        tex1.base_address_bytes);
        }
      }
      REXLOG_INFO(
          "SW2E projected transform gap rejected frame {} draw {}: {} support={} prim={} "
          "indexed={} index_count={} vfetches={} tfetches={} textures={}",
          event.frame_index, event.draw_index, reason, NativeReplaySupportName(replay_support),
          event.primitive_type, event.indexed, event.index_count, event.vertex_fetch_summary_count,
          event.texture_fetch_summary_count, texture_summary);
    } else if (native_gpu_replay_projected_reject_log_count_ ==
               kNativeGpuReplayProjectedRejectLogLimit) {
      REXLOG_INFO("SW2E projected transform gap suppressing further rejection logs");
    }
    ++native_gpu_replay_projected_reject_log_count_;
    return false;
  }

  bool CaptureNativeGpuReplayDepthOnlyDraw(const DrawEvent& event,
                                           const DrawEventContext& context) {
    return CaptureNativeGpuReplaySolidLikeDraw(event, context, true);
  }

  bool CaptureNativeGpuReplaySolidDraw(const DrawEvent& event, const DrawEventContext& context) {
    return CaptureNativeGpuReplaySolidLikeDraw(event, context, false);
  }

  bool CaptureNativeGpuReplaySolidLikeDraw(const DrawEvent& event,
                                           const DrawEventContext& context, bool depth_only) {
    if (depth_only) {
      if (!CanReplayNativeDepthOnlyDraw(event)) {
        return false;
      }
    } else if (!HasNativeReplayColorOutput(event)) {
      return false;
    }
    if (event.indexed || event.vertex_fetch_summary_count == 0 ||
        event.texture_fetch_summary_count != 0) {
      return false;
    }
    if (!IsNativeReplaySolidShape(event)) {
      return false;
    }

    const VertexFetchSummary* vertex_fetch = FindNativeReplaySolidVertexFetch(event);
    if (!vertex_fetch) {
      return false;
    }

    const uint64_t key = (uint64_t(event.frame_index) << 32) | event.draw_index;
    if (!native_gpu_replay_draw_keys_.insert(key).second) {
      return false;
    }

    std::vector<uint8_t> vertex_bytes;
    const uint32_t vertex_stride_bytes = vertex_fetch->stride_words * 4;
    const uint32_t vertex_bytes_needed = event.index_count * vertex_stride_bytes;
    if (!CopyPhysicalMemorySample(context.memory, vertex_fetch->address_bytes, vertex_bytes_needed,
                                  vertex_bytes)) {
      native_gpu_replay_draw_keys_.erase(key);
      return false;
    }

    const std::array<float, 4> solid_color =
        depth_only ? std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}
                   : NativeGpuReplaySolidColor(event);
    auto read_vertex = [&](uint32_t index, bool clip_space) {
      const uint8_t* vertex = vertex_bytes.data() + size_t(index) * vertex_stride_bytes;
      gpu_replay::ReplayVertex replay_vertex;
      const float x = ReadF32(vertex + 0, vertex_fetch->endian);
      const float y = ReadF32(vertex + 4, vertex_fetch->endian);
      replay_vertex.x = clip_space ? ((x * 0.5f + 0.5f) * kNativeGpuReplayWidth) : x;
      replay_vertex.y = clip_space ? ((1.0f - (y * 0.5f + 0.5f)) * kNativeGpuReplayHeight) : y;
      replay_vertex.z = 0.0f;
      replay_vertex.w = 1.0f;
      if (vertex_fetch->stride_words >= 7) {
        replay_vertex.z = ReadF32(vertex + 8, vertex_fetch->endian);
        replay_vertex.r = ReadF32(vertex + 12, vertex_fetch->endian);
        replay_vertex.g = ReadF32(vertex + 16, vertex_fetch->endian);
        replay_vertex.b = ReadF32(vertex + 20, vertex_fetch->endian);
        replay_vertex.a = ReadF32(vertex + 24, vertex_fetch->endian);
      } else {
        replay_vertex.r = solid_color[0];
        replay_vertex.g = solid_color[1];
        replay_vertex.b = solid_color[2];
        replay_vertex.a = solid_color[3];
      }
      return replay_vertex;
    };

    gpu_replay::ReplayDraw replay_draw;
    replay_draw.kind = depth_only ? gpu_replay::ReplayDrawKind::kDepthOnlyTriangles
                                  : gpu_replay::ReplayDrawKind::kSolidTriangles;
    replay_draw.frame = event.frame_index;
    replay_draw.draw = event.draw_index;
    replay_draw.vertex_shader_hash = event.vertex_shader_hash;
    replay_draw.pixel_shader_hash = event.pixel_shader_hash;
    replay_draw.vertex_stride_words = vertex_fetch->stride_words;
    replay_draw.rt0_blendcontrol = event.rt_blendcontrol[0];
    replay_draw.normalized_depthcontrol = event.normalized_depthcontrol;
    replay_draw.rt0_write_mask =
        depth_only ? 0 : static_cast<uint8_t>(event.normalized_color_mask & 0x0F);
    if (event.primitive_type == 8) {
      gpu_replay::ReplayVertex v0 = read_vertex(0, false);
      gpu_replay::ReplayVertex v1 = read_vertex(1, false);
      gpu_replay::ReplayVertex v2 = read_vertex(2, false);
      gpu_replay::ReplayVertex v3 = v0;
      v3.y = v2.y;
      replay_draw.vertices = {v0, v1, v2, v0, v2, v3};
    } else {
      gpu_replay::ReplayVertex v0 = read_vertex(0, true);
      gpu_replay::ReplayVertex v1 = read_vertex(1, true);
      gpu_replay::ReplayVertex v2 = read_vertex(2, true);
      gpu_replay::ReplayVertex v3 = read_vertex(3, true);
      replay_draw.vertices = {v0, v1, v2, v2, v1, v3};
    }

    QueueNativeGpuReplayDraw(
        std::move(replay_draw),
        depth_only ? (event.primitive_type == 8 ? "depth rectangle" : "depth strip")
                   : (event.primitive_type == 8 ? "rectangle" : "strip"));
    return true;
  }

  void QueueNativeGpuReplayDraw(gpu_replay::ReplayDraw&& replay_draw, const char* kind) {
    const uint32_t vertex_count = static_cast<uint32_t>(replay_draw.vertices.size());
    const uint32_t index_count = static_cast<uint32_t>(replay_draw.indices.size());
    const uint32_t frame_index = replay_draw.frame;
    const uint32_t draw_index = replay_draw.draw;
    native_gpu_replay_draws_.push_back(std::move(replay_draw));

    if (native_gpu_replay_captured_draw_log_count_ < kNativeGpuReplayDrawLogLimit) {
      REXLOG_INFO("SW2E native GPU replay captured {} frame {} draw {} ({} vertices, {} indices)",
                  kind, frame_index, draw_index, vertex_count, index_count);
    } else if (native_gpu_replay_captured_draw_log_count_ == kNativeGpuReplayDrawLogLimit) {
      REXLOG_INFO("SW2E native GPU replay suppressing further per-draw capture logs");
    }
    ++native_gpu_replay_captured_draw_log_count_;
  }

  void LogNativeGpuReplayRetainedDraws(const std::vector<gpu_replay::ReplayDraw>& draws) const {
    const size_t log_count = std::min<size_t>(draws.size(), 12);
    for (size_t i = 0; i < log_count; ++i) {
      const gpu_replay::ReplayDraw& draw = draws[i];
      REXLOG_INFO(
          "SW2E native GPU replay retained [{}] kind={} frame={} draw={} VS={:#018x} "
          "PS={:#018x} stride_words={} vertices={} indices={} texture_fmt={} tiled={} "
          "texture={}x{} depth=0x{:08x} score={}",
          i, NativeGpuReplayDrawKindName(draw.kind), draw.frame, draw.draw,
          draw.vertex_shader_hash, draw.pixel_shader_hash, draw.vertex_stride_words,
          draw.vertices.size(), draw.indices.size(), draw.texture.format, draw.texture.tiled,
          draw.texture.width, draw.texture.height, draw.normalized_depthcontrol,
          NativeGpuReplayDrawScore(draw));
    }
    if (draws.size() > log_count) {
      REXLOG_INFO("SW2E native GPU replay retained {} additional draws", draws.size() - log_count);
    }
  }

  void PruneNativeGpuReplayDrawsToLimit(size_t draw_limit) {
    while (native_gpu_replay_draws_.size() > draw_limit) {
      auto weakest_it = std::min_element(
          native_gpu_replay_draws_.begin(), native_gpu_replay_draws_.end(),
          [](const gpu_replay::ReplayDraw& a, const gpu_replay::ReplayDraw& b) {
            return NativeGpuReplayDrawScore(a) < NativeGpuReplayDrawScore(b);
          });
      if (weakest_it == native_gpu_replay_draws_.end()) {
        return;
      }
      native_gpu_replay_draws_.erase(weakest_it);
    }
  }

  void CompleteNativeGpuReplayPass(const char* reason) {
    if (native_gpu_replay_draws_.empty()) {
      return;
    }

    const size_t completed_draw_count = native_gpu_replay_draws_.size();
    const gpu_replay::ReplayDraw& first_draw = native_gpu_replay_draws_.front();
    const gpu_replay::ReplayDraw& last_draw = native_gpu_replay_draws_.back();
    const bool has_color_output = NativeGpuReplayPassHasColorOutput(native_gpu_replay_draws_);
    const bool owns_current_frame = NativeGpuReplayOwnsCurrentFrame(completed_draw_count);
    bool presented = false;
    if (has_color_output && REXCVAR_GET(sw2e_native_renderer_gpu_replay_live_present)) {
      const int32_t present_limit = REXCVAR_GET(sw2e_native_renderer_gpu_replay_live_present_limit);
      if (present_limit <= 0 ||
          native_gpu_replay_live_present_count_ < static_cast<uint32_t>(present_limit)) {
        if (gpu_replay::PresentMenuReplayD3D11Child(native_window_handle_, native_gpu_replay_draws_,
                                                    1280, 720)) {
          ++native_gpu_replay_live_present_count_;
          presented = true;
        }
      }
    }
    if (presented && owns_current_frame &&
        REXCVAR_GET(sw2e_native_renderer_gpu_replay_suppress_backend_swap)) {
      suppress_current_backend_swap_ = true;
    }

    ++native_gpu_replay_completed_pass_count_;
    const uint32_t pass_score = NativeGpuReplayPassScore(native_gpu_replay_draws_);
    if (pass_score > native_gpu_replay_best_completed_score_) {
      native_gpu_replay_best_completed_score_ = pass_score;
      native_gpu_replay_best_completed_draws_ = native_gpu_replay_draws_;
    }

    if (native_gpu_replay_completed_pass_count_ <= kNativeGpuReplayPassLogInitial ||
        (native_gpu_replay_completed_pass_count_ % kNativeGpuReplayPassLogInterval) == 0) {
      REXLOG_INFO(
          "SW2E native GPU replay completed pass {} by {} (draws={}, frame_range={}-{}, "
          "draw_range={}-{}, live_presents={}, owns_frame={}, suppress_backend={}, "
          "best_score={})",
          native_gpu_replay_completed_pass_count_, reason, completed_draw_count, first_draw.frame,
          last_draw.frame, first_draw.draw, last_draw.draw, native_gpu_replay_live_present_count_,
          owns_current_frame, suppress_current_backend_swap_, native_gpu_replay_best_completed_score_);
      LogNativeGpuReplayRetainedDraws(native_gpu_replay_draws_);
    }

    native_gpu_replay_last_completed_draws_ = std::move(native_gpu_replay_draws_);
    native_gpu_replay_draws_.clear();
    native_gpu_replay_draw_keys_.clear();
    native_gpu_replay_texture_bytes_cache_.clear();
  }

  bool NativeGpuReplayOwnsCurrentFrame(size_t completed_draw_count) const {
    return completed_draw_count == stats_.native_replay_supported_draws &&
           stats_.native_replay_supported_draws != 0 &&
           stats_.native_replay_supported_draws >
               stats_.native_replay_supported_depth_only_draws &&
           stats_.native_replay_unsupported_indexed_draws == 0 &&
           stats_.native_replay_unsupported_shape_draws == 0 &&
           stats_.native_replay_unsupported_layout_draws == 0 &&
           stats_.native_replay_unsupported_texture_draws == 0 &&
           stats_.native_replay_unsupported_transform_draws == 0;
  }

  void CountShaderPair(uint64_t vertex_hash, uint64_t pixel_hash) {
    ShaderPairCounter* empty_slot = nullptr;
    for (auto& pair : stats_.shader_pairs) {
      if (pair.draws != 0 && pair.vertex_hash == vertex_hash && pair.pixel_hash == pixel_hash) {
        ++pair.draws;
        return;
      }
      if (pair.draws == 0 && !empty_slot) {
        empty_slot = &pair;
      }
    }

    if (empty_slot) {
      empty_slot->vertex_hash = vertex_hash;
      empty_slot->pixel_hash = pixel_hash;
      empty_slot->draws = 1;
      return;
    }

    ++stats_.other_shader_pair_draws;
  }

  void CountDepthOnly(const DrawEvent& event) {
    DepthOnlyCounter* empty_slot = nullptr;
    for (auto& depth_only : stats_.depth_only) {
      if (depth_only.draws != 0 && depth_only.vertex_hash == event.vertex_shader_hash &&
          depth_only.pixel_hash == event.pixel_shader_hash &&
          depth_only.primitive_type == event.primitive_type &&
          depth_only.index_count == event.index_count && depth_only.indexed == event.indexed &&
          depth_only.normalized_depthcontrol == event.normalized_depthcontrol &&
          depth_only.rb_depth_info == event.rb_depth_info &&
          depth_only.normalized_color_mask == event.normalized_color_mask &&
          depth_only.pixel_color_target_mask == event.pixel_color_target_mask &&
          depth_only.vertex_fetch_count == event.vertex_fetch_summary_count &&
          depth_only.texture_fetch_count == event.texture_fetch_summary_count &&
          depth_only.pixel_shader_needed == event.pixel_shader_needed_with_rasterization &&
          depth_only.rasterization_potentially_done == event.rasterization_potentially_done) {
        ++depth_only.draws;
        return;
      }
      if (depth_only.draws == 0 && !empty_slot) {
        empty_slot = &depth_only;
      }
    }

    if (empty_slot) {
      empty_slot->vertex_hash = event.vertex_shader_hash;
      empty_slot->pixel_hash = event.pixel_shader_hash;
      empty_slot->primitive_type = event.primitive_type;
      empty_slot->index_count = event.index_count;
      empty_slot->indexed = event.indexed;
      empty_slot->normalized_depthcontrol = event.normalized_depthcontrol;
      empty_slot->rb_depth_info = event.rb_depth_info;
      empty_slot->normalized_color_mask = event.normalized_color_mask;
      empty_slot->pixel_color_target_mask = event.pixel_color_target_mask;
      empty_slot->vertex_fetch_count = event.vertex_fetch_summary_count;
      empty_slot->texture_fetch_count = event.texture_fetch_summary_count;
      empty_slot->pixel_shader_needed = event.pixel_shader_needed_with_rasterization;
      empty_slot->rasterization_potentially_done = event.rasterization_potentially_done;
      empty_slot->draws = 1;
      return;
    }

    ++stats_.other_depth_only_draws;
  }

  void CountTransformGap(const DrawEvent& event) {
    const VertexFetchSummary* fetch = FindDecodableNativeReplayTexturedVertexFetch(event);
    if (!fetch) {
      return;
    }

    const TextureFetchSummary* texture =
        event.texture_fetch_summary_count != 0 ? &event.texture_fetches[0] : nullptr;
    const uint64_t attribute_signature_hash = TransformGapAttributeSignature(*fetch);
    TransformGapCounter* empty_slot = nullptr;
    for (auto& gap : stats_.transform_gaps) {
      if (gap.draws != 0 && gap.vertex_hash == event.vertex_shader_hash &&
          gap.pixel_hash == event.pixel_shader_hash && gap.primitive_type == event.primitive_type &&
          gap.indexed == event.indexed && gap.fetch_constant == fetch->fetch_constant &&
          gap.stride_words == fetch->stride_words && gap.attribute_count == fetch->attribute_count &&
          gap.attribute_signature_hash == attribute_signature_hash &&
          gap.texture_fetch_count == event.texture_fetch_summary_count &&
          gap.first_texture_format == (texture ? texture->format : 0) &&
          gap.first_texture_dimension == (texture ? texture->dimension : 0) &&
          gap.first_texture_tiled == (texture ? texture->tiled : 0)) {
        ++gap.draws;
        return;
      }
      if (gap.draws == 0 && !empty_slot) {
        empty_slot = &gap;
      }
    }

    if (empty_slot) {
      empty_slot->vertex_hash = event.vertex_shader_hash;
      empty_slot->pixel_hash = event.pixel_shader_hash;
      empty_slot->attribute_signature_hash = attribute_signature_hash;
      empty_slot->primitive_type = event.primitive_type;
      empty_slot->indexed = event.indexed;
      empty_slot->fetch_constant = fetch->fetch_constant;
      empty_slot->stride_words = fetch->stride_words;
      empty_slot->attribute_count = fetch->attribute_count;
      empty_slot->recorded_attribute_count =
          std::min<uint32_t>(fetch->attribute_summary_count, empty_slot->attributes.size());
      for (uint32_t i = 0; i < empty_slot->recorded_attribute_count; ++i) {
        const VertexAttributeSummary& source = fetch->attributes[i];
        TransformGapAttribute& destination = empty_slot->attributes[i];
        destination.data_format = source.data_format;
        destination.offset_words = source.offset_words;
        destination.exp_adjust = source.exp_adjust;
        destination.result_storage_target = source.result_storage_target;
        destination.result_storage_index = source.result_storage_index;
        destination.result_write_mask = source.result_write_mask;
        destination.result_used_components = source.result_used_components;
        destination.result_swizzle = source.result_swizzle;
        destination.is_signed = source.is_signed;
        destination.is_integer = source.is_integer;
      }
      empty_slot->texture_fetch_count = event.texture_fetch_summary_count;
      empty_slot->first_texture_format = texture ? texture->format : 0;
      empty_slot->first_texture_dimension = texture ? texture->dimension : 0;
      empty_slot->first_texture_tiled = texture ? texture->tiled : 0;
      empty_slot->draws = 1;
      return;
    }

    ++stats_.other_transform_gap_draws;
  }

  void CountLayoutGap(const DrawEvent& event) {
    const VertexFetchSummary* fetch = FirstUsefulVertexFetch(event);
    const TextureFetchSummary* texture =
        event.texture_fetch_summary_count != 0 ? &event.texture_fetches[0] : nullptr;
    const uint64_t attribute_signature_hash = fetch ? TransformGapAttributeSignature(*fetch) : 0;
    const uint32_t fetch_constant = fetch ? fetch->fetch_constant : UINT32_MAX;
    const uint32_t stride_words = fetch ? fetch->stride_words : 0;
    const uint32_t attribute_count = fetch ? fetch->attribute_count : 0;

    TransformGapCounter* empty_slot = nullptr;
    for (auto& gap : stats_.layout_gaps) {
      if (gap.draws != 0 && gap.vertex_hash == event.vertex_shader_hash &&
          gap.pixel_hash == event.pixel_shader_hash && gap.primitive_type == event.primitive_type &&
          gap.indexed == event.indexed && gap.fetch_constant == fetch_constant &&
          gap.stride_words == stride_words && gap.attribute_count == attribute_count &&
          gap.attribute_signature_hash == attribute_signature_hash &&
          gap.texture_fetch_count == event.texture_fetch_summary_count &&
          gap.first_texture_format == (texture ? texture->format : 0) &&
          gap.first_texture_dimension == (texture ? texture->dimension : 0) &&
          gap.first_texture_tiled == (texture ? texture->tiled : 0)) {
        ++gap.draws;
        return;
      }
      if (gap.draws == 0 && !empty_slot) {
        empty_slot = &gap;
      }
    }

    if (empty_slot) {
      empty_slot->vertex_hash = event.vertex_shader_hash;
      empty_slot->pixel_hash = event.pixel_shader_hash;
      empty_slot->attribute_signature_hash = attribute_signature_hash;
      empty_slot->primitive_type = event.primitive_type;
      empty_slot->indexed = event.indexed;
      empty_slot->fetch_constant = fetch_constant;
      empty_slot->stride_words = stride_words;
      empty_slot->attribute_count = attribute_count;
      if (fetch) {
        empty_slot->recorded_attribute_count =
            std::min<uint32_t>(fetch->attribute_summary_count, empty_slot->attributes.size());
        for (uint32_t i = 0; i < empty_slot->recorded_attribute_count; ++i) {
          const VertexAttributeSummary& source = fetch->attributes[i];
          TransformGapAttribute& destination = empty_slot->attributes[i];
          destination.data_format = source.data_format;
          destination.offset_words = source.offset_words;
          destination.exp_adjust = source.exp_adjust;
          destination.result_storage_target = source.result_storage_target;
          destination.result_storage_index = source.result_storage_index;
          destination.result_write_mask = source.result_write_mask;
          destination.result_used_components = source.result_used_components;
          destination.result_swizzle = source.result_swizzle;
          destination.is_signed = source.is_signed;
          destination.is_integer = source.is_integer;
        }
      }
      empty_slot->texture_fetch_count = event.texture_fetch_summary_count;
      empty_slot->first_texture_format = texture ? texture->format : 0;
      empty_slot->first_texture_dimension = texture ? texture->dimension : 0;
      empty_slot->first_texture_tiled = texture ? texture->tiled : 0;
      empty_slot->draws = 1;
      return;
    }

    ++stats_.other_layout_gap_draws;
  }

  bool ShouldSampleDraw(const DrawEvent& event, NativeReplaySupport replay_support) const {
    if (REXCVAR_GET(sw2e_native_renderer_dump_gap_samples_only)) {
      return IsNativeReplayGapSupport(replay_support);
    }
    if (REXCVAR_GET(sw2e_native_renderer_dump_priority_samples_only)) {
      return IsPrioritySampleDraw(event);
    }
    return true;
  }

  void HashGuestMemorySamples(const DrawEvent& event, const DrawEventContext& context,
                              NativeReplaySupport replay_support) {
    if (!REXCVAR_GET(sw2e_native_renderer_hash_memory) || !context.memory) {
      return;
    }

    if (!ShouldSampleDraw(event, replay_support)) {
      return;
    }

    const int32_t configured_limit = REXCVAR_GET(sw2e_native_renderer_memory_hash_bytes);
    if (configured_limit <= 0) {
      return;
    }
    const uint32_t hash_limit = static_cast<uint32_t>(configured_limit);

    const uint32_t index_bytes_needed = IndexBytesNeeded(event);
    if (index_bytes_needed != 0) {
      const uint32_t requested_size = std::min(index_bytes_needed, hash_limit);
      uint32_t bytes_hashed = 0;
      const uint64_t hash = HashPhysicalMemorySample(context.memory, event.index_guest_base,
                                                     requested_size, bytes_hashed);
      if (bytes_hashed != 0) {
        MixHash(stats_.index_memory_hash, hash);
        ++stats_.hashed_index_buffers;
        stats_.index_sample_bytes += bytes_hashed;
        if (auto sample_path = MaybeDumpSample('i', event, replay_support, 0,
                                               event.index_guest_base, bytes_hashed, hash,
                                               context.memory)) {
          WriteIndexSampleMetadata(event, replay_support, index_bytes_needed, bytes_hashed, hash,
                                   *sample_path);
        }
      }
    }

    for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
      const auto& fetch = event.vertex_fetches[i];
      const uint32_t requested_size = std::min(fetch.size_bytes, hash_limit);
      uint32_t bytes_hashed = 0;
      const uint64_t hash = HashPhysicalMemorySample(context.memory, fetch.address_bytes,
                                                     requested_size, bytes_hashed);
      if (bytes_hashed != 0) {
        MixHash(stats_.vertex_memory_hash, hash);
        ++stats_.hashed_vertex_fetches;
        stats_.vertex_sample_bytes += bytes_hashed;
        if (auto sample_path = MaybeDumpSample('v', event, replay_support,
                                               fetch.fetch_constant, fetch.address_bytes,
                                               bytes_hashed, hash, context.memory)) {
          WriteVertexSampleMetadata(event, replay_support, fetch, bytes_hashed, hash,
                                    *sample_path);
        }
      }
    }

    for (uint32_t i = 0; i < event.texture_fetch_summary_count; ++i) {
      const auto& fetch = event.texture_fetches[i];
      const TextureDumpPlan dump_plan = BuildTextureDumpPlan(fetch, hash_limit);
      uint32_t bytes_hashed = 0;
      const uint64_t hash = HashPhysicalMemorySample(context.memory, fetch.base_address_bytes,
                                                     dump_plan.requested_size, bytes_hashed);
      if (bytes_hashed != 0) {
        MixHash(stats_.texture_memory_hash, hash);
        ++stats_.hashed_texture_fetches;
        stats_.texture_sample_bytes += bytes_hashed;
        if (auto sample_path = MaybeDumpSample('t', event, replay_support,
                                               fetch.fetch_constant, fetch.base_address_bytes,
                                               bytes_hashed, hash, context.memory)) {
          WriteTextureSampleMetadata(event, replay_support, fetch, dump_plan, bytes_hashed, hash,
                                     *sample_path);
        }
      }
    }
  }

  std::optional<std::filesystem::path> MaybeDumpSample(char kind, const DrawEvent& event,
                                                       NativeReplaySupport replay_support,
                                                       uint32_t fetch_constant, uint32_t address,
                                                       uint32_t size, uint64_t hash,
                                                       const rex::memory::Memory* memory) {
    if (!REXCVAR_GET(sw2e_native_renderer_dump_samples) || !memory || size == 0) {
      return std::nullopt;
    }

    if (!ShouldSampleDraw(event, replay_support)) {
      return std::nullopt;
    }

    const int32_t limit = REXCVAR_GET(sw2e_native_renderer_dump_sample_limit);
    if (limit <= 0 || dumped_sample_keys_.size() >= static_cast<size_t>(limit)) {
      return std::nullopt;
    }

    const uint64_t key = SampleKey(address, size, hash, kind);
    if (!dumped_sample_keys_.insert(key).second) {
      return std::nullopt;
    }

    const std::filesystem::path path =
        SamplePath(kind, event.frame_index, event.draw_index, fetch_constant, address, size, hash);
    if (!rex::filesystem::CreateParentFolder(path)) {
      REXLOG_WARN("SW2E native sidecar failed to create sample folder for {}", path.string());
      return std::nullopt;
    }

    FILE* file = rex::filesystem::OpenFile(path, "wb");
    if (!file) {
      REXLOG_WARN("SW2E native sidecar failed to open sample {}", path.string());
      return std::nullopt;
    }

    const uint8_t* data = memory->TranslatePhysical<const uint8_t*>(address);
    const size_t written = std::fwrite(data, 1, size, file);
    std::fclose(file);
    if (written != size) {
      REXLOG_WARN("SW2E native sidecar short sample write {}: {}/{}", path.string(), written, size);
      return std::nullopt;
    }

    return path;
  }

  bool EnsureSampleMetadataOpen() {
    const std::filesystem::path root = rex::to_path(REXCVAR_GET(sw2e_native_renderer_sample_root));
    if (root.empty()) {
      return false;
    }

    if (sample_metadata_file_ && root == sample_metadata_root_) {
      return true;
    }

    CloseSampleMetadata();
    const std::filesystem::path path = root / "samples.jsonl";
    if (!rex::filesystem::CreateParentFolder(path)) {
      REXLOG_WARN("SW2E native sidecar failed to create sample metadata folder for {}",
                  path.string());
      return false;
    }

    sample_metadata_file_ = rex::filesystem::OpenFile(path, "wb");
    if (!sample_metadata_file_) {
      REXLOG_WARN("SW2E native sidecar failed to open sample metadata {}", path.string());
      return false;
    }

    sample_metadata_root_ = root;
    return true;
  }

  void WriteSampleMetadataPrefix(const DrawEvent& event, const std::filesystem::path& sample_path,
                                 NativeReplaySupport replay_support, const char* kind,
                                 uint32_t fetch_constant, uint32_t address, uint32_t sampled_size,
                                 uint64_t hash) {
    std::fprintf(
        sample_metadata_file_,
        "{\"event\":\"sample\",\"kind\":\"%s\",\"file\":\"%s\",\"frame\":%u,\"draw\":%u,"
        "\"fetch_constant\":%u,\"address\":\"0x%08X\",\"sampled_size\":%u,"
        "\"hash\":\"0x%016llX\",\"native_replay_support\":\"%s\","
        "\"primitive\":%u,\"index_count\":%u,\"indexed\":%s,"
        "\"vertex_shader\":\"0x%016llX\",\"pixel_shader\":\"0x%016llX\"",
        kind, sample_path.filename().string().c_str(), event.frame_index, event.draw_index,
        fetch_constant, address, sampled_size, static_cast<unsigned long long>(hash),
        NativeReplaySupportName(replay_support),
        event.primitive_type, event.index_count, event.indexed ? "true" : "false",
        static_cast<unsigned long long>(event.vertex_shader_hash),
        static_cast<unsigned long long>(event.pixel_shader_hash));
    WriteFloatConstants("vertex_float_constant_values", event.vertex_float_constants,
                        event.vertex_float_constant_summary_count);
    WriteFloatConstants("pixel_float_constant_values", event.pixel_float_constants,
                        event.pixel_float_constant_summary_count);
  }

  void WriteFloatConstants(
      const char* name,
      const rex::graphics::native_render::FloatConstantSummary* constants,
      uint32_t count) {
    std::fprintf(sample_metadata_file_, ",\"%s\":[", name);
    const uint32_t constant_count =
        std::min(count, rex::graphics::native_render::kMaxFloatConstantSummariesPerDraw);
    for (uint32_t i = 0; i < constant_count; ++i) {
      const auto& constant = constants[i];
      if (i) {
        std::fputc(',', sample_metadata_file_);
      }
      std::fprintf(sample_metadata_file_, "{\"index\":%u,\"values\":[",
                   constant.constant_index);
      for (uint32_t component = 0; component < 4; ++component) {
        if (component) {
          std::fputc(',', sample_metadata_file_);
        }
        WriteJsonFloat(sample_metadata_file_, constant.values[component]);
      }
      std::fprintf(sample_metadata_file_,
                   "],\"bits\":[\"0x%08X\",\"0x%08X\",\"0x%08X\",\"0x%08X\"]}",
                   FloatBits(constant.values[0]), FloatBits(constant.values[1]),
                   FloatBits(constant.values[2]), FloatBits(constant.values[3]));
    }
    std::fputc(']', sample_metadata_file_);
  }

  void WriteVertexSampleMetadata(const DrawEvent& event, NativeReplaySupport replay_support,
                                 const rex::graphics::native_render::VertexFetchSummary& fetch,
                                 uint32_t sampled_size, uint64_t hash,
                                 const std::filesystem::path& sample_path) {
    if (!EnsureSampleMetadataOpen()) {
      return;
    }

    WriteSampleMetadataPrefix(event, sample_path, replay_support, "vertex", fetch.fetch_constant,
                              fetch.address_bytes, sampled_size, hash);
    const uint32_t attribute_summary_count =
        fetch.attribute_summary_count <
                rex::graphics::native_render::kMaxVertexAttributeSummariesPerFetch
            ? fetch.attribute_summary_count
            : rex::graphics::native_render::kMaxVertexAttributeSummariesPerFetch;
    std::fprintf(sample_metadata_file_,
                 ",\"vertex\":{\"type\":%u,\"full_size\":%u,\"endian\":%u,"
                 "\"stride_words\":%u,\"attribute_count\":%u,"
                 "\"attribute_summary_count\":%u,\"attributes\":[",
                 fetch.type, fetch.size_bytes, fetch.endian, fetch.stride_words,
                 fetch.attribute_count, attribute_summary_count);
    for (uint32_t attribute_index = 0; attribute_index < attribute_summary_count;
         ++attribute_index) {
      const auto& attribute = fetch.attributes[attribute_index];
      if (attribute_index) {
        std::fputc(',', sample_metadata_file_);
      }
      std::fprintf(sample_metadata_file_,
                   "{\"data_format\":%u,\"offset_words\":%d,\"exp_adjust\":%d,"
                   "\"is_signed\":%s,\"is_integer\":%s,\"result_storage_target\":%u,"
                   "\"result_storage_index\":%u,\"result_write_mask\":%u,"
                   "\"result_used_components\":%u,\"result_swizzle\":%u}",
                   attribute.data_format, attribute.offset_words, attribute.exp_adjust,
                   attribute.is_signed ? "true" : "false",
                   attribute.is_integer ? "true" : "false",
                   attribute.result_storage_target, attribute.result_storage_index,
                   attribute.result_write_mask, attribute.result_used_components,
                   attribute.result_swizzle);
    }
    std::fprintf(sample_metadata_file_, "]}}\n");
    MaybeFlushSampleMetadata();
  }

  void WriteIndexSampleMetadata(const DrawEvent& event, NativeReplaySupport replay_support,
                                uint32_t full_size,
                                uint32_t sampled_size, uint64_t hash,
                                const std::filesystem::path& sample_path) {
    if (!EnsureSampleMetadataOpen()) {
      return;
    }

    WriteSampleMetadataPrefix(event, sample_path, replay_support, "index", 0, event.index_guest_base,
                              sampled_size, hash);
    std::fprintf(sample_metadata_file_,
                 ",\"index\":{\"base_address\":\"0x%08X\",\"full_size\":%u,"
                 "\"format\":%u,\"element_size\":%u,\"endian\":%u}}\n",
                 event.index_guest_base, full_size, event.index_format,
                 IndexElementSizeBytes(event.index_format), event.index_endianness);
    MaybeFlushSampleMetadata();
  }

  void WriteTextureSampleMetadata(const DrawEvent& event, NativeReplaySupport replay_support,
                                  const TextureFetchSummary& fetch,
                                  const TextureDumpPlan& dump_plan,
                                  uint32_t sampled_size, uint64_t hash,
                                  const std::filesystem::path& sample_path) {
    if (!EnsureSampleMetadataOpen()) {
      return;
    }

    const bool dumped_full =
        dump_plan.requested_full && sampled_size == dump_plan.expected_full_size;
    const char* pc_format_json = dump_plan.known_pc_format ? "\"BC3_UNORM\"" : "null";

    WriteSampleMetadataPrefix(event, sample_path, replay_support, "texture", fetch.fetch_constant,
                              fetch.base_address_bytes, sampled_size, hash);
    std::fprintf(sample_metadata_file_,
                 ",\"dump_full\":%s,\"expected_full_size\":%u"
                 ",\"texture\":{\"type\":%u,\"base_address\":\"0x%08X\","
                 "\"mip_address\":\"0x%08X\",\"width\":%u,\"height\":%u,\"depth\":%u,"
                 "\"dimension\":%u,\"format\":%u,\"endian\":%u,\"tiled\":%u,"
                 "\"pitch\":%u,\"stacked\":%u,\"pc_format\":%s,"
                 "\"row_pitch_bytes\":%u,\"width_blocks\":%u,\"height_blocks\":%u,"
                 "\"array_size\":%u,\"block_bytes\":%u}}\n",
                 dumped_full ? "true" : "false", dump_plan.expected_full_size, fetch.type,
                 fetch.base_address_bytes, fetch.mip_address_bytes, fetch.width, fetch.height,
                 fetch.depth, fetch.dimension, fetch.format, fetch.endian, fetch.tiled,
                 fetch.pitch, fetch.stacked, pc_format_json, dump_plan.row_pitch_bytes,
                 dump_plan.width_blocks, dump_plan.height_blocks, dump_plan.array_size,
                 dump_plan.block_bytes);
    MaybeFlushSampleMetadata();
  }

  void MaybeFlushSampleMetadata() {
    if (!sample_metadata_file_ || (dumped_sample_keys_.size() & 0x3Fu) != 0) {
      return;
    }

    std::fflush(sample_metadata_file_);
  }

  const ShaderPairCounter* TopShaderPair() const {
    const ShaderPairCounter* top = nullptr;
    for (const auto& pair : stats_.shader_pairs) {
      if (pair.draws == 0) {
        continue;
      }
      if (!top || pair.draws > top->draws) {
        top = &pair;
      }
    }
    return top;
  }

  const TransformGapCounter* TopGap(const std::array<TransformGapCounter, 8>& gaps) const {
    const TransformGapCounter* top = nullptr;
    for (const auto& gap : gaps) {
      if (gap.draws == 0) {
        continue;
      }
      if (!top || gap.draws > top->draws) {
        top = &gap;
      }
    }
    return top;
  }

  const DepthOnlyCounter* TopDepthOnly() const {
    const DepthOnlyCounter* top = nullptr;
    for (const auto& depth_only : stats_.depth_only) {
      if (depth_only.draws == 0) {
        continue;
      }
      if (!top || depth_only.draws > top->draws) {
        top = &depth_only;
      }
    }
    return top;
  }

  void LogReplayGap(const SwapEvent& event, const TransformGapCounter* gap, const char* kind,
                    uint32_t other_draws) const {
    if (!gap) {
      return;
    }

    REXLOG_INFO(
        "SW2E native {} gap frame {}: draws={} other={} prim={} indexed={} "
        "VS={:#018x} PS={:#018x} vfetch_c={} stride_words={} attrs={} attr_sig={:#018x} "
        "a0=fmt{}@w{}->t{}i{}m{}u{}s{} a1=fmt{}@w{}->t{}i{}m{}u{}s{} "
        "a2=fmt{}@w{}->t{}i{}m{}u{}s{} a3=fmt{}@w{}->t{}i{}m{}u{}s{} "
        "tfetches={} tex0_format={} tex0_dim={} tex0_tiled={}",
        kind, event.frame_index, gap->draws, other_draws, gap->primitive_type, gap->indexed,
        gap->vertex_hash, gap->pixel_hash, gap->fetch_constant,
        gap->stride_words, gap->attribute_count, gap->attribute_signature_hash,
        gap->attributes[0].data_format, gap->attributes[0].offset_words,
        gap->attributes[0].result_storage_target, gap->attributes[0].result_storage_index,
        gap->attributes[0].result_write_mask, gap->attributes[0].result_used_components,
        gap->attributes[0].result_swizzle, gap->attributes[1].data_format,
        gap->attributes[1].offset_words, gap->attributes[1].result_storage_target,
        gap->attributes[1].result_storage_index, gap->attributes[1].result_write_mask,
        gap->attributes[1].result_used_components, gap->attributes[1].result_swizzle,
        gap->attributes[2].data_format, gap->attributes[2].offset_words,
        gap->attributes[2].result_storage_target, gap->attributes[2].result_storage_index,
        gap->attributes[2].result_write_mask, gap->attributes[2].result_used_components,
        gap->attributes[2].result_swizzle, gap->attributes[3].data_format,
        gap->attributes[3].offset_words, gap->attributes[3].result_storage_target,
        gap->attributes[3].result_storage_index, gap->attributes[3].result_write_mask,
        gap->attributes[3].result_used_components, gap->attributes[3].result_swizzle,
        gap->texture_fetch_count,
        gap->first_texture_format, gap->first_texture_dimension, gap->first_texture_tiled);
  }

  void LogTopDepthOnly(const SwapEvent& event) const {
    const DepthOnlyCounter* top = TopDepthOnly();
    if (!top) {
      return;
    }

    REXLOG_INFO(
        "SW2E native depth-only frame {}: draws={} other={} prim={} indices={} indexed={} "
        "depth=0x{:08x} z_enable={} z_write={} z_func={} stencil={} rb_depth=0x{:08x} "
        "color_mask=0x{:08x} pixel_targets=0x{:08x} raster={} pixel_needed={} "
        "vfetches={} tfetches={} VS={:#018x} PS={:#018x}",
        event.frame_index, top->draws, stats_.other_depth_only_draws, top->primitive_type,
        top->index_count, top->indexed, top->normalized_depthcontrol,
        NativeDepthControlZEnable(top->normalized_depthcontrol),
        NativeDepthControlZWriteEnable(top->normalized_depthcontrol),
        NativeDepthControlZFunc(top->normalized_depthcontrol),
        NativeDepthControlStencilEnable(top->normalized_depthcontrol), top->rb_depth_info,
        top->normalized_color_mask, top->pixel_color_target_mask,
        top->rasterization_potentially_done, top->pixel_shader_needed,
        top->vertex_fetch_count, top->texture_fetch_count, top->vertex_hash, top->pixel_hash);
  }

  void LogTopTransformGap(const SwapEvent& event) const {
    LogReplayGap(event, TopGap(stats_.transform_gaps), "transform", stats_.other_transform_gap_draws);
  }

  void LogTopLayoutGap(const SwapEvent& event) const {
    LogReplayGap(event, TopGap(stats_.layout_gaps), "layout", stats_.other_layout_gap_draws);
  }

  void LogFrame(const SwapEvent& event) const {
    const ShaderPairCounter* top = TopShaderPair();
    const uint32_t min_fetch =
        stats_.min_vertex_fetch_addr == UINT32_MAX ? 0 : stats_.min_vertex_fetch_addr;
    const uint32_t min_index = stats_.min_index_addr == UINT32_MAX ? 0 : stats_.min_index_addr;
    if (top) {
      REXLOG_INFO(
          "SW2E native sidecar frame {} swap#{}: draws={} indexed={} index_samples={} "
          "vfetch_draws={} "
          "tfetch_draws={} vfetches={} tfetches={} memexport={} om_writes={} "
          "noout_skip={} noout_point={} raster_noout={} viz_query={} "
          "native_supported={} native_tex={} native_solid={} native_depth={} native_projected={} "
          "native_indexed={} "
          "depth_only={} "
          "unsupported_output(indexed/shape/layout/texture/transform)={}/{}/{}/{}/{} "
          "vertex_range={:#010x}-{:#010x} index_range={:#010x}-{:#010x} "
          "frontbuffer={:#010x} {}x{} "
          "vmem_hash={:#018x} tmem_hash={:#018x} imem_hash={:#018x} "
          "hashed_vfetches={} hashed_tfetches={} hashed_indices={} "
          "sample_bytes={}/{}/{} top_vs={:#018x} top_ps={:#018x} top_draws={} other_pairs={}",
          event.frame_index, swap_count_, stats_.draws, stats_.indexed_draws,
          stats_.indexed_sample_draws,
          stats_.vertex_fetch_draws, stats_.texture_fetch_draws, stats_.vertex_fetch_count,
          stats_.texture_fetch_count, stats_.memexport_draws,
          stats_.output_merger_write_draws, stats_.no_output_skip_candidate_draws,
          stats_.no_output_point_list_draws, stats_.rasterized_no_output_draws,
          stats_.viz_query_draws, stats_.native_replay_supported_draws,
          stats_.native_replay_supported_textured_draws,
          stats_.native_replay_supported_solid_draws,
          stats_.native_replay_supported_depth_only_draws,
          stats_.native_replay_supported_projected_draws,
          stats_.native_replay_supported_indexed_draws, stats_.native_replay_depth_only_draws,
          stats_.native_replay_unsupported_indexed_draws,
          stats_.native_replay_unsupported_shape_draws,
          stats_.native_replay_unsupported_layout_draws,
          stats_.native_replay_unsupported_texture_draws,
          stats_.native_replay_unsupported_transform_draws, min_fetch,
          stats_.max_vertex_fetch_end, min_index, stats_.max_index_end, event.frontbuffer_ptr,
          event.frontbuffer_width, event.frontbuffer_height, stats_.vertex_memory_hash,
          stats_.texture_memory_hash, stats_.index_memory_hash, stats_.hashed_vertex_fetches,
          stats_.hashed_texture_fetches, stats_.hashed_index_buffers, stats_.vertex_sample_bytes,
          stats_.texture_sample_bytes, stats_.index_sample_bytes,
          top->vertex_hash, top->pixel_hash, top->draws, stats_.other_shader_pair_draws);
      LogTopTransformGap(event);
      LogTopLayoutGap(event);
      LogTopDepthOnly(event);
      return;
    }

    REXLOG_INFO(
        "SW2E native sidecar frame {} swap#{}: draws={} indexed={} index_samples={} "
        "vfetch_draws={} "
        "tfetch_draws={} vfetches={} tfetches={} memexport={} om_writes={} "
        "noout_skip={} noout_point={} raster_noout={} viz_query={} "
        "native_supported={} native_tex={} native_solid={} native_depth={} native_projected={} "
        "native_indexed={} "
        "depth_only={} "
        "unsupported_output(indexed/shape/layout/texture/transform)={}/{}/{}/{}/{} "
        "vertex_range={:#010x}-{:#010x} index_range={:#010x}-{:#010x} "
        "frontbuffer={:#010x} {}x{} "
        "vmem_hash={:#018x} tmem_hash={:#018x} imem_hash={:#018x} "
        "hashed_vfetches={} hashed_tfetches={} hashed_indices={} sample_bytes={}/{}/{}",
        event.frame_index, swap_count_, stats_.draws, stats_.indexed_draws,
        stats_.indexed_sample_draws,
        stats_.vertex_fetch_draws, stats_.texture_fetch_draws, stats_.vertex_fetch_count,
        stats_.texture_fetch_count, stats_.memexport_draws, stats_.output_merger_write_draws,
        stats_.no_output_skip_candidate_draws, stats_.no_output_point_list_draws,
        stats_.rasterized_no_output_draws, stats_.viz_query_draws,
        stats_.native_replay_supported_draws, stats_.native_replay_supported_textured_draws,
        stats_.native_replay_supported_solid_draws,
        stats_.native_replay_supported_depth_only_draws,
        stats_.native_replay_supported_projected_draws,
        stats_.native_replay_supported_indexed_draws, stats_.native_replay_depth_only_draws,
        stats_.native_replay_unsupported_indexed_draws,
        stats_.native_replay_unsupported_shape_draws,
        stats_.native_replay_unsupported_layout_draws,
        stats_.native_replay_unsupported_texture_draws,
        stats_.native_replay_unsupported_transform_draws, min_fetch,
        stats_.max_vertex_fetch_end, min_index, stats_.max_index_end, event.frontbuffer_ptr,
        event.frontbuffer_width, event.frontbuffer_height, stats_.vertex_memory_hash,
        stats_.texture_memory_hash, stats_.index_memory_hash, stats_.hashed_vertex_fetches,
        stats_.hashed_texture_fetches, stats_.hashed_index_buffers, stats_.vertex_sample_bytes,
        stats_.texture_sample_bytes, stats_.index_sample_bytes);
    LogTopTransformGap(event);
    LogTopLayoutGap(event);
    LogTopDepthOnly(event);
  }

  uint32_t swap_count_ = 0;
  FrameStats stats_ = {};
  std::unordered_set<uint64_t> dumped_sample_keys_;
  std::unordered_set<uint64_t> native_gpu_replay_draw_keys_;
  std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>>
      native_gpu_replay_texture_bytes_cache_;
  std::vector<gpu_replay::ReplayDraw> native_gpu_replay_draws_;
  std::vector<gpu_replay::ReplayDraw> native_gpu_replay_last_completed_draws_;
  std::vector<gpu_replay::ReplayDraw> native_gpu_replay_best_completed_draws_;
  uint32_t native_gpu_replay_best_completed_score_ = 0;
  bool native_gpu_replay_flushed_ = false;
  uint32_t native_gpu_replay_live_present_count_ = 0;
  uint32_t native_gpu_replay_captured_draw_log_count_ = 0;
  uint32_t native_gpu_replay_projected_reject_log_count_ = 0;
  uint32_t native_gpu_replay_completed_pass_count_ = 0;
  bool suppress_current_backend_swap_ = false;
  void* native_window_handle_ = nullptr;
  FILE* sample_metadata_file_ = nullptr;
  std::filesystem::path sample_metadata_root_;
};

Sidecar g_sidecar;
bool g_installed = false;

}  // namespace

void Install(void* native_window_handle) {
  if (g_installed || !REXCVAR_GET(sw2e_native_renderer)) {
    return;
  }

  g_sidecar.SetNativeWindowHandle(native_window_handle);
  g_sidecar.Reset(0);
  rex::graphics::native_render::SetEventSink(&g_sidecar);
  g_installed = true;
  REXLOG_INFO("SW2E native renderer sidecar enabled");
}

void Shutdown() {
  if (!g_installed) {
    return;
  }

  g_sidecar.CloseSampleMetadata();
  g_sidecar.FlushNativeGpuReplay();
  rex::graphics::native_render::SetEventSink(nullptr);
  g_installed = false;
  REXLOG_INFO("SW2E native renderer sidecar disabled");
}

}  // namespace sw2e::native_renderer
