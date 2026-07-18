#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace sw2e::native_renderer::gpu_replay {

enum class ReplayDrawKind : uint32_t {
  kTexturedTriangles = 0,
  kSolidTriangles = 1,
};

struct ReplayVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 1.0f;
  float u = 0.0f;
  float v = 0.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
};

struct ReplayTexture {
  uint32_t source_base_address = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
  uint32_t row_pitch_bytes = 0;
  uint32_t endian = 0;
  std::shared_ptr<const std::vector<uint8_t>> bytes_ref;
  std::vector<uint8_t> bytes;
};

struct ReplayDraw {
  ReplayDrawKind kind = ReplayDrawKind::kTexturedTriangles;
  uint32_t frame = 0;
  uint32_t draw = 0;
  uint32_t rt0_blendcontrol = 0x00010001;
  uint8_t rt0_write_mask = 0x0F;
  std::vector<ReplayVertex> vertices;
  std::vector<uint32_t> indices;
  ReplayTexture texture;
};

bool RenderMenuReplayD3D11(const std::vector<ReplayDraw>& draws,
                           const std::filesystem::path& output_path, uint32_t width,
                           uint32_t height);

bool InitializeMenuReplayD3D11Child(void* parent_window_handle, uint32_t width, uint32_t height);
bool PresentMenuReplayD3D11Child(void* parent_window_handle, const std::vector<ReplayDraw>& draws,
                                 uint32_t width, uint32_t height);

}  // namespace sw2e::native_renderer::gpu_replay
