#include "generated/default/samurai_warriors_2_empires_init.h"

#include "recomp/SourceFunctionMap.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/system/xio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(sw2e_auto_boot_input, false, "SM2/Input",
                    "Synthesize Start/A during startup for unattended smoke tests");
REXCVAR_DEFINE_BOOL(sw2e_auto_probe_input, false, "SM2/Input",
                    "Synthesize bounded Start/A menu input after startup for renderer probes");

namespace rex::kernel::xam {
u32 XamInputGetState_entry(u32 user_index, u32 flags,
                           ppc_ptr_t<rex::input::X_INPUT_STATE> input_state);
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke);
u32 XamContentCreateEx_entry(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                             u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                             u32 cache_size, u64 content_size, mapped_void overlapped_ptr);
u32 XamContentClose_entry(mapped_string root_name, mapped_void overlapped_ptr);
u32 XamContentFlush_entry(mapped_string root_name, mapped_void overlapped_ptr);
u32 XamContentSetThumbnail_entry(u32 user_index, mapped_void content_data_ptr,
                                 mapped_void buffer_ptr, u32 buffer_size,
                                 mapped_void overlapped_ptr);
}

namespace rex::kernel::xboxkrnl {
u32 NtClose_entry(u32 handle);
u32 NtCreateFile_entry(mapped_u32 handle_out, u32 desired_access,
                       ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> object_attrs,
                       ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                       mapped_u64 allocation_size_ptr, u32 file_attributes, u32 share_access,
                       u32 creation_disposition, u32 create_options);
u32 NtOpenFile_entry(mapped_u32 handle_out, u32 desired_access,
                     ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> object_attributes,
                     ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block, u32 open_options);
u32 NtReadFile_entry(u32 file_handle, u32 event_handle, mapped_void apc_routine_ptr,
                     mapped_void apc_context,
                     ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                     mapped_void buffer, u32 buffer_length, mapped_u64 byte_offset_ptr);
u32 NtWriteFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, mapped_void apc_context,
                      ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block,
                      mapped_void buffer, u32 buffer_length, mapped_u64 byte_offset_ptr);
u32 NtFlushBuffersFile_entry(
    u32 file_handle, ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> io_status_block_ptr);
u32 NtWaitForSingleObjectEx_entry(u32 object_handle, u32 wait_mode, u32 alertable,
                                  mapped_u64 timeout_ptr);
}

namespace sw2e::hooks {

void LogHookHitOnce(std::atomic_bool& seen, uint32_t address, const char* symbol) {
  if (seen.exchange(true)) {
    return;
  }

  if (const FunctionInfo* info = FindKnownFunction(address)) {
    if (!info->source_path.empty()) {
      REXLOG_INFO("SM2 hook active: {} @ {:#010x} [{}] - {} ({})", info->symbol, info->address,
                  ConfidenceName(info->confidence), info->label, info->source_path);
    } else {
      REXLOG_INFO("SM2 hook active: {} @ {:#010x} [{}] - {}", info->symbol, info->address,
                  ConfidenceName(info->confidence), info->label);
    }
    return;
  }

  REXLOG_INFO("SM2 hook active: {} @ {:#010x}", symbol, address);
}

void LogHookCall(std::atomic_uint32_t& hits, uint32_t address, const char* symbol, PPCContext& ctx) {
  const uint32_t hit = hits.fetch_add(1, std::memory_order_relaxed);

  if (hit < 16) {
    REXLOG_INFO(
        "SM2 hook call #{}: {} @ {:#010x} lr={:#010x} r3={:#010x} r4={:#010x} r5={:#010x} "
        "r6={:#010x} r7={:#010x} r8={:#010x}",
        hit + 1, symbol, address, ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        ctx.r7.u32, ctx.r8.u32);
  } else if (hit == 16) {
    REXLOG_INFO("SM2 hook call logging muted after 16 hits: {} @ {:#010x}", symbol, address);
  } else if ((hit % 1024) == 0) {
    REXLOG_INFO(
        "SM2 hook heartbeat #{}: {} @ {:#010x} lr={:#010x} r3={:#010x} r4={:#010x}",
        hit + 1, symbol, address, ctx.lr, ctx.r3.u32, ctx.r4.u32);
  }
}

constexpr uint32_t kXErrorSuccess = 0x00000000;
constexpr uint32_t kXErrorEmpty = 0x000010D2;
constexpr uint32_t kXInputFlagAnyUser = 1u << 30;
constexpr uint16_t kXInputGamepadStart = 0x0010;
constexpr uint16_t kXInputGamepadA = 0x1000;
constexpr uint16_t kXInputKeystrokeKeyDown = 0x0001;
constexpr uint16_t kXInputKeystrokeKeyUp = 0x0002;
constexpr uint16_t kVkPadA = 0x5800;
constexpr uint16_t kVkPadStart = 0x5814;
constexpr uint32_t kStorageDeviceIdAddress = 0x82501270;
constexpr uint32_t kDummyHddDeviceId = 0x00000001;

std::atomic_uint32_t g_xam_input_get_state_calls{};
std::atomic_uint32_t g_user0_xam_input_get_state_calls{};
std::atomic_uint32_t g_xam_input_get_keystroke_ex_calls{};
std::atomic_uint32_t g_boot_keystroke_calls{};
std::atomic_uint32_t g_xam_content_create_ex_calls{};
std::atomic_uint32_t g_xam_content_close_calls{};
std::atomic_uint32_t g_xam_content_flush_calls{};
std::atomic_uint32_t g_xam_content_set_thumbnail_calls{};
std::atomic_uint32_t g_nt_close_calls{};
std::atomic_uint32_t g_nt_create_file_calls{};
std::atomic_uint32_t g_nt_open_file_calls{};
std::atomic_uint32_t g_nt_read_file_calls{};
std::atomic_uint32_t g_nt_write_file_calls{};
std::atomic_uint32_t g_nt_flush_buffers_file_calls{};
std::atomic_uint32_t g_nt_wait_for_single_object_ex_calls{};
std::atomic_uint32_t g_archive_read_entry_calls{};
std::atomic_uint32_t g_asset_load_wrapper_calls{};
std::atomic_uint32_t g_dbg_print_calls{};
std::atomic_uint32_t g_debug_command_parser_calls{};
std::atomic_uint32_t g_guest_event_clear_calls{};
std::atomic_uint32_t g_render_packet_page_helper_calls{};
std::atomic_uint32_t g_render_packet_block_helper_calls{};
std::atomic_uint32_t g_transient_draw_packet_skips{};
std::atomic_bool g_storage_device_seeded{};
std::atomic_uint32_t g_boot_input_stage{};
std::atomic_uint32_t g_post_storage_input_calls{};
std::atomic_bool g_post_storage_input_done_logged{};
std::atomic_uint32_t g_probe_input_calls{};

bool ShouldLogImportCall(std::atomic_uint32_t& hits) {
  const uint32_t hit = hits.fetch_add(1, std::memory_order_relaxed);
  return hit < 64 || (hit % 1024) == 0;
}

uint32_t ArchiveSelector(uint32_t packed_id) {
  if ((packed_id & 0x40000000) != 0) {
    return 2;
  }
  return (packed_id >> 28) & 1;
}

uint32_t ArchiveEntryIndex(uint32_t packed_id) {
  return packed_id & 0x03FFFFFF;
}

uint32_t PackedArchiveIdFromAssetRequest(uint32_t source, uint32_t entry_or_packed_id) {
  const uint32_t entry_index = ArchiveEntryIndex(entry_or_packed_id);
  if (source == 0) {
    return entry_index | 0x40000000;
  }
  if (source == 1) {
    return entry_index;
  }
  if (source == 2) {
    return entry_index | 0x10000000;
  }
  return entry_or_packed_id;
}

bool ShouldLogArchiveEntry(uint32_t call_index, uint32_t entry_index) {
  if (call_index < 128 || (call_index % 2048) == 0) {
    return true;
  }

  if (entry_index >= 1566 && entry_index <= 1945) {
    return true;
  }

  return entry_index >= 2856 && entry_index <= 2990;
}

bool ShouldLogAssetLoad(uint32_t call_index, uint32_t entry_index, uint32_t lr) {
  if (call_index < 96 || (call_index % 2048) == 0) {
    return true;
  }

  if (entry_index >= 146 && entry_index <= 351) {
    return true;
  }
  if (entry_index >= 1566 && entry_index <= 1945) {
    return true;
  }
  if (entry_index >= 2062 && entry_index <= 2411) {
    return true;
  }
  if (entry_index >= 2505 && entry_index <= 2681) {
    return true;
  }
  if (entry_index >= 2856 && entry_index <= 3124) {
    return true;
  }
  if (entry_index >= 3506 && entry_index <= 3547) {
    return true;
  }

  return lr >= 0x82286B30 && lr <= 0x82286F40;
}

bool GuestRangeIsCommitted(u8* base, uint32_t addr, size_t size, bool require_writable);
bool GuestWriteRangeIsCommitted(u8* base, uint32_t addr, size_t size);

std::string GuestCStringPreview(u8* base, uint32_t addr, size_t max_length = 260) {
  if (addr == 0) {
    return {};
  }

  std::string value;
  value.reserve(std::min<size_t>(max_length, 64));
  for (size_t i = 0; i < max_length; ++i) {
    const uint32_t current = addr + static_cast<uint32_t>(i);
    if (current < addr || !GuestRangeIsCommitted(base, current, 1, false)) {
      if (value.empty()) {
        return "<unmapped>";
      }
      value += "<truncated>";
      break;
    }

    const char ch = static_cast<char>(REX_LOAD_U8(current));
    if (ch == '\0') {
      break;
    }
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      value.push_back(' ');
    } else if (static_cast<unsigned char>(ch) < 32) {
      value.push_back('.');
    } else {
      value.push_back(ch);
    }
  }
  return value;
}

std::string GuestMagicPreview(u8* base, uint32_t addr) {
  if (!GuestWriteRangeIsCommitted(base, addr, 4)) {
    return "....";
  }

  char magic[5]{};
  for (uint32_t i = 0; i < 4; ++i) {
    const uint8_t value = REX_LOAD_U8(addr + i);
    magic[i] = value >= 32 && value <= 126 ? static_cast<char>(value) : '.';
  }
  return std::string(magic, 4);
}

mapped_void GuestVoid(u8* base, uint32_t addr) {
  if (addr == 0) {
    return mapped_void(nullptr);
  }
  return mapped_void(REX_RAW_ADDR(addr), addr);
}

mapped_string GuestString(u8* base, uint32_t addr) {
  if (addr == 0) {
    return mapped_string(nullptr);
  }
  return mapped_string(reinterpret_cast<char*>(REX_RAW_ADDR(addr)), addr);
}

mapped_u32 GuestU32(u8* base, uint32_t addr) {
  if (addr == 0) {
    return mapped_u32(nullptr);
  }
  return mapped_u32(reinterpret_cast<rex::be_u32*>(REX_RAW_ADDR(addr)), addr);
}

mapped_u64 GuestU64(u8* base, uint32_t addr) {
  if (addr == 0) {
    return mapped_u64(nullptr);
  }
  return mapped_u64(reinterpret_cast<rex::be_u64*>(REX_RAW_ADDR(addr)), addr);
}

ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK> GuestIoStatusBlock(u8* base, uint32_t addr) {
  if (addr == 0) {
    return ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK>(nullptr);
  }
  return ppc_ptr_t<rex::system::X_IO_STATUS_BLOCK>(
      reinterpret_cast<rex::system::X_IO_STATUS_BLOCK*>(REX_RAW_ADDR(addr)), addr);
}

ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> GuestKeystroke(u8* base, uint32_t addr) {
  if (addr == 0) {
    return ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE>(nullptr);
  }
  return ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE>(
      reinterpret_cast<rex::input::X_INPUT_KEYSTROKE*>(REX_RAW_ADDR(addr)), addr);
}

ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES> GuestObjectAttributes(u8* base, uint32_t addr) {
  if (addr == 0) {
    return ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES>(nullptr);
  }
  return ppc_ptr_t<rex::system::X_OBJECT_ATTRIBUTES>(
      reinterpret_cast<rex::system::X_OBJECT_ATTRIBUTES*>(REX_RAW_ADDR(addr)), addr);
}

std::string GuestAnsiStringValue(u8* base, uint32_t ansi_string_addr) {
  if (ansi_string_addr == 0) {
    return {};
  }

  const auto* ansi =
      reinterpret_cast<const rex::system::X_ANSI_STRING*>(REX_RAW_ADDR(ansi_string_addr));
  const uint32_t buffer_addr = ansi->pointer;
  const uint16_t length = ansi->length;
  if (buffer_addr == 0 || length == 0) {
    return {};
  }

  const size_t safe_length = std::min<size_t>(length, 1024);
  const auto* buffer = reinterpret_cast<const char*>(REX_RAW_ADDR(buffer_addr));
  return std::string(buffer, safe_length);
}

std::string GuestObjectPath(u8* base, uint32_t object_attrs_addr) {
  if (object_attrs_addr == 0) {
    return {};
  }

  const auto* object_attrs =
      reinterpret_cast<const rex::system::X_OBJECT_ATTRIBUTES*>(REX_RAW_ADDR(object_attrs_addr));
  return GuestAnsiStringValue(base, object_attrs->name_ptr);
}

uint32_t GuestObjectRootDirectory(u8* base, uint32_t object_attrs_addr) {
  if (object_attrs_addr == 0) {
    return 0;
  }

  const auto* object_attrs =
      reinterpret_cast<const rex::system::X_OBJECT_ATTRIBUTES*>(REX_RAW_ADDR(object_attrs_addr));
  return object_attrs->root_directory;
}

bool IsStashedGuestEvent(u8* base, uint32_t addr) {
  if (addr == 0) {
    return false;
  }

  const uint8_t type = REX_LOAD_U8(addr);
  if (type > 1) {
    return false;
  }

  constexpr uint32_t kRexObjectSignature = 0x52455800;  // "REX\0"
  return REX_LOAD_U32(addr + 8) == kRexObjectSignature;
}

bool GuestRangeIsCommitted(u8* base, uint32_t addr, size_t size, bool require_writable) {
  if (size == 0) {
    return true;
  }
  if (addr == 0 || size > UINT32_MAX) {
    return false;
  }

  const uint32_t last_addr = addr + static_cast<uint32_t>(size - 1);
  if (last_addr < addr) {
    return false;
  }

#if REX_PLATFORM_WIN32
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(REX_RAW_ADDR(addr), &info, sizeof(info)) == 0) {
    return false;
  }
  if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    return false;
  }

  if (require_writable) {
    constexpr DWORD kWritable =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & kWritable) == 0) {
      return false;
    }
  }

  const auto region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
  const auto request_end = reinterpret_cast<uintptr_t>(REX_RAW_ADDR(last_addr)) + 1;
  return request_end <= region_end;
#else
  return true;
#endif
}

bool GuestWriteRangeIsCommitted(u8* base, uint32_t addr, size_t size) {
  return GuestRangeIsCommitted(base, addr, size, true);
}

uint32_t GuestU32OrZero(u8* base, uint32_t addr) {
  return GuestRangeIsCommitted(base, addr, sizeof(uint32_t), false) ? REX_LOAD_U32(addr) : 0;
}

uint8_t GuestU8OrZero(u8* base, uint32_t addr) {
  return GuestRangeIsCommitted(base, addr, sizeof(uint8_t), false) ? REX_LOAD_U8(addr) : 0;
}

bool ShouldLogRenderPacketHook(uint32_t call_index) {
  return call_index < 24 || (call_index % 2048) == 0;
}

void LogRenderPacketState(u8* base, uint32_t call_index, const char* symbol, uint32_t address,
                          const char* phase, const PPCContext& ctx, uint32_t state_addr,
                          uint32_t output_addr, uint32_t extra_addr) {
  if (!ShouldLogRenderPacketHook(call_index)) {
    return;
  }

  const uint32_t cursor = GuestU32OrZero(base, state_addr + 48);
  const uint32_t cursor_limit = GuestU32OrZero(base, state_addr + 56);
  const uint32_t command_base = GuestU32OrZero(base, state_addr + 10768);
  const uint32_t command_cursor = GuestU32OrZero(base, state_addr + 10780);
  const uint32_t flags0 = GuestU8OrZero(base, state_addr + 10812);
  const uint32_t flags1 = GuestU8OrZero(base, state_addr + 10813);
  const uint32_t flags2 = GuestU8OrZero(base, state_addr + 10814);
  const uint32_t pending_words = GuestU32OrZero(base, state_addr + 12944);
  const uint32_t rt0_info = GuestU32OrZero(base, state_addr + 14528);
  const uint32_t rt1_info = GuestU32OrZero(base, state_addr + 14532);
  const uint32_t depth_base = GuestU32OrZero(base, state_addr + 14540);
  const uint32_t color_info = GuestU32OrZero(base, state_addr + 14544);

  REXLOG_INFO(
      "SM2 render packet {} #{} {} @ {:#010x}: lr={:#010x} state={:#010x} "
      "out={:#010x} extra={:#010x} cursor={:#010x}/{:#010x} cmd={:#010x}->{:#010x} "
      "flags={:#04x}/{:#04x}/{:#04x} pending={} rt={:#010x}/{:#010x} "
      "depth_base={:#010x} color={:#010x} r3={:#010x} r4={:#010x} r5={:#010x}",
      symbol, call_index + 1, phase, address, ctx.lr, state_addr, output_addr, extra_addr,
      cursor, cursor_limit, command_base, command_cursor, flags0, flags1, flags2,
      pending_words, rt0_info, rt1_info, depth_base, color_info, ctx.r3.u32, ctx.r4.u32,
      ctx.r5.u32);
}

void LogRenderPacketPageResult(u8* base, uint32_t call_index, uint32_t packet_out_addr,
                               uint32_t words_out_addr) {
  if (!ShouldLogRenderPacketHook(call_index)) {
    return;
  }

  const uint32_t packet_addr = GuestU32OrZero(base, packet_out_addr);
  const uint32_t packet_words = GuestU32OrZero(base, words_out_addr);
  const uint32_t packet0 = GuestU32OrZero(base, packet_addr);
  const uint32_t packet1 = GuestU32OrZero(base, packet_addr + 4);
  const uint32_t packet2 = GuestU32OrZero(base, packet_addr + 8);
  const uint32_t packet3 = GuestU32OrZero(base, packet_addr + 12);
  REXLOG_INFO(
      "SM2 render packet page result #{}: packet={:#010x} words={} "
      "head={:#010x}/{:#010x}/{:#010x}/{:#010x}",
      call_index + 1, packet_addr, packet_words, packet0, packet1, packet2, packet3);
}

void LogRenderPacketBlockResult(u8* base, uint32_t call_index, uint32_t block_addr,
                                uint32_t result_addr) {
  if (!ShouldLogRenderPacketHook(call_index)) {
    return;
  }

  const uint32_t packet0 = GuestU32OrZero(base, block_addr);
  const uint32_t packet1 = GuestU32OrZero(base, block_addr + 4);
  const uint32_t packet2 = GuestU32OrZero(base, block_addr + 8);
  const uint32_t packet3 = GuestU32OrZero(base, block_addr + 12);
  const uint32_t packet4 = GuestU32OrZero(base, block_addr + 16);
  REXLOG_INFO(
      "SM2 render packet block result #{}: block={:#010x} result={:#010x} "
      "head={:#010x}/{:#010x}/{:#010x}/{:#010x}/{:#010x}",
      call_index + 1, block_addr, result_addr, packet0, packet1, packet2, packet3, packet4);
}

bool StoredDrawCursorWritableAt(u8* base, uint32_t state_addr, uint32_t cursor_offset,
                                size_t bytes) {
  if (state_addr == 0) {
    return false;
  }

  const uint32_t cursor = REX_LOAD_U32(state_addr + cursor_offset);
  const uint32_t write_addr = cursor + 4;
  if (GuestWriteRangeIsCommitted(base, write_addr, bytes)) {
    return true;
  }

#if REX_PLATFORM_WIN32
  // SW2E builds short-lived draw command lists in high guest stack pages. ReXGlue reserves the
  // address range, but some paths reach a page before the normal stack-growth handler commits it.
  // Commit only pages already inside that reservation, then execute the original game routine.
  MEMORY_BASIC_INFORMATION info{};
  void* host_addr = REX_RAW_ADDR(write_addr);
  if (VirtualQuery(host_addr, &info, sizeof(info)) != 0 && info.State == MEM_RESERVE) {
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const uintptr_t page_mask = uintptr_t(system_info.dwPageSize - 1);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(host_addr) & ~page_mask;
    const uintptr_t end = (reinterpret_cast<uintptr_t>(host_addr) + bytes + page_mask) & ~page_mask;
    if (end > begin && VirtualAlloc(reinterpret_cast<void*>(begin), end - begin, MEM_COMMIT,
                                    PAGE_READWRITE) != nullptr) {
      static std::atomic_uint32_t committed_pages{};
      const uint32_t count = committed_pages.fetch_add(1, std::memory_order_relaxed);
      if (count < 16) {
        REXLOG_INFO("SM2 transient draw guard committed guest command page at {:#010x} ({} bytes)",
                    write_addr, bytes);
      }
      return GuestWriteRangeIsCommitted(base, write_addr, bytes);
    }
  }
#endif

  return false;
}

bool StoredDrawCursorWritable(u8* base, uint32_t state_addr, size_t bytes) {
  return StoredDrawCursorWritableAt(base, state_addr, 48, bytes);
}

void LogTransientDrawSkip(u8* base, uint32_t state_addr, uint32_t cursor_offset,
                          const char* symbol) {
  const uint32_t hit = g_transient_draw_packet_skips.fetch_add(1, std::memory_order_relaxed);
  if (hit < 16) {
    const uint32_t cursor = state_addr != 0 ? REX_LOAD_U32(state_addr + cursor_offset) : 0;
    REXLOG_WARN("SM2 transient draw guard: skipped {} with non-writable cursor {:#010x}", symbol,
                cursor);
  } else if (hit == 16) {
    REXLOG_WARN("SM2 transient draw guard: skip logging muted after 16 hits");
  }
}

void EnsureStorageDeviceSelected(u8* base, const char* caller) {
  const uint32_t current_device = REX_LOAD_U32(kStorageDeviceIdAddress);
  if (current_device != 0xFFFFFFFF) {
    if (REXCVAR_GET(sw2e_auto_boot_input)) {
      uint32_t expected_stage = 0;
      if (g_boot_input_stage.compare_exchange_strong(expected_stage, 1)) {
        REXLOG_INFO(
            "SM2 storage bootstrap: selected storage device already {} before {}; "
            "switching boot input to prompt-confirm stage",
            current_device, caller);
      }
    } else if (!g_storage_device_seeded.exchange(true)) {
      REXLOG_INFO(
          "SM2 storage bootstrap: selected storage device already {} before {}; "
          "auto boot input disabled",
          current_device, caller);
    }
    return;
  }

  REX_STORE_U32(kStorageDeviceIdAddress, kDummyHddDeviceId);
  if (REXCVAR_GET(sw2e_auto_boot_input)) {
    uint32_t expected_stage = 0;
    g_boot_input_stage.compare_exchange_strong(expected_stage, 1);
  }
  if (!g_storage_device_seeded.exchange(true)) {
    REXLOG_INFO(
        "SM2 storage bootstrap: seeded selected storage device to dummy HDD {} at {:#010x} "
        "before {}; auto boot input {}",
        kDummyHddDeviceId, kStorageDeviceIdAddress, caller,
        REXCVAR_GET(sw2e_auto_boot_input) ? "enabled" : "disabled");
  }
}

uint16_t BootInputButtons(uint32_t call_index) {
  if (!REXCVAR_GET(sw2e_auto_boot_input)) {
    return 0;
  }

  const uint32_t stage = g_boot_input_stage.load(std::memory_order_relaxed);
  if (stage == 0) {
    if (call_index >= 900) {
      return 0;
    }

    const uint32_t phase = call_index % 90;
    if (phase >= 28 && phase < 40) {
      return kXInputGamepadStart;
    }
    if (call_index >= 150 && phase >= 58 && phase < 70) {
      return kXInputGamepadA;
    }
    return 0;
  }

  if (stage == 1) {
    const uint32_t post_storage_call =
        g_post_storage_input_calls.fetch_add(1, std::memory_order_relaxed);
    if (post_storage_call >= 720) {
      g_boot_input_stage.store(2, std::memory_order_relaxed);
      if (!g_post_storage_input_done_logged.exchange(true)) {
        REXLOG_INFO("SM2 synthetic boot input disabled after startup prompt-confirm window");
      }
      return 0;
    }

    const uint32_t phase = post_storage_call % 120;
    if (phase >= 18 && phase < 34) {
      return kXInputGamepadA;
    }
  }

  return 0;
}

uint16_t ProbeInputButtons() {
  if (!REXCVAR_GET(sw2e_auto_probe_input)) {
    return 0;
  }

  if (REXCVAR_GET(sw2e_auto_boot_input) &&
      g_boot_input_stage.load(std::memory_order_relaxed) != 2) {
    return 0;
  }

  const uint32_t call_index = g_probe_input_calls.fetch_add(1, std::memory_order_relaxed);
  if (call_index >= 5400) {
    return 0;
  }

  if (call_index < 360) {
    const uint32_t phase = call_index % 60;
    return phase >= 6 && phase < 18 ? kXInputGamepadStart : 0;
  }

  const uint32_t phase = call_index % 45;
  return phase >= 4 && phase < 14 ? kXInputGamepadA : 0;
}

bool BootKeystroke(uint32_t call_index, uint16_t& virtual_key, uint16_t& flags) {
  if (!REXCVAR_GET(sw2e_auto_boot_input)) {
    return false;
  }

  const uint32_t stage = g_boot_input_stage.load(std::memory_order_relaxed);
  if (stage == 0) {
    if (call_index >= 900) {
      return false;
    }

    const uint32_t phase = call_index % 90;
    if (phase == 28 || phase == 40) {
      virtual_key = kVkPadStart;
      flags = phase == 28 ? kXInputKeystrokeKeyDown : kXInputKeystrokeKeyUp;
      return true;
    }
    if (call_index >= 150 && (phase == 58 || phase == 70)) {
      virtual_key = kVkPadA;
      flags = phase == 58 ? kXInputKeystrokeKeyDown : kXInputKeystrokeKeyUp;
      return true;
    }
    return false;
  }

  if (stage == 1) {
    if (call_index >= 720) {
      return false;
    }

    const uint32_t phase = call_index % 45;
    if (phase == 0 || phase == 2) {
      virtual_key = kVkPadA;
      flags = phase == 0 ? kXInputKeystrokeKeyDown : kXInputKeystrokeKeyUp;
      return true;
    }
  }

  return false;
}

}  // namespace sw2e::hooks

#define SW2E_PASSTHROUGH_HOOK(symbol, address)        \
  extern "C" REX_FUNC(symbol) {                       \
    static std::atomic_bool seen{};                   \
    sw2e::hooks::LogHookHitOnce(seen, address, #symbol); \
    __imp__##symbol(ctx, base);                       \
  }

#define SW2E_LOGGING_PASSTHROUGH_HOOK(symbol, address)        \
  extern "C" REX_FUNC(symbol) {                               \
    static std::atomic_bool seen{};                           \
    static std::atomic_uint32_t hits{};                       \
    sw2e::hooks::LogHookHitOnce(seen, address, #symbol);      \
    sw2e::hooks::LogHookCall(hits, address, #symbol, ctx);    \
    __imp__##symbol(ctx, base);                               \
  }

extern "C" REX_FUNC(__imp__DbgPrint) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249EBE4, "__imp__DbgPrint");

  const uint32_t call_index =
      sw2e::hooks::g_dbg_print_calls.fetch_add(1, std::memory_order_relaxed);
  const bool log = call_index < 256 || (call_index % 2048) == 0;
  if (log) {
    REXLOG_INFO("SM2 guest DbgPrint #{}: lr={:#010x} text='{}'", call_index + 1, ctx.lr,
                sw2e::hooks::GuestCStringPreview(base, ctx.r3.u32));
  } else if (call_index == 256) {
    REXLOG_INFO("SM2 guest DbgPrint logging muted after 256 lines");
  }

  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(__imp__XamInputGetState) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249E854, "__imp__XamInputGetState");

  const uint32_t user_index = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t state_addr = ctx.r5.u32;

  ppc_ptr_t<rex::input::X_INPUT_STATE> input_state(nullptr);
  if (state_addr != 0) {
    input_state = ppc_ptr_t<rex::input::X_INPUT_STATE>(
        reinterpret_cast<rex::input::X_INPUT_STATE*>(REX_RAW_ADDR(state_addr)), state_addr);
  }

  uint32_t result = rex::kernel::xam::XamInputGetState_entry(user_index, flags, input_state);

  sw2e::hooks::g_xam_input_get_state_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t user0_call_index =
      user_index == 0 ? sw2e::hooks::g_user0_xam_input_get_state_calls.fetch_add(
                            1, std::memory_order_relaxed)
                      : 0;
  const uint16_t buttons =
      user_index == 0 && REXCVAR_GET(sw2e_auto_boot_input)
          ? sw2e::hooks::BootInputButtons(user0_call_index)
          : 0;
  const uint16_t probe_buttons =
      user_index == 0 ? sw2e::hooks::ProbeInputButtons() : 0;
  const uint16_t merged_synthetic_buttons = buttons | probe_buttons;

  if (merged_synthetic_buttons != 0 && state_addr != 0) {
    if (result != sw2e::hooks::kXErrorSuccess) {
      REX_STORE_U32(state_addr + 0, user0_call_index + 1);
      REX_STORE_U16(state_addr + 4, 0);
      REX_STORE_U8(state_addr + 6, 0);
      REX_STORE_U8(state_addr + 7, 0);
      REX_STORE_U16(state_addr + 8, 0);
      REX_STORE_U16(state_addr + 10, 0);
      REX_STORE_U16(state_addr + 12, 0);
      REX_STORE_U16(state_addr + 14, 0);
      result = sw2e::hooks::kXErrorSuccess;
    }

    REX_STORE_U32(state_addr + 0, user0_call_index + 1);
    const uint16_t merged_buttons = REX_LOAD_U16(state_addr + 4) | merged_synthetic_buttons;
    REX_STORE_U16(state_addr + 4, merged_buttons);
  }

  if (probe_buttons != 0) {
    const uint32_t probe_call = sw2e::hooks::g_probe_input_calls.load(std::memory_order_relaxed);
    const uint32_t phase = probe_call % 45;
    const bool should_log = probe_call < 420 || phase == 5;
    if (should_log) {
      REXLOG_INFO(
          "SM2 synthetic probe input pulse: XamInputGetState user={} user0-call #{} "
          "probe-call #{} buttons={:#06x}",
          user_index, user0_call_index + 1, probe_call, probe_buttons);
    }
  } else if (buttons != 0) {
    const uint32_t stage = sw2e::hooks::g_boot_input_stage.load(std::memory_order_relaxed);
    const uint32_t phase =
        stage == 1 ? sw2e::hooks::g_post_storage_input_calls.load(std::memory_order_relaxed) % 120
                   : user0_call_index % 90;
    const bool should_log = stage == 1 ? phase == 19 : phase == 28;
    if (should_log) {
    REXLOG_INFO(
        "SM2 synthetic boot input pulse: XamInputGetState user={} user0-call #{} "
        "stage={} buttons={:#06x}",
        user_index, user0_call_index + 1, stage, buttons);
    }
  } else if (user_index == 0 && (user0_call_index % 600) == 0) {
    const uint32_t stage = sw2e::hooks::g_boot_input_stage.load(std::memory_order_relaxed);
    REXLOG_INFO("SM2 input heartbeat: XamInputGetState user0-call #{} stage={} buttons=0",
                user0_call_index + 1, stage);
  }

  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__XamInputGetKeystrokeEx) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249E874, "__imp__XamInputGetKeystrokeEx");

  const bool log =
      sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_xam_input_get_keystroke_ex_calls);
  const uint32_t user_index_ptr = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t keystroke_addr = ctx.r5.u32;
  const uint32_t requested_user = user_index_ptr != 0 ? REX_LOAD_U32(user_index_ptr) : 0xFFFFFFFF;
  const bool user0_or_any =
      requested_user == 0 || (requested_user & 0xFF) == 0xFF ||
      (flags & sw2e::hooks::kXInputFlagAnyUser) != 0;

  uint32_t result = rex::kernel::xam::XamInputGetKeystrokeEx_entry(
      sw2e::hooks::GuestU32(base, user_index_ptr), flags,
      sw2e::hooks::GuestKeystroke(base, keystroke_addr));

  if (REXCVAR_GET(sw2e_auto_boot_input) && user0_or_any && keystroke_addr != 0 &&
      (result == sw2e::hooks::kXErrorEmpty || result != sw2e::hooks::kXErrorSuccess)) {
    const uint32_t call_index =
        sw2e::hooks::g_boot_keystroke_calls.fetch_add(1, std::memory_order_relaxed);
    uint16_t virtual_key = 0;
    uint16_t keystroke_flags = 0;
    if (sw2e::hooks::BootKeystroke(call_index, virtual_key, keystroke_flags)) {
      REX_STORE_U16(keystroke_addr + 0, virtual_key);
      REX_STORE_U16(keystroke_addr + 2, 0);
      REX_STORE_U16(keystroke_addr + 4, keystroke_flags);
      REX_STORE_U8(keystroke_addr + 6, 0);
      REX_STORE_U8(keystroke_addr + 7, 0);
      if (user_index_ptr != 0) {
        REX_STORE_U32(user_index_ptr, 0);
      }
      result = sw2e::hooks::kXErrorSuccess;

      if (log || keystroke_flags == sw2e::hooks::kXInputKeystrokeKeyDown) {
        REXLOG_INFO(
            "SM2 synthetic keystroke: XamInputGetKeystrokeEx call #{} vk={:#06x} "
            "flags={:#06x} stage={}",
            call_index + 1, virtual_key, keystroke_flags,
            sw2e::hooks::g_boot_input_stage.load(std::memory_order_relaxed));
      }
    }
  }

  if (log) {
    const uint16_t vk = keystroke_addr != 0 ? REX_LOAD_U16(keystroke_addr + 0) : 0;
    const uint16_t out_flags = keystroke_addr != 0 ? REX_LOAD_U16(keystroke_addr + 4) : 0;
    REXLOG_INFO(
        "SM2 input event import: XamInputGetKeystrokeEx user_ptr={:#010x} user={} "
        "flags={:#010x} ks={:#010x} -> {:#010x} vk={:#06x} ks_flags={:#06x}",
        user_index_ptr, requested_user, flags, keystroke_addr, result, vk, out_flags);
  }

  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__XamContentCreateEx) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249E7D4, "__imp__XamContentCreateEx");

  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_xam_content_create_ex_calls);
  if (log) {
    REXLOG_INFO(
        "SM2 save import call: XamContentCreateEx user={} root={:#010x} data={:#010x} "
        "flags={:#010x} disp={:#010x} license={:#010x} cache={:#010x} size={} ovl={:#010x}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32,
        ctx.r10.u64, REX_LOAD_U32(ctx.r1.u32 + 84));
  }

  const uint32_t overlapped = REX_LOAD_U32(ctx.r1.u32 + 84);
  const uint32_t result = rex::kernel::xam::XamContentCreateEx_entry(
      ctx.r3.u32, sw2e::hooks::GuestString(base, ctx.r4.u32),
      sw2e::hooks::GuestVoid(base, ctx.r5.u32), ctx.r6.u32,
      sw2e::hooks::GuestU32(base, ctx.r7.u32), sw2e::hooks::GuestU32(base, ctx.r8.u32),
      ctx.r9.u32, ctx.r10.u64, sw2e::hooks::GuestVoid(base, overlapped));

  if (log) {
    REXLOG_INFO("SM2 save import ret: XamContentCreateEx -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__XamContentClose) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249E7E4, "__imp__XamContentClose");

  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_xam_content_close_calls);
  if (log) {
    REXLOG_INFO("SM2 save import call: XamContentClose root={:#010x} ovl={:#010x}", ctx.r3.u32,
                ctx.r4.u32);
  }
  const uint32_t result = rex::kernel::xam::XamContentClose_entry(
      sw2e::hooks::GuestString(base, ctx.r3.u32), sw2e::hooks::GuestVoid(base, ctx.r4.u32));
  if (log) {
    REXLOG_INFO("SM2 save import ret: XamContentClose -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__XamContentSetThumbnail) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249E7F4, "__imp__XamContentSetThumbnail");

  const bool log =
      sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_xam_content_set_thumbnail_calls);
  if (log) {
    REXLOG_INFO(
        "SM2 save import call: XamContentSetThumbnail user={} data={:#010x} buf={:#010x} "
        "len={} ovl={:#010x}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  }
  const uint32_t result = rex::kernel::xam::XamContentSetThumbnail_entry(
      ctx.r3.u32, sw2e::hooks::GuestVoid(base, ctx.r4.u32),
      sw2e::hooks::GuestVoid(base, ctx.r5.u32), ctx.r6.u32,
      sw2e::hooks::GuestVoid(base, ctx.r7.u32));
  if (log) {
    REXLOG_INFO("SM2 save import ret: XamContentSetThumbnail -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__NtClose) {
  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_nt_close_calls);
  if (log) {
    REXLOG_INFO("SM2 file import call: NtClose handle={:#010x}", ctx.r3.u32);
  }
  const uint32_t result = rex::kernel::xboxkrnl::NtClose_entry(ctx.r3.u32);
  if (log) {
    REXLOG_INFO("SM2 file import ret: NtClose -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__NtCreateFile) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249E974, "__imp__NtCreateFile");

  const uint32_t handle_out = ctx.r3.u32;
  const uint32_t object_attrs = ctx.r5.u32;
  const uint32_t create_options = REX_LOAD_U32(ctx.r1.u32 + 84);
  const std::string path = sw2e::hooks::GuestObjectPath(base, object_attrs);
  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_nt_create_file_calls);
  if (log) {
    REXLOG_INFO(
        "SM2 file import call: NtCreateFile path='{}' root={:#010x} access={:#010x} "
        "attrs={:#010x} share={:#010x} disp={:#010x} options={:#010x} out={:#010x}",
        path, sw2e::hooks::GuestObjectRootDirectory(base, object_attrs), ctx.r4.u32, ctx.r8.u32,
        ctx.r9.u32, ctx.r10.u32, create_options, handle_out);
  }

  const uint32_t result = rex::kernel::xboxkrnl::NtCreateFile_entry(
      sw2e::hooks::GuestU32(base, handle_out), ctx.r4.u32,
      sw2e::hooks::GuestObjectAttributes(base, object_attrs),
      sw2e::hooks::GuestIoStatusBlock(base, ctx.r6.u32), sw2e::hooks::GuestU64(base, ctx.r7.u32),
      ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, create_options);
  const uint32_t opened_handle = handle_out != 0 ? REX_LOAD_U32(handle_out) : 0;
  if (log) {
    REXLOG_INFO("SM2 file import ret: NtCreateFile path='{}' -> {:#010x} handle={:#010x}",
                path, result, opened_handle);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__NtOpenFile) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8249E9D4, "__imp__NtOpenFile");

  const uint32_t handle_out = ctx.r3.u32;
  const uint32_t object_attrs = ctx.r5.u32;
  const std::string path = sw2e::hooks::GuestObjectPath(base, object_attrs);
  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_nt_open_file_calls);
  if (log) {
    REXLOG_INFO(
        "SM2 file import call: NtOpenFile path='{}' root={:#010x} access={:#010x} "
        "options={:#010x} out={:#010x}",
        path, sw2e::hooks::GuestObjectRootDirectory(base, object_attrs), ctx.r4.u32, ctx.r7.u32,
        handle_out);
  }

  const uint32_t result = rex::kernel::xboxkrnl::NtOpenFile_entry(
      sw2e::hooks::GuestU32(base, handle_out), ctx.r4.u32,
      sw2e::hooks::GuestObjectAttributes(base, object_attrs),
      sw2e::hooks::GuestIoStatusBlock(base, ctx.r6.u32), ctx.r7.u32);
  const uint32_t opened_handle = handle_out != 0 ? REX_LOAD_U32(handle_out) : 0;
  if (log) {
    REXLOG_INFO("SM2 file import ret: NtOpenFile path='{}' -> {:#010x} handle={:#010x}", path,
                result, opened_handle);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__NtReadFile) {
  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_nt_read_file_calls);
  if (log) {
    REXLOG_INFO(
        "SM2 file import call: NtReadFile handle={:#010x} event={:#010x} iosb={:#010x} "
        "buf={:#010x} len={} off={:#010x}",
        ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
  }
  const uint32_t result = rex::kernel::xboxkrnl::NtReadFile_entry(
      ctx.r3.u32, ctx.r4.u32, sw2e::hooks::GuestVoid(base, ctx.r5.u32),
      sw2e::hooks::GuestVoid(base, ctx.r6.u32), sw2e::hooks::GuestIoStatusBlock(base, ctx.r7.u32),
      sw2e::hooks::GuestVoid(base, ctx.r8.u32), ctx.r9.u32,
      sw2e::hooks::GuestU64(base, ctx.r10.u32));
  if (log) {
    REXLOG_INFO("SM2 file import ret: NtReadFile -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__NtWriteFile) {
  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_nt_write_file_calls);
  if (log) {
    REXLOG_INFO(
        "SM2 file import call: NtWriteFile handle={:#010x} event={:#010x} iosb={:#010x} "
        "buf={:#010x} len={} off={:#010x}",
        ctx.r3.u32, ctx.r4.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
  }
  const uint32_t result = rex::kernel::xboxkrnl::NtWriteFile_entry(
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, sw2e::hooks::GuestVoid(base, ctx.r6.u32),
      sw2e::hooks::GuestIoStatusBlock(base, ctx.r7.u32), sw2e::hooks::GuestVoid(base, ctx.r8.u32),
      ctx.r9.u32, sw2e::hooks::GuestU64(base, ctx.r10.u32));
  if (log) {
    REXLOG_INFO("SM2 file import ret: NtWriteFile -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__NtFlushBuffersFile) {
  const bool log = sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_nt_flush_buffers_file_calls);
  if (log) {
    REXLOG_INFO("SM2 file import call: NtFlushBuffersFile handle={:#010x} iosb={:#010x}",
                ctx.r3.u32, ctx.r4.u32);
  }
  const uint32_t result = rex::kernel::xboxkrnl::NtFlushBuffersFile_entry(
      ctx.r3.u32, sw2e::hooks::GuestIoStatusBlock(base, ctx.r4.u32));
  if (log) {
    REXLOG_INFO("SM2 file import ret: NtFlushBuffersFile -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(__imp__NtWaitForSingleObjectEx) {
  const bool log =
      sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_nt_wait_for_single_object_ex_calls);
  if (log) {
    REXLOG_INFO(
        "SM2 sync import call: NtWaitForSingleObjectEx handle={:#010x} mode={} alertable={} "
        "timeout={:#010x}",
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  const uint32_t result = rex::kernel::xboxkrnl::NtWaitForSingleObjectEx_entry(
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, sw2e::hooks::GuestU64(base, ctx.r6.u32));
  if (log) {
    REXLOG_INFO("SM2 sync import ret: NtWaitForSingleObjectEx -> {:#010x}", result);
  }
  ctx.r3.u64 = result;
}

extern "C" REX_FUNC(sub_820F3C48) {
  const uint32_t event_addr = ctx.r3.u32;

  __imp__sub_820F3C48(ctx, base);

  if (!sw2e::hooks::IsStashedGuestEvent(base, event_addr)) {
    return;
  }

  const bool log =
      sw2e::hooks::ShouldLogImportCall(sw2e::hooks::g_guest_event_clear_calls);
  if (log) {
    REXLOG_INFO("SM2 sync fix: mirroring guest event clear to host event {:#010x}",
                event_addr);
  }

  const auto saved_r3 = ctx.r3;
  const auto saved_r4 = ctx.r4;
  const auto saved_r5 = ctx.r5;
  const auto saved_r6 = ctx.r6;
  const auto saved_r7 = ctx.r7;
  const auto saved_r8 = ctx.r8;
  const auto saved_r9 = ctx.r9;
  const auto saved_r10 = ctx.r10;
  const auto saved_r11 = ctx.r11;
  const auto saved_r12 = ctx.r12;
  const uint64_t saved_lr = ctx.lr;

  ctx.r3.u64 = event_addr;
  __imp__KeResetEvent(ctx, base);

  ctx.r3 = saved_r3;
  ctx.r4 = saved_r4;
  ctx.r5 = saved_r5;
  ctx.r6 = saved_r6;
  ctx.r7 = saved_r7;
  ctx.r8 = saved_r8;
  ctx.r9 = saved_r9;
  ctx.r10 = saved_r10;
  ctx.r11 = saved_r11;
  ctx.r12 = saved_r12;
  ctx.lr = saved_lr;
}

extern "C" REX_FUNC(sub_8210F1F8) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::EnsureStorageDeviceSelected(base, "sub_8210F1F8");
  sw2e::hooks::LogHookHitOnce(seen, 0x8210F1F8, "sub_8210F1F8");
  sw2e::hooks::LogHookCall(hits, 0x8210F1F8, "sub_8210F1F8", ctx);
  __imp__sub_8210F1F8(ctx, base);
}

extern "C" REX_FUNC(sub_82110570) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::EnsureStorageDeviceSelected(base, "sub_82110570");
  sw2e::hooks::LogHookHitOnce(seen, 0x82110570, "sub_82110570");
  sw2e::hooks::LogHookCall(hits, 0x82110570, "sub_82110570", ctx);
  __imp__sub_82110570(ctx, base);
}

extern "C" REX_FUNC(sub_8210E528) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8210E528, "sub_8210E528");

  const uint32_t call_index =
      sw2e::hooks::g_archive_read_entry_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t packed_id = ctx.r3.u32;
  const uint32_t entry_index = sw2e::hooks::ArchiveEntryIndex(packed_id);
  const uint32_t selector = sw2e::hooks::ArchiveSelector(packed_id);
  const uint32_t sector_offset = ctx.r4.u32;
  const uint32_t sector_count = ctx.r5.u32;
  const uint32_t output_addr = ctx.r6.u32;
  const uint32_t read_bytes = sector_count << 11;
  const bool log = sw2e::hooks::ShouldLogArchiveEntry(call_index, entry_index);

  if (log) {
    REXLOG_INFO(
        "SM2 archive read call #{}: selector={} entry={} packed={:#010x} "
        "sector_offset={} sectors={} bytes={} out={:#010x} lr={:#010x}",
        call_index + 1, selector, entry_index, packed_id, sector_offset, sector_count,
        read_bytes, output_addr, ctx.lr);
  }

  __imp__sub_8210E528(ctx, base);

  if (log) {
    const uint32_t result_addr = ctx.r3.u32 != 0 ? ctx.r3.u32 : output_addr;
    REXLOG_INFO(
        "SM2 archive read ret #{}: selector={} entry={} result={:#010x} magic='{}'",
        call_index + 1, selector, entry_index, result_addr,
        sw2e::hooks::GuestMagicPreview(base, result_addr));
  }
}

extern "C" REX_FUNC(sub_82347750) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82347750, "sub_82347750");

  const uint32_t call_index =
      sw2e::hooks::g_asset_load_wrapper_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t source = ctx.r3.u32;
  const uint32_t entry_or_packed_id = ctx.r4.u32;
  const uint32_t buffer_addr = ctx.r5.u32;
  const uint32_t immediate = ctx.r6.u32;
  const uint32_t byte_count = ctx.r7.u32;
  const uint32_t byte_offset = ctx.r8.u32;
  const uint32_t packed_id =
      sw2e::hooks::PackedArchiveIdFromAssetRequest(source, entry_or_packed_id);
  const uint32_t entry_index = sw2e::hooks::ArchiveEntryIndex(packed_id);
  const uint32_t selector = sw2e::hooks::ArchiveSelector(packed_id);
  const uint32_t lr = ctx.lr;
  const bool log = sw2e::hooks::ShouldLogAssetLoad(call_index, entry_index, lr);

  if (log) {
    REXLOG_INFO(
        "SM2 asset load call #{}: source={} selector={} entry={} packed={:#010x} "
        "buffer={:#010x} immediate={} byte_count={} byte_offset={} lr={:#010x}",
        call_index + 1, source, selector, entry_index, packed_id, buffer_addr, immediate,
        byte_count, byte_offset, lr);
  }

  __imp__sub_82347750(ctx, base);

  if (log) {
    const uint32_t result_addr = ctx.r3.u32;
    REXLOG_INFO("SM2 asset load ret #{}: entry={} result={:#010x} magic='{}'",
                call_index + 1, entry_index, result_addr,
                sw2e::hooks::GuestMagicPreview(base, result_addr));
  }
}

extern "C" REX_FUNC(sub_8236BBF8) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8236BBF8, "sub_8236BBF8");

  const uint32_t call_index =
      sw2e::hooks::g_debug_command_parser_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t command_addr = ctx.r3.u32;
  const uint32_t output_addr = ctx.r4.u32;
  const uint32_t command_byte_addr = command_addr + 4;
  uint8_t command = 0;
  if (command_byte_addr >= command_addr &&
      sw2e::hooks::GuestRangeIsCommitted(base, command_byte_addr, 1, false)) {
    command = REX_LOAD_U8(command_byte_addr);
  }

  const bool interesting =
      command == 'a' || command == 'c' || command == 'd' || command == 'f' || command == 'g' ||
      command == 'm' || command == 'p' || command == 't' || command == 'x';
  const bool log = call_index < 64 || interesting || (call_index % 2048) == 0;
  if (log) {
    REXLOG_INFO(
        "SM2 debug command parser call #{}: cmd='{}' input={:#010x} output={:#010x} "
        "input-preview='{}' lr={:#010x}",
        call_index + 1, command >= 32 && command <= 126 ? static_cast<char>(command) : '.',
        command_addr, output_addr, sw2e::hooks::GuestCStringPreview(base, command_byte_addr, 32),
        ctx.lr);
  }

  __imp__sub_8236BBF8(ctx, base);

  if (log) {
    REXLOG_INFO("SM2 debug command parser ret #{}: result={:#010x} output='{}'",
                call_index + 1, ctx.r3.u32,
                sw2e::hooks::GuestCStringPreview(base, output_addr, 96));
  }
}

SW2E_LOGGING_PASSTHROUGH_HOOK(sub_8236D978, 0x8236D978)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_8236DCE0, 0x8236DCE0)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_8236DF60, 0x8236DF60)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_8236E780, 0x8236E780)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_823523D0, 0x823523D0)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_82353A98, 0x82353A98)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_82353BF8, 0x82353BF8)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_8234C7A8, 0x8234C7A8)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_8234D948, 0x8234D948)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_82351CD8, 0x82351CD8)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_823540B0, 0x823540B0)
extern "C" REX_FUNC(sub_8235C820) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8235C820, "sub_8235C820");
  sw2e::hooks::LogHookCall(hits, 0x8235C820, "sub_8235C820", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 0x400)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_8235C820");
    return;
  }

  __imp__sub_8235C820(ctx, base);
}
extern "C" REX_FUNC(sub_8235CC48) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8235CC48, "sub_8235CC48");
  sw2e::hooks::LogHookCall(hits, 0x8235CC48, "sub_8235CC48", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 0x400)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_8235CC48");
    return;
  }

  __imp__sub_8235CC48(ctx, base);
}
extern "C" REX_FUNC(sub_8235D660) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x8235D660, "sub_8235D660");
  sw2e::hooks::LogHookCall(hits, 0x8235D660, "sub_8235D660", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 0x800)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_8235D660");
    return;
  }

  __imp__sub_8235D660(ctx, base);
}
extern "C" REX_FUNC(sub_82369820) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82369820, "sub_82369820");
  sw2e::hooks::LogHookCall(hits, 0x82369820, "sub_82369820", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 0x400)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_82369820");
    return;
  }

  __imp__sub_82369820(ctx, base);
}
extern "C" REX_FUNC(sub_82363DA8) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82363DA8, "sub_82363DA8");
  sw2e::hooks::LogHookCall(hits, 0x82363DA8, "sub_82363DA8", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 32)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_82363DA8");
    return;
  }

  __imp__sub_82363DA8(ctx, base);
}
extern "C" REX_FUNC(sub_82363FE8) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82363FE8, "sub_82363FE8");
  sw2e::hooks::LogHookCall(hits, 0x82363FE8, "sub_82363FE8", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 32)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_82363FE8");
    return;
  }

  __imp__sub_82363FE8(ctx, base);
}
extern "C" REX_FUNC(sub_82364248) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82364248, "sub_82364248");
  sw2e::hooks::LogHookCall(hits, 0x82364248, "sub_82364248", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 0x120)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_82364248");
    return;
  }

  __imp__sub_82364248(ctx, base);
}
extern "C" REX_FUNC(sub_82361B70) {
  static std::atomic_bool seen{};
  static std::atomic_uint32_t hits{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82361B70, "sub_82361B70");
  sw2e::hooks::LogHookCall(hits, 0x82361B70, "sub_82361B70", ctx);

  if (!sw2e::hooks::StoredDrawCursorWritable(base, ctx.r3.u32, 0x200)) {
    sw2e::hooks::LogTransientDrawSkip(base, ctx.r3.u32, 48, "sub_82361B70");
    return;
  }

  __imp__sub_82361B70(ctx, base);
}
extern "C" REX_FUNC(sub_82365290) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82365290, "sub_82365290");

  const uint32_t call_index =
      sw2e::hooks::g_render_packet_page_helper_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t state_addr = ctx.r3.u32;
  const uint32_t packet_out_addr = ctx.r4.u32;
  const uint32_t words_out_addr = ctx.r5.u32;
  sw2e::hooks::LogRenderPacketState(base, call_index, "page", 0x82365290, "call", ctx,
                                    state_addr, packet_out_addr, words_out_addr);

  __imp__sub_82365290(ctx, base);

  sw2e::hooks::LogRenderPacketState(base, call_index, "page", 0x82365290, "ret", ctx,
                                    state_addr, packet_out_addr, words_out_addr);
  sw2e::hooks::LogRenderPacketPageResult(base, call_index, packet_out_addr, words_out_addr);
}

extern "C" REX_FUNC(sub_82365428) {
  static std::atomic_bool seen{};
  sw2e::hooks::LogHookHitOnce(seen, 0x82365428, "sub_82365428");

  const uint32_t call_index =
      sw2e::hooks::g_render_packet_block_helper_calls.fetch_add(1, std::memory_order_relaxed);
  const uint32_t state_addr = ctx.r3.u32;
  const uint32_t block_addr = ctx.r4.u32;
  sw2e::hooks::LogRenderPacketState(base, call_index, "block", 0x82365428, "call", ctx,
                                    state_addr, block_addr, 0);

  __imp__sub_82365428(ctx, base);

  sw2e::hooks::LogRenderPacketState(base, call_index, "block", 0x82365428, "ret", ctx,
                                    state_addr, block_addr, 0);
  sw2e::hooks::LogRenderPacketBlockResult(base, call_index, block_addr, ctx.r3.u32);
}

SW2E_PASSTHROUGH_HOOK(sub_82355400, 0x82355400)
SW2E_PASSTHROUGH_HOOK(sub_82356300, 0x82356300)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_82385138, 0x82385138)
SW2E_PASSTHROUGH_HOOK(sub_82385210, 0x82385210)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_82385800, 0x82385800)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_823859A8, 0x823859A8)
SW2E_PASSTHROUGH_HOOK(sub_82386910, 0x82386910)
SW2E_LOGGING_PASSTHROUGH_HOOK(sub_82386F50, 0x82386F50)
