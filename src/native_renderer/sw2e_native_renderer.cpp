#include "native_renderer/sw2e_native_renderer.h"

#include "native_renderer/sw2e_native_gpu_replay.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/native_render_event_stream.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
                    "Capture supported live menu draws and replay them through a native PC GPU path");

REXCVAR_DEFINE_STRING(sw2e_native_renderer_gpu_replay_path,
                      "extracted/native_render_samples/native_gpu_replay.bmp",
                      "SM2/Native Render", "Output BMP path for native GPU menu replay");

REXCVAR_DEFINE_INT32(sw2e_native_renderer_gpu_replay_draw_limit, 7, "SM2/Native Render",
                     "Maximum live menu draws captured for the native GPU replay")
    .range(0, 256);

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
  uint32_t native_replay_supported_indexed_draws = 0;
  uint32_t native_replay_depth_only_draws = 0;
  uint32_t native_replay_unsupported_indexed_draws = 0;
  uint32_t native_replay_unsupported_shape_draws = 0;
  uint32_t native_replay_unsupported_layout_draws = 0;
  uint32_t native_replay_unsupported_texture_draws = 0;
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

constexpr uint32_t kGuestPhysicalMemoryBytes = 0x20000000;
constexpr uint64_t kFnv1a64Offset = 14695981039346656037ull;
constexpr uint64_t kFnv1a64Prime = 1099511628211ull;
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
constexpr uint32_t kNativeGpuReplayPassLogInitial = 8;
constexpr uint32_t kNativeGpuReplayPassLogInterval = 60;
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

float ReadF32(const uint8_t* data, uint32_t endian) {
  uint32_t bits = 0;
  if (endian == 2) {
    bits = ReadBe32(data);
  } else {
    std::memcpy(&bits, data, sizeof(bits));
  }

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
    max_index = std::max(max_index, index);
  }
  return true;
}

bool ExpandNativeReplayTriangleStripIndices(const std::vector<uint32_t>& strip_indices,
                                            std::vector<uint32_t>& triangle_indices) {
  triangle_indices.clear();
  if (strip_indices.size() < 3) {
    return false;
  }

  triangle_indices.reserve((strip_indices.size() - 2) * 3);
  for (size_t i = 0; i + 2 < strip_indices.size(); ++i) {
    const uint32_t a = strip_indices[i + 0];
    const uint32_t b = strip_indices[i + 1];
    const uint32_t c = strip_indices[i + 2];
    if (a == b || a == c || b == c) {
      continue;
    }

    if ((i & 1) == 0) {
      triangle_indices.push_back(a);
      triangle_indices.push_back(b);
      triangle_indices.push_back(c);
    } else {
      triangle_indices.push_back(b);
      triangle_indices.push_back(a);
      triangle_indices.push_back(c);
    }
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

  if (fetch.format != kXenosFormatDxt45 || fetch.tiled != 0 ||
      fetch.dimension != kXenosDimension2DOrStacked || fetch.width == 0 || fetch.height == 0) {
    return plan;
  }

  plan.known_pc_format = true;
  plan.block_bytes = kBc3BytesPerBlock;
  plan.width_blocks = AlignUp(fetch.width, kBc3BlockWidth) / kBc3BlockWidth;
  plan.height_blocks = AlignUp(fetch.height, kBc3BlockHeight) / kBc3BlockHeight;
  plan.array_size = std::max(fetch.depth, uint32_t(1));

  const uint32_t pitch_texels = fetch.pitch ? (fetch.pitch << 5) : fetch.width;
  const uint32_t row_pitch_blocks =
      AlignUp(AlignUp(std::max(pitch_texels, fetch.width), kBc3BlockWidth) / kBc3BlockWidth,
              kTextureTileWidthHeightBlocks);
  const uint64_t row_pitch_bytes64 = uint64_t(row_pitch_blocks) * kBc3BytesPerBlock;
  const uint64_t one_slice_data_extent =
      row_pitch_bytes64 * (plan.height_blocks - 1) +
      uint64_t(plan.width_blocks) * kBc3BytesPerBlock;
  const uint64_t slice_stride =
      AlignUp64(row_pitch_bytes64 * AlignUp(plan.height_blocks, kTextureTileWidthHeightBlocks),
                kTextureSubresourceAlignmentBytes);
  const uint64_t full_size =
      slice_stride * (plan.array_size - 1) + one_slice_data_extent;

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

uint32_t NativeGpuReplayPassScore(const std::vector<gpu_replay::ReplayDraw>& draws) {
  uint32_t textured_draws = 0;
  for (const auto& draw : draws) {
    textured_draws += draw.kind == gpu_replay::ReplayDrawKind::kTexturedTriangles ? 1 : 0;
  }

  return textured_draws * 1000u + static_cast<uint32_t>(draws.size());
}

const VertexFetchSummary* FindNativeReplayTexturedVertexFetch(const DrawEvent& event) {
  for (uint32_t i = 0; i < event.vertex_fetch_summary_count; ++i) {
    const auto& candidate = event.vertex_fetches[i];
    if (CanDecodeNativeReplayTexturedVertexFetch(candidate) &&
        (event.indexed || candidate.size_bytes >= event.index_count * candidate.stride_words * 4)) {
      return &candidate;
    }
  }
  return nullptr;
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
  kDepthOnly,
  kUnsupportedIndexed,
  kUnsupportedShape,
  kUnsupportedLayout,
  kUnsupportedTexture,
};

NativeReplaySupport ClassifyNativeReplaySupport(const DrawEvent& event) {
  if (IsNativeNoOutputSkipCandidate(event)) {
    return NativeReplaySupport::kNoOutputSkipCandidate;
  }
  if (!HasNativeReplayColorOutput(event)) {
    return NativeReplaySupport::kDepthOnly;
  }
  if (IsNativeReplayTexturedTriangleShape(event)) {
    if (!FindNativeReplayTexturedVertexFetch(event)) {
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

  const bool solid_shape =
      (event.primitive_type == 8 && event.index_count == 3) ||
      (event.primitive_type == 6 && event.index_count == 4);
  if (solid_shape && event.texture_fetch_summary_count == 0 && event.vertex_fetch_summary_count != 0) {
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
    native_gpu_replay_completed_pass_count_ = 0;
    suppress_current_backend_swap_ = false;
    CloseSampleMetadata();
    ResetFrame(stats_, frame_index);
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
    switch (ClassifyNativeReplaySupport(event)) {
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
      case NativeReplaySupport::kDepthOnly:
        ++stats_.native_replay_depth_only_draws;
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
      case NativeReplaySupport::kNoOutputSkipCandidate:
        break;
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

    HashGuestMemorySamples(event, context);
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
    if (draw_limit <= 0 || native_gpu_replay_draws_.size() >= static_cast<size_t>(draw_limit)) {
      return;
    }

    bool captured = CaptureNativeGpuReplayTexturedDraw(event, context);
    if (!captured && REXCVAR_GET(sw2e_native_renderer_gpu_replay_include_solid_geometry)) {
      captured = CaptureNativeGpuReplaySolidDraw(event, context);
    }
    if (!captured) {
      return;
    }

    if (native_gpu_replay_draws_.size() >= static_cast<size_t>(draw_limit)) {
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
                                ? ExpandNativeReplayTriangleStripIndices(indices, triangle_indices)
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
    replay_draw.rt0_blendcontrol = event.rt_blendcontrol[0];
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

  bool CaptureNativeGpuReplaySolidDraw(const DrawEvent& event, const DrawEventContext& context) {
    if (!HasNativeReplayColorOutput(event)) {
      return false;
    }
    if (event.indexed || event.vertex_fetch_summary_count == 0 ||
        event.texture_fetch_summary_count != 0) {
      return false;
    }
    if (!((event.primitive_type == 8 && event.index_count == 3) ||
          (event.primitive_type == 6 && event.index_count == 4))) {
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

    const std::array<float, 4> solid_color = NativeGpuReplaySolidColor(event);
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
    replay_draw.kind = gpu_replay::ReplayDrawKind::kSolidTriangles;
    replay_draw.frame = event.frame_index;
    replay_draw.draw = event.draw_index;
    replay_draw.rt0_blendcontrol = event.rt_blendcontrol[0];
    replay_draw.rt0_write_mask = static_cast<uint8_t>(event.normalized_color_mask & 0x0F);
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

    QueueNativeGpuReplayDraw(std::move(replay_draw), event.primitive_type == 8 ? "rectangle" : "strip");
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

  void CompleteNativeGpuReplayPass(const char* reason) {
    if (native_gpu_replay_draws_.empty()) {
      return;
    }

    const size_t completed_draw_count = native_gpu_replay_draws_.size();
    const gpu_replay::ReplayDraw& first_draw = native_gpu_replay_draws_.front();
    const gpu_replay::ReplayDraw& last_draw = native_gpu_replay_draws_.back();
    const bool owns_current_frame = NativeGpuReplayOwnsCurrentFrame(completed_draw_count);
    bool presented = false;
    if (REXCVAR_GET(sw2e_native_renderer_gpu_replay_live_present)) {
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
    }

    native_gpu_replay_last_completed_draws_ = std::move(native_gpu_replay_draws_);
    native_gpu_replay_draws_.clear();
    native_gpu_replay_draw_keys_.clear();
    native_gpu_replay_texture_bytes_cache_.clear();
  }

  bool NativeGpuReplayOwnsCurrentFrame(size_t completed_draw_count) const {
    return completed_draw_count == stats_.native_replay_supported_draws &&
           stats_.native_replay_supported_draws != 0 &&
           stats_.native_replay_depth_only_draws == 0 &&
           stats_.native_replay_unsupported_indexed_draws == 0 &&
           stats_.native_replay_unsupported_shape_draws == 0 &&
           stats_.native_replay_unsupported_layout_draws == 0 &&
           stats_.native_replay_unsupported_texture_draws == 0;
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

  void HashGuestMemorySamples(const DrawEvent& event, const DrawEventContext& context) {
    if (!REXCVAR_GET(sw2e_native_renderer_hash_memory) || !context.memory) {
      return;
    }

    if (REXCVAR_GET(sw2e_native_renderer_dump_priority_samples_only) &&
        !IsPrioritySampleDraw(event)) {
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
        if (auto sample_path = MaybeDumpSample('i', event, 0, event.index_guest_base,
                                               bytes_hashed, hash, context.memory)) {
          WriteIndexSampleMetadata(event, index_bytes_needed, bytes_hashed, hash, *sample_path);
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
        if (auto sample_path = MaybeDumpSample('v', event, fetch.fetch_constant,
                                               fetch.address_bytes, bytes_hashed, hash,
                                               context.memory)) {
          WriteVertexSampleMetadata(event, fetch, bytes_hashed, hash, *sample_path);
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
        if (auto sample_path = MaybeDumpSample('t', event, fetch.fetch_constant,
                                               fetch.base_address_bytes, bytes_hashed, hash,
                                               context.memory)) {
          WriteTextureSampleMetadata(event, fetch, dump_plan, bytes_hashed, hash, *sample_path);
        }
      }
    }
  }

  std::optional<std::filesystem::path> MaybeDumpSample(char kind, const DrawEvent& event,
                                                       uint32_t fetch_constant, uint32_t address,
                                                       uint32_t size, uint64_t hash,
                                                       const rex::memory::Memory* memory) {
    if (!REXCVAR_GET(sw2e_native_renderer_dump_samples) || !memory || size == 0) {
      return std::nullopt;
    }

    if (REXCVAR_GET(sw2e_native_renderer_dump_priority_samples_only) &&
        !IsPrioritySampleDraw(event)) {
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
                                 const char* kind, uint32_t fetch_constant, uint32_t address,
                                 uint32_t sampled_size, uint64_t hash) {
    std::fprintf(
        sample_metadata_file_,
        "{\"event\":\"sample\",\"kind\":\"%s\",\"file\":\"%s\",\"frame\":%u,\"draw\":%u,"
        "\"fetch_constant\":%u,\"address\":\"0x%08X\",\"sampled_size\":%u,"
        "\"hash\":\"0x%016llX\",\"primitive\":%u,\"index_count\":%u,\"indexed\":%s,"
        "\"vertex_shader\":\"0x%016llX\",\"pixel_shader\":\"0x%016llX\"",
        kind, sample_path.filename().string().c_str(), event.frame_index, event.draw_index,
        fetch_constant, address, sampled_size, static_cast<unsigned long long>(hash),
        event.primitive_type, event.index_count, event.indexed ? "true" : "false",
        static_cast<unsigned long long>(event.vertex_shader_hash),
        static_cast<unsigned long long>(event.pixel_shader_hash));
  }

  void WriteVertexSampleMetadata(const DrawEvent& event,
                                 const rex::graphics::native_render::VertexFetchSummary& fetch,
                                 uint32_t sampled_size, uint64_t hash,
                                 const std::filesystem::path& sample_path) {
    if (!EnsureSampleMetadataOpen()) {
      return;
    }

    WriteSampleMetadataPrefix(event, sample_path, "vertex", fetch.fetch_constant,
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

  void WriteIndexSampleMetadata(const DrawEvent& event, uint32_t full_size,
                                uint32_t sampled_size, uint64_t hash,
                                const std::filesystem::path& sample_path) {
    if (!EnsureSampleMetadataOpen()) {
      return;
    }

    WriteSampleMetadataPrefix(event, sample_path, "index", 0, event.index_guest_base,
                              sampled_size, hash);
    std::fprintf(sample_metadata_file_,
                 ",\"index\":{\"base_address\":\"0x%08X\",\"full_size\":%u,"
                 "\"format\":%u,\"element_size\":%u,\"endian\":%u}}\n",
                 event.index_guest_base, full_size, event.index_format,
                 IndexElementSizeBytes(event.index_format), event.index_endianness);
    MaybeFlushSampleMetadata();
  }

  void WriteTextureSampleMetadata(const DrawEvent& event,
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

    WriteSampleMetadataPrefix(event, sample_path, "texture", fetch.fetch_constant,
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
          "native_supported={} native_tex={} native_solid={} native_indexed={} depth_only={} "
          "unsupported_output={}/{}/{}/{} "
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
          stats_.native_replay_supported_indexed_draws, stats_.native_replay_depth_only_draws,
          stats_.native_replay_unsupported_indexed_draws,
          stats_.native_replay_unsupported_shape_draws,
          stats_.native_replay_unsupported_layout_draws,
          stats_.native_replay_unsupported_texture_draws, min_fetch, stats_.max_vertex_fetch_end,
          min_index, stats_.max_index_end, event.frontbuffer_ptr, event.frontbuffer_width,
          event.frontbuffer_height, stats_.vertex_memory_hash, stats_.texture_memory_hash,
          stats_.index_memory_hash, stats_.hashed_vertex_fetches, stats_.hashed_texture_fetches,
          stats_.hashed_index_buffers, stats_.vertex_sample_bytes, stats_.texture_sample_bytes,
          stats_.index_sample_bytes,
          top->vertex_hash, top->pixel_hash, top->draws, stats_.other_shader_pair_draws);
      return;
    }

    REXLOG_INFO(
        "SW2E native sidecar frame {} swap#{}: draws={} indexed={} index_samples={} "
        "vfetch_draws={} "
        "tfetch_draws={} vfetches={} tfetches={} memexport={} om_writes={} "
        "noout_skip={} noout_point={} raster_noout={} viz_query={} "
        "native_supported={} native_tex={} native_solid={} native_indexed={} depth_only={} "
        "unsupported_output={}/{}/{}/{} "
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
        stats_.native_replay_supported_solid_draws, stats_.native_replay_supported_indexed_draws,
        stats_.native_replay_depth_only_draws,
        stats_.native_replay_unsupported_indexed_draws,
        stats_.native_replay_unsupported_shape_draws,
        stats_.native_replay_unsupported_layout_draws,
        stats_.native_replay_unsupported_texture_draws, min_fetch, stats_.max_vertex_fetch_end,
        min_index, stats_.max_index_end, event.frontbuffer_ptr, event.frontbuffer_width,
        event.frontbuffer_height, stats_.vertex_memory_hash, stats_.texture_memory_hash,
        stats_.index_memory_hash, stats_.hashed_vertex_fetches, stats_.hashed_texture_fetches,
        stats_.hashed_index_buffers, stats_.vertex_sample_bytes, stats_.texture_sample_bytes,
        stats_.index_sample_bytes);
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
