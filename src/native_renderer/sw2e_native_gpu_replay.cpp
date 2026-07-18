#include "native_renderer/sw2e_native_gpu_replay.h"

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

#if REX_PLATFORM_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace sw2e::native_renderer::gpu_replay {
namespace {

constexpr uint32_t kEndianNone = 0;
constexpr uint32_t kEndian8In16 = 1;
constexpr uint32_t kEndian8In32 = 2;
constexpr uint32_t kEndian16In32 = 3;

struct BmpFileHeader {
  uint16_t signature = 0x4D42;
  uint32_t file_size = 0;
  uint16_t reserved0 = 0;
  uint16_t reserved1 = 0;
  uint32_t pixel_offset = 54;
};

struct BmpInfoHeader {
  uint32_t header_size = 40;
  int32_t width = 0;
  int32_t height = 0;
  uint16_t planes = 1;
  uint16_t bits_per_pixel = 32;
  uint32_t compression = 0;
  uint32_t image_size = 0;
  int32_t x_pixels_per_meter = 0;
  int32_t y_pixels_per_meter = 0;
  uint32_t colors_used = 0;
  uint32_t important_colors = 0;
};

void WriteU16(FILE* file, uint16_t value) {
  uint8_t bytes[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  std::fwrite(bytes, 1, sizeof(bytes), file);
}

void WriteU32(FILE* file, uint32_t value) {
  uint8_t bytes[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                     static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  std::fwrite(bytes, 1, sizeof(bytes), file);
}

void WriteI32(FILE* file, int32_t value) { WriteU32(file, static_cast<uint32_t>(value)); }

bool WriteBmpTopDownBgra(const std::filesystem::path& output_path, const uint8_t* rgba,
                         uint32_t source_pitch, uint32_t width, uint32_t height) {
  if (!rex::filesystem::CreateParentFolder(output_path)) {
    REXLOG_ERROR("SW2E native GPU replay failed to create output folder for {}",
                 output_path.string());
    return false;
  }

  FILE* file = rex::filesystem::OpenFile(output_path, "wb");
  if (!file) {
    REXLOG_ERROR("SW2E native GPU replay failed to open {}", output_path.string());
    return false;
  }

  const uint32_t row_bytes = width * 4;
  BmpFileHeader file_header;
  BmpInfoHeader info_header;
  info_header.width = static_cast<int32_t>(width);
  info_header.height = -static_cast<int32_t>(height);
  info_header.image_size = row_bytes * height;
  file_header.file_size = file_header.pixel_offset + info_header.image_size;

  WriteU16(file, file_header.signature);
  WriteU32(file, file_header.file_size);
  WriteU16(file, file_header.reserved0);
  WriteU16(file, file_header.reserved1);
  WriteU32(file, file_header.pixel_offset);

  WriteU32(file, info_header.header_size);
  WriteI32(file, info_header.width);
  WriteI32(file, info_header.height);
  WriteU16(file, info_header.planes);
  WriteU16(file, info_header.bits_per_pixel);
  WriteU32(file, info_header.compression);
  WriteU32(file, info_header.image_size);
  WriteI32(file, info_header.x_pixels_per_meter);
  WriteI32(file, info_header.y_pixels_per_meter);
  WriteU32(file, info_header.colors_used);
  WriteU32(file, info_header.important_colors);

  std::vector<uint8_t> bgra(row_bytes);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source = rgba + size_t(y) * source_pitch;
    for (uint32_t x = 0; x < width; ++x) {
      bgra[x * 4 + 0] = source[x * 4 + 2];
      bgra[x * 4 + 1] = source[x * 4 + 1];
      bgra[x * 4 + 2] = source[x * 4 + 0];
      bgra[x * 4 + 3] = source[x * 4 + 3];
    }
    std::fwrite(bgra.data(), 1, bgra.size(), file);
  }

  std::fclose(file);
  return true;
}

void CopySwapBlock(uint32_t endian, uint8_t* output, const uint8_t* input, size_t length) {
  switch (endian) {
    case kEndian8In16:
      for (size_t i = 0; i < length; i += 2) {
        output[i + 0] = input[i + 1];
        output[i + 1] = input[i + 0];
      }
      break;
    case kEndian8In32:
      for (size_t i = 0; i < length; i += 4) {
        output[i + 0] = input[i + 3];
        output[i + 1] = input[i + 2];
        output[i + 2] = input[i + 1];
        output[i + 3] = input[i + 0];
      }
      break;
    case kEndian16In32:
      for (size_t i = 0; i < length; i += 4) {
        output[i + 0] = input[i + 2];
        output[i + 1] = input[i + 3];
        output[i + 2] = input[i + 0];
        output[i + 3] = input[i + 1];
      }
      break;
    case kEndianNone:
    default:
      std::memcpy(output, input, length);
      break;
  }
}

const std::vector<uint8_t>& ReplayTextureBytes(const ReplayTexture& texture) {
  if (texture.bytes_ref) {
    return *texture.bytes_ref;
  }
  return texture.bytes;
}

std::vector<uint8_t> BuildPcBc3TextureBytes(const ReplayTexture& texture) {
  const std::vector<uint8_t>& texture_bytes = ReplayTextureBytes(texture);
  std::vector<uint8_t> output(size_t(texture.row_pitch_bytes) * texture.height_blocks);
  for (uint32_t block_y = 0; block_y < texture.height_blocks; ++block_y) {
    for (uint32_t block_x = 0; block_x < texture.width_blocks; ++block_x) {
      const size_t offset = size_t(block_y) * texture.row_pitch_bytes + size_t(block_x) * 16;
      if (offset + 16 > texture_bytes.size() || offset + 16 > output.size()) {
        continue;
      }
      CopySwapBlock(texture.endian, output.data() + offset, texture_bytes.data() + offset, 16);
    }
  }
  return output;
}

uint64_t HashReplayBytes(const uint8_t* data, size_t size) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

void MixTextureKey(uint64_t& key, uint64_t value) {
  key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
}

uint64_t TextureCacheKey(const ReplayTexture& texture) {
  const std::vector<uint8_t>& texture_bytes = ReplayTextureBytes(texture);
  uint64_t key = 0;
  MixTextureKey(key, texture.source_base_address);
  MixTextureKey(key, texture.width);
  MixTextureKey(key, texture.height);
  MixTextureKey(key, texture.width_blocks);
  MixTextureKey(key, texture.height_blocks);
  MixTextureKey(key, texture.row_pitch_bytes);
  MixTextureKey(key, texture.endian);
  MixTextureKey(key, texture_bytes.size());
  if (texture.source_base_address == 0 && !texture_bytes.empty()) {
    MixTextureKey(key, HashReplayBytes(texture_bytes.data(), texture_bytes.size()));
  }
  return key;
}

#if REX_PLATFORM_WIN32

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

const char kVertexShaderHlsl[] = R"(
cbuffer ViewportParams : register(b0) {
  float2 viewport_size;
  float2 padding;
};

struct VSInput {
  float4 position : POSITION;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

struct VSOutput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

VSOutput main(VSInput input) {
  VSOutput output;
  float2 ndc = float2((input.position.x / viewport_size.x) * 2.0f - 1.0f,
                      1.0f - (input.position.y / viewport_size.y) * 2.0f);
  output.position = float4(ndc, 0.0f, 1.0f);
  output.texcoord = input.texcoord;
  output.color = input.color;
  return output;
}
)";

const char kProjectedVertexShaderHlsl[] = R"(
struct VSInput {
  float4 position : POSITION;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

struct VSOutput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

VSOutput main(VSInput input) {
  VSOutput output;
  output.position = input.position;
  output.texcoord = input.texcoord;
  output.color = input.color;
  return output;
}
)";

const char kPixelShaderHlsl[] = R"(
Texture2D<float4> source_texture : register(t0);
SamplerState source_sampler : register(s0);

struct PSInput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

float4 main(PSInput input) : SV_Target {
  return source_texture.Sample(source_sampler, input.texcoord);
}
)";

const char kProjectedPixelShaderHlsl[] = R"(
Texture2D<float4> source_texture : register(t0);
SamplerState source_sampler : register(s0);

struct PSInput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

float4 main(PSInput input) : SV_Target {
  float4 tex = source_texture.Sample(source_sampler, input.texcoord);
  float3 debug_tint = float3(0.95f, 0.72f, 0.24f);
  float3 color = max(tex.rgb, debug_tint * 0.75f);
  return float4(color, 1.0f);
}
)";

const char kSolidPixelShaderHlsl[] = R"(
struct PSInput {
  float4 position : SV_Position;
  float2 texcoord : TEXCOORD0;
  float4 color : COLOR0;
};

float4 main(PSInput input) : SV_Target {
  return input.color;
}
)";

bool CompileShader(const char* source, const char* entry_point, const char* target,
                   ComPtr<ID3DBlob>& blob) {
  UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

  ComPtr<ID3DBlob> errors;
  HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry_point,
                          target, flags, 0, &blob, &errors);
  if (FAILED(hr)) {
    const char* error_text =
        errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error";
    REXLOG_ERROR("SW2E native GPU replay shader compile failed: {}", error_text);
    return false;
  }
  return true;
}

D3D11_BLEND MapXenosBlendFactor(uint32_t factor, bool alpha_channel) {
  if (alpha_channel) {
    switch (factor) {
      case 0:
        return D3D11_BLEND_ZERO;
      case 1:
        return D3D11_BLEND_ONE;
      case 4:
      case 6:
        return D3D11_BLEND_SRC_ALPHA;
      case 5:
      case 7:
        return D3D11_BLEND_INV_SRC_ALPHA;
      case 8:
      case 10:
        return D3D11_BLEND_DEST_ALPHA;
      case 9:
      case 11:
        return D3D11_BLEND_INV_DEST_ALPHA;
      case 12:
      case 14:
        return D3D11_BLEND_BLEND_FACTOR;
      case 13:
      case 15:
        return D3D11_BLEND_INV_BLEND_FACTOR;
      case 16:
        return D3D11_BLEND_SRC_ALPHA_SAT;
      default:
        return D3D11_BLEND_ZERO;
    }
  }

  switch (factor) {
    case 0:
      return D3D11_BLEND_ZERO;
    case 1:
      return D3D11_BLEND_ONE;
    case 4:
      return D3D11_BLEND_SRC_COLOR;
    case 5:
      return D3D11_BLEND_INV_SRC_COLOR;
    case 6:
      return D3D11_BLEND_SRC_ALPHA;
    case 7:
      return D3D11_BLEND_INV_SRC_ALPHA;
    case 8:
      return D3D11_BLEND_DEST_COLOR;
    case 9:
      return D3D11_BLEND_INV_DEST_COLOR;
    case 10:
      return D3D11_BLEND_DEST_ALPHA;
    case 11:
      return D3D11_BLEND_INV_DEST_ALPHA;
    case 12:
    case 14:
      return D3D11_BLEND_BLEND_FACTOR;
    case 13:
    case 15:
      return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16:
      return D3D11_BLEND_SRC_ALPHA_SAT;
    default:
      return D3D11_BLEND_ZERO;
  }
}

D3D11_BLEND_OP MapXenosBlendOp(uint32_t op) {
  switch (op) {
    case 0:
      return D3D11_BLEND_OP_ADD;
    case 1:
      return D3D11_BLEND_OP_SUBTRACT;
    case 2:
      return D3D11_BLEND_OP_MIN;
    case 3:
      return D3D11_BLEND_OP_MAX;
    case 4:
      return D3D11_BLEND_OP_REV_SUBTRACT;
    default:
      return D3D11_BLEND_OP_ADD;
  }
}

bool IsIdentityBlend(uint32_t blend_control) {
  const uint32_t color_src = blend_control & 0x1F;
  const uint32_t color_op = (blend_control >> 5) & 0x7;
  const uint32_t color_dest = (blend_control >> 8) & 0x1F;
  const uint32_t alpha_src = (blend_control >> 16) & 0x1F;
  const uint32_t alpha_op = (blend_control >> 21) & 0x7;
  const uint32_t alpha_dest = (blend_control >> 24) & 0x1F;
  return color_src == 1 && color_dest == 0 && color_op == 0 && alpha_src == 1 &&
         alpha_dest == 0 && alpha_op == 0;
}

uint64_t BlendStateKey(uint32_t blend_control, uint8_t write_mask) {
  return (uint64_t(blend_control) << 8) | uint64_t(write_mask & 0x0F);
}

ID3D11BlendState* GetReplayBlendState(
    ID3D11Device* device, std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>>& cache,
    uint32_t blend_control, uint8_t write_mask) {
  const uint64_t key = BlendStateKey(blend_control, write_mask);
  auto existing = cache.find(key);
  if (existing != cache.end()) {
    return existing->second.Get();
  }

  D3D11_BLEND_DESC blend_desc = {};
  D3D11_RENDER_TARGET_BLEND_DESC& rt = blend_desc.RenderTarget[0];
  rt.BlendEnable = !IsIdentityBlend(blend_control);
  rt.SrcBlend = MapXenosBlendFactor(blend_control & 0x1F, false);
  rt.BlendOp = MapXenosBlendOp((blend_control >> 5) & 0x7);
  rt.DestBlend = MapXenosBlendFactor((blend_control >> 8) & 0x1F, false);
  rt.SrcBlendAlpha = MapXenosBlendFactor((blend_control >> 16) & 0x1F, true);
  rt.BlendOpAlpha = MapXenosBlendOp((blend_control >> 21) & 0x7);
  rt.DestBlendAlpha = MapXenosBlendFactor((blend_control >> 24) & 0x1F, true);
  rt.RenderTargetWriteMask = write_mask & 0x0F;

  ComPtr<ID3D11BlendState> state;
  HRESULT hr = device->CreateBlendState(&blend_desc, &state);
  if (FAILED(hr)) {
    REXLOG_WARN("SW2E native GPU replay failed to create blend state 0x{:08x}/mask 0x{:X}: "
                "0x{:08x}",
                blend_control, write_mask & 0x0F, static_cast<uint32_t>(hr));
    return nullptr;
  }

  ID3D11BlendState* state_ptr = state.Get();
  cache.emplace(key, std::move(state));
  return state_ptr;
}

struct ReplayD3D11Pipeline {
  uint32_t viewport_width = 0;
  uint32_t viewport_height = 0;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11VertexShader> projected_vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11PixelShader> projected_pixel_shader;
  ComPtr<ID3D11PixelShader> solid_pixel_shader;
  ComPtr<ID3D11InputLayout> input_layout;
  ComPtr<ID3D11Buffer> constant_buffer;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11RasterizerState> rasterizer_state;
  std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blend_state_cache;
  std::unordered_map<uint64_t, ComPtr<ID3D11ShaderResourceView>> texture_view_cache;

  bool Ensure(ID3D11Device* device, uint32_t width, uint32_t height) {
    if (!device) {
      return false;
    }

    if (!vertex_shader) {
      if (!CreateFixedPipelineObjects(device)) {
        return false;
      }
    }

    if (!constant_buffer || viewport_width != width || viewport_height != height) {
      if (!CreateViewportConstantBuffer(device, width, height)) {
        return false;
      }
    }

    return true;
  }

  ID3D11ShaderResourceView* GetTextureView(ID3D11Device* device, const ReplayDraw& draw) {
    if (!device || ReplayTextureBytes(draw.texture).empty()) {
      return nullptr;
    }

    const uint64_t key = TextureCacheKey(draw.texture);
    auto existing = texture_view_cache.find(key);
    if (existing != texture_view_cache.end()) {
      return existing->second.Get();
    }

    const std::vector<uint8_t> pc_texture_bytes = BuildPcBc3TextureBytes(draw.texture);
    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = draw.texture.width;
    texture_desc.Height = draw.texture.height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_BC3_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA texture_data = {};
    texture_data.pSysMem = pc_texture_bytes.data();
    texture_data.SysMemPitch = draw.texture.row_pitch_bytes;
    texture_data.SysMemSlicePitch = draw.texture.row_pitch_bytes * draw.texture.height_blocks;

    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&texture_desc, &texture_data, &texture);
    if (FAILED(hr)) {
      REXLOG_WARN("SW2E native GPU replay skipped frame {} draw {} texture upload: 0x{:08x}",
                  draw.frame, draw.draw, static_cast<uint32_t>(hr));
      return nullptr;
    }

    ComPtr<ID3D11ShaderResourceView> texture_view;
    hr = device->CreateShaderResourceView(texture.Get(), nullptr, &texture_view);
    if (FAILED(hr)) {
      REXLOG_WARN("SW2E native GPU replay skipped frame {} draw {} SRV creation: 0x{:08x}",
                  draw.frame, draw.draw, static_cast<uint32_t>(hr));
      return nullptr;
    }

    ID3D11ShaderResourceView* texture_view_ptr = texture_view.Get();
    texture_view_cache.emplace(key, std::move(texture_view));
    return texture_view_ptr;
  }

 private:
  struct ViewportParams {
    float viewport_size[2];
    float padding[2];
  };

  bool CreateFixedPipelineObjects(ID3D11Device* device) {
    ComPtr<ID3DBlob> vs_blob;
    ComPtr<ID3DBlob> projected_vs_blob;
    ComPtr<ID3DBlob> ps_blob;
    ComPtr<ID3DBlob> projected_ps_blob;
    ComPtr<ID3DBlob> solid_ps_blob;
    if (!CompileShader(kVertexShaderHlsl, "main", "vs_5_0", vs_blob) ||
        !CompileShader(kProjectedVertexShaderHlsl, "main", "vs_5_0", projected_vs_blob) ||
        !CompileShader(kPixelShaderHlsl, "main", "ps_5_0", ps_blob) ||
        !CompileShader(kProjectedPixelShaderHlsl, "main", "ps_5_0", projected_ps_blob) ||
        !CompileShader(kSolidPixelShaderHlsl, "main", "ps_5_0", solid_ps_blob)) {
      return false;
    }

    HRESULT hr = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                            vs_blob->GetBufferSize(), nullptr, &vertex_shader);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create vertex shader: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    hr = device->CreateVertexShader(projected_vs_blob->GetBufferPointer(),
                                    projected_vs_blob->GetBufferSize(), nullptr,
                                    &projected_vertex_shader);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create projected vertex shader: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    hr = device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr,
                                   &pixel_shader);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create pixel shader: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    hr = device->CreatePixelShader(projected_ps_blob->GetBufferPointer(),
                                   projected_ps_blob->GetBufferSize(), nullptr,
                                   &projected_pixel_shader);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create projected pixel shader: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    hr = device->CreatePixelShader(solid_ps_blob->GetBufferPointer(),
                                   solid_ps_blob->GetBufferSize(), nullptr, &solid_pixel_shader);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create solid pixel shader: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    std::array<D3D11_INPUT_ELEMENT_DESC, 3> input_elements = {{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    }};
    hr = device->CreateInputLayout(input_elements.data(), static_cast<UINT>(input_elements.size()),
                                   vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                   &input_layout);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create input layout: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device->CreateSamplerState(&sampler_desc, &sampler);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create sampler: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    D3D11_RASTERIZER_DESC rasterizer_desc = {};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;
    hr = device->CreateRasterizerState(&rasterizer_desc, &rasterizer_state);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create rasterizer state: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    return true;
  }

  bool CreateViewportConstantBuffer(ID3D11Device* device, uint32_t width, uint32_t height) {
    ViewportParams viewport_params = {{static_cast<float>(width), static_cast<float>(height)},
                                      {0.0f, 0.0f}};

    D3D11_BUFFER_DESC constant_desc = {};
    constant_desc.ByteWidth = sizeof(ViewportParams);
    constant_desc.Usage = D3D11_USAGE_IMMUTABLE;
    constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constant_data = {};
    constant_data.pSysMem = &viewport_params;

    ComPtr<ID3D11Buffer> new_constant_buffer;
    HRESULT hr = device->CreateBuffer(&constant_desc, &constant_data, &new_constant_buffer);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU replay failed to create constant buffer: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    constant_buffer = std::move(new_constant_buffer);
    viewport_width = width;
    viewport_height = height;
    return true;
  }
};

uint32_t DrawReplayDrawsD3D11(ID3D11Device* device, ID3D11DeviceContext* context,
                              ID3D11RenderTargetView* render_target,
                              ReplayD3D11Pipeline& pipeline,
                              const std::vector<ReplayDraw>& draws, uint32_t width,
                              uint32_t height) {
  if (!pipeline.Ensure(device, width, height)) {
    return 0;
  }

  const float clear_color[] = {0.0f, 0.0f, 0.0f, 1.0f};
  context->ClearRenderTargetView(render_target, clear_color);
  context->OMSetRenderTargets(1, &render_target, nullptr);
  context->RSSetState(pipeline.rasterizer_state.Get());
  D3D11_VIEWPORT viewport = {};
  viewport.Width = static_cast<float>(width);
  viewport.Height = static_cast<float>(height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->IASetInputLayout(pipeline.input_layout.Get());
  context->VSSetShader(pipeline.vertex_shader.Get(), nullptr, 0);
  context->VSSetConstantBuffers(0, 1, pipeline.constant_buffer.GetAddressOf());
  context->PSSetShader(pipeline.pixel_shader.Get(), nullptr, 0);
  context->PSSetSamplers(0, 1, pipeline.sampler.GetAddressOf());

  uint32_t drawn = 0;
  HRESULT hr = S_OK;
  for (const ReplayDraw& draw : draws) {
    if (draw.vertices.empty()) {
      continue;
    }

    ID3D11BlendState* blend_state =
        GetReplayBlendState(device, pipeline.blend_state_cache, draw.rt0_blendcontrol,
                            draw.rt0_write_mask);
    if (!blend_state) {
      continue;
    }
    context->OMSetBlendState(blend_state, nullptr, 0xFFFFFFFF);

    ID3D11ShaderResourceView* texture_view = nullptr;
    const bool textured_draw = draw.kind == ReplayDrawKind::kTexturedTriangles ||
                               draw.kind == ReplayDrawKind::kProjectedTexturedTriangles;
    if (textured_draw) {
      texture_view = pipeline.GetTextureView(device, draw);
      if (!texture_view) {
        continue;
      }
    }

    D3D11_BUFFER_DESC vertex_desc = {};
    vertex_desc.ByteWidth = static_cast<UINT>(draw.vertices.size() * sizeof(ReplayVertex));
    vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertex_data = {};
    vertex_data.pSysMem = draw.vertices.data();
    ComPtr<ID3D11Buffer> vertex_buffer;
    hr = device->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer);
    if (FAILED(hr)) {
      REXLOG_WARN("SW2E native GPU replay skipped frame {} draw {} vertex upload: 0x{:08x}",
                  draw.frame, draw.draw, static_cast<uint32_t>(hr));
      continue;
    }

    ComPtr<ID3D11Buffer> index_buffer;
    if (!draw.indices.empty()) {
      D3D11_BUFFER_DESC index_desc = {};
      index_desc.ByteWidth = static_cast<UINT>(draw.indices.size() * sizeof(uint32_t));
      index_desc.Usage = D3D11_USAGE_IMMUTABLE;
      index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
      D3D11_SUBRESOURCE_DATA index_data = {};
      index_data.pSysMem = draw.indices.data();
      hr = device->CreateBuffer(&index_desc, &index_data, &index_buffer);
      if (FAILED(hr)) {
        REXLOG_WARN("SW2E native GPU replay skipped frame {} draw {} index upload: 0x{:08x}",
                    draw.frame, draw.draw, static_cast<uint32_t>(hr));
        continue;
      }
    }

    const UINT stride = sizeof(ReplayVertex);
    const UINT offset = 0;
    if (draw.kind == ReplayDrawKind::kProjectedTexturedTriangles) {
      context->VSSetShader(pipeline.projected_vertex_shader.Get(), nullptr, 0);
    } else {
      context->VSSetShader(pipeline.vertex_shader.Get(), nullptr, 0);
    }
    if (textured_draw) {
      context->PSSetShader(draw.kind == ReplayDrawKind::kProjectedTexturedTriangles
                               ? pipeline.projected_pixel_shader.Get()
                               : pipeline.pixel_shader.Get(),
                           nullptr, 0);
      context->PSSetShaderResources(0, 1, &texture_view);
    } else {
      ID3D11ShaderResourceView* null_view = nullptr;
      context->PSSetShader(pipeline.solid_pixel_shader.Get(), nullptr, 0);
      context->PSSetShaderResources(0, 1, &null_view);
    }
    context->IASetVertexBuffers(0, 1, vertex_buffer.GetAddressOf(), &stride, &offset);
    if (index_buffer) {
      context->IASetIndexBuffer(index_buffer.Get(), DXGI_FORMAT_R32_UINT, 0);
      context->DrawIndexed(static_cast<UINT>(draw.indices.size()), 0, 0);
    } else {
      context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
      context->Draw(static_cast<UINT>(draw.vertices.size()), 0);
    }
    ++drawn;
  }

  return drawn;
}

bool RenderMenuReplayD3D11Win32(const std::vector<ReplayDraw>& draws,
                                const std::filesystem::path& output_path, uint32_t width,
                                uint32_t height) {
  if (draws.empty()) {
    REXLOG_WARN("SW2E native GPU replay skipped because no draws were captured");
    return false;
  }

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &feature_level, 1,
                                 D3D11_SDK_VERSION, &device, nullptr, &context);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &feature_level, 1,
                           D3D11_SDK_VERSION, &device, nullptr, &context);
  }
  if (FAILED(hr)) {
    REXLOG_ERROR("SW2E native GPU replay failed to create a D3D11 device: 0x{:08x}",
                 static_cast<uint32_t>(hr));
    return false;
  }

  D3D11_TEXTURE2D_DESC render_desc = {};
  render_desc.Width = width;
  render_desc.Height = height;
  render_desc.MipLevels = 1;
  render_desc.ArraySize = 1;
  render_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  render_desc.SampleDesc.Count = 1;
  render_desc.Usage = D3D11_USAGE_DEFAULT;
  render_desc.BindFlags = D3D11_BIND_RENDER_TARGET;

  ComPtr<ID3D11Texture2D> render_texture;
  hr = device->CreateTexture2D(&render_desc, nullptr, &render_texture);
  if (FAILED(hr)) {
    REXLOG_ERROR("SW2E native GPU replay failed to create render texture: 0x{:08x}",
                 static_cast<uint32_t>(hr));
    return false;
  }

  ComPtr<ID3D11RenderTargetView> render_target;
  hr = device->CreateRenderTargetView(render_texture.Get(), nullptr, &render_target);
  if (FAILED(hr)) {
    REXLOG_ERROR("SW2E native GPU replay failed to create render target: 0x{:08x}",
                 static_cast<uint32_t>(hr));
    return false;
  }

  ReplayD3D11Pipeline pipeline;
  const uint32_t drawn = DrawReplayDrawsD3D11(device.Get(), context.Get(), render_target.Get(),
                                             pipeline, draws, width, height);

  if (drawn == 0) {
    REXLOG_WARN("SW2E native GPU replay produced no draw calls");
    return false;
  }

  D3D11_TEXTURE2D_DESC staging_desc = render_desc;
  staging_desc.Usage = D3D11_USAGE_STAGING;
  staging_desc.BindFlags = 0;
  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging_texture;
  hr = device->CreateTexture2D(&staging_desc, nullptr, &staging_texture);
  if (FAILED(hr)) {
    REXLOG_ERROR("SW2E native GPU replay failed to create staging texture: 0x{:08x}",
                 static_cast<uint32_t>(hr));
    return false;
  }

  context->CopyResource(staging_texture.Get(), render_texture.Get());
  context->Flush();

  D3D11_MAPPED_SUBRESOURCE mapped = {};
  hr = context->Map(staging_texture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr)) {
    REXLOG_ERROR("SW2E native GPU replay failed to map staging texture: 0x{:08x}",
                 static_cast<uint32_t>(hr));
    return false;
  }

  const bool wrote =
      WriteBmpTopDownBgra(output_path, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch,
                          width, height);
  context->Unmap(staging_texture.Get(), 0);
  if (!wrote) {
    return false;
  }

  REXLOG_INFO("SW2E native GPU replay wrote {} using {} captured D3D11 draws",
              output_path.string(), drawn);
  return true;
}

LRESULT CALLBACK NativeReplayChildWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

struct LivePreviewState {
  HWND hwnd = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swap_chain;
  ComPtr<ID3D11RenderTargetView> render_target;
  ReplayD3D11Pipeline pipeline;

  bool Initialize(HWND parent_hwnd, uint32_t requested_width, uint32_t requested_height) {
    if (requested_width == 0 || requested_height == 0) {
      REXLOG_WARN("SW2E native GPU live replay skipped child initialization for empty size {}x{}",
                  requested_width, requested_height);
      return false;
    }

    if (hwnd && width == requested_width && height == requested_height) {
      return true;
    }

    if (hwnd) {
      return Resize(parent_hwnd, requested_width, requested_height);
    }

    width = requested_width;
    height = requested_height;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t kClassName[] = L"SW2ENativeReplayChildWindow";
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = NativeReplayChildWndProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = kClassName;
    RegisterClassExW(&window_class);

    hwnd = CreateWindowExW(0, kClassName, L"SW2E Native D3D11 Replay",
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
                           static_cast<int>(width), static_cast<int>(height), parent_hwnd, nullptr,
                           instance, nullptr);
    if (!hwnd) {
      REXLOG_ERROR("SW2E native GPU live replay failed to create child window");
      return false;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, static_cast<int>(width), static_cast<int>(height),
                 SWP_SHOWWINDOW);

    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags,
                                   &feature_level, 1, D3D11_SDK_VERSION, &device, nullptr,
                                   &context);
    if (FAILED(hr)) {
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, device_flags, &feature_level,
                             1, D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to create D3D11 device: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = device.As(&dxgi_device);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to query IDXGIDevice: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_device->GetAdapter(&adapter);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to query IDXGIAdapter: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    ComPtr<IDXGIFactory> factory;
    hr = adapter->GetParent(__uuidof(IDXGIFactory), &factory);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to query IDXGIFactory: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    DXGI_SWAP_CHAIN_DESC swap_desc = {};
    swap_desc.BufferDesc.Width = width;
    swap_desc.BufferDesc.Height = height;
    swap_desc.BufferDesc.RefreshRate.Numerator = 60;
    swap_desc.BufferDesc.RefreshRate.Denominator = 1;
    swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swap_desc.SampleDesc.Count = 1;
    swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = 2;
    swap_desc.OutputWindow = hwnd;
    swap_desc.Windowed = TRUE;
    swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = factory->CreateSwapChain(device.Get(), &swap_desc, &swap_chain);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to create swap chain: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    return RecreateBackBufferRenderTarget();
  }

  bool Present(const std::vector<ReplayDraw>& draws) {
    if (!device || !context || !swap_chain || !render_target) {
      return false;
    }

    const uint32_t drawn = DrawReplayDrawsD3D11(device.Get(), context.Get(), render_target.Get(),
                                                pipeline, draws, width, height);
    if (drawn == 0) {
      REXLOG_WARN("SW2E native GPU live replay produced no draw calls");
      return false;
    }

    HRESULT hr = swap_chain->Present(1, 0);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay present failed: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    REXLOG_INFO("SW2E native GPU live replay presented {} captured D3D11 draws", drawn);
    return true;
  }

  bool Resize(HWND parent_hwnd, uint32_t requested_width, uint32_t requested_height) {
    if (!device || !context || !swap_chain || !hwnd) {
      return false;
    }
    if (requested_width == 0 || requested_height == 0) {
      REXLOG_WARN("SW2E native GPU live replay skipped resize for empty size {}x{}",
                  requested_width, requested_height);
      return false;
    }

    if (parent_hwnd && GetParent(hwnd) != parent_hwnd) {
      SetParent(hwnd, parent_hwnd);
    }

    context->OMSetRenderTargets(0, nullptr, nullptr);
    render_target.Reset();

    SetWindowPos(hwnd, HWND_TOP, 0, 0, static_cast<int>(requested_width),
                 static_cast<int>(requested_height),
                 SWP_SHOWWINDOW);

    HRESULT hr =
        swap_chain->ResizeBuffers(0, requested_width, requested_height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to resize swap chain to {}x{}: 0x{:08x}",
                   requested_width, requested_height, static_cast<uint32_t>(hr));
      return false;
    }

    width = requested_width;
    height = requested_height;
    return RecreateBackBufferRenderTarget();
  }

  bool RecreateBackBufferRenderTarget() {
    if (!device || !swap_chain) {
      return false;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    HRESULT hr = swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), &back_buffer);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to get back buffer: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }

    hr = device->CreateRenderTargetView(back_buffer.Get(), nullptr, &render_target);
    if (FAILED(hr)) {
      REXLOG_ERROR("SW2E native GPU live replay failed to create back-buffer RTV: 0x{:08x}",
                   static_cast<uint32_t>(hr));
      return false;
    }
    return true;
  }
};

LivePreviewState g_live_preview;

bool InitializeMenuReplayD3D11ChildWin32(void* parent_window_handle, uint32_t width,
                                         uint32_t height) {
  HWND parent_hwnd = static_cast<HWND>(parent_window_handle);
  if (!parent_hwnd) {
    REXLOG_WARN("SW2E native GPU live replay skipped child initialization because parent HWND is null");
    return false;
  }

  return g_live_preview.Initialize(parent_hwnd, width, height);
}

bool PresentMenuReplayD3D11ChildWin32(void* parent_window_handle,
                                      const std::vector<ReplayDraw>& draws, uint32_t width,
                                      uint32_t height) {
  if (draws.empty()) {
    REXLOG_WARN("SW2E native GPU live replay skipped because no draws were captured");
    return false;
  }

  if (!g_live_preview.hwnd &&
      !InitializeMenuReplayD3D11ChildWin32(parent_window_handle, width, height)) {
    return false;
  }
  return g_live_preview.Present(draws);
}

#endif  // REX_PLATFORM_WIN32

}  // namespace

bool RenderMenuReplayD3D11(const std::vector<ReplayDraw>& draws,
                           const std::filesystem::path& output_path, uint32_t width,
                           uint32_t height) {
#if REX_PLATFORM_WIN32
  return RenderMenuReplayD3D11Win32(draws, output_path, width, height);
#else
  (void)draws;
  (void)output_path;
  (void)width;
  (void)height;
  REXLOG_WARN("SW2E native GPU replay is only implemented on Windows/D3D11");
  return false;
#endif
}

bool InitializeMenuReplayD3D11Child(void* parent_window_handle, uint32_t width, uint32_t height) {
#if REX_PLATFORM_WIN32
  return InitializeMenuReplayD3D11ChildWin32(parent_window_handle, width, height);
#else
  (void)parent_window_handle;
  (void)width;
  (void)height;
  REXLOG_WARN("SW2E native GPU live replay is only implemented on Windows/D3D11");
  return false;
#endif
}

bool PresentMenuReplayD3D11Child(void* parent_window_handle, const std::vector<ReplayDraw>& draws,
                                 uint32_t width, uint32_t height) {
#if REX_PLATFORM_WIN32
  return PresentMenuReplayD3D11ChildWin32(parent_window_handle, draws, width, height);
#else
  (void)parent_window_handle;
  (void)draws;
  (void)width;
  (void)height;
  REXLOG_WARN("SW2E native GPU live replay is only implemented on Windows/D3D11");
  return false;
#endif
}

}  // namespace sw2e::native_renderer::gpu_replay
