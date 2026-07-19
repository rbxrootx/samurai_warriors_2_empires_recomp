#include "generated/default/samurai_warriors_2_empires_init.h"
#include "recomp/SourceFunctionMap.h"

#include <rex/cvar.h>

#include <atomic>

namespace {

void LogSceneArchiveLoaderOnce(std::atomic_bool& seen, uint32_t address, uint32_t entry_id,
                               uint32_t caller) {
  if (seen.exchange(true, std::memory_order_relaxed)) {
    return;
  }

  const auto* info = sw2e::hooks::FindKnownFunction(address);
  REXLOG_INFO(
      "SW2E SMusou4Scene loader active: {} @ {:#010x} source={} first_entry={:#010x} "
      "caller={:#010x}",
      info ? info->label : "scene archive object loader", address,
      info ? info->source_path : ".\\xbox\\app\\SMusou4Scene.cpp", entry_id, caller);
}

}  // namespace

#define SW2E_SCENE_ARCHIVE_LOADER(symbol, address)                                  \
  extern "C" REX_FUNC(symbol) {                                                     \
    static std::atomic_bool seen{};                                                  \
    LogSceneArchiveLoaderOnce(seen, address, ctx.r6.u32, ctx.lr);                   \
    __imp__##symbol(ctx, base);                                                      \
  }

// Direct source ownership is proven by embedded path xrefs in the retail XEX. The six
// wrappers instantiate distinct scene-object classes from archive entries.
SW2E_SCENE_ARCHIVE_LOADER(sub_820F7908, 0x820F7908)
SW2E_SCENE_ARCHIVE_LOADER(sub_820F79F0, 0x820F79F0)
SW2E_SCENE_ARCHIVE_LOADER(sub_820F7AD8, 0x820F7AD8)
SW2E_SCENE_ARCHIVE_LOADER(sub_820FAA48, 0x820FAA48)
SW2E_SCENE_ARCHIVE_LOADER(sub_820FD240, 0x820FD240)
SW2E_SCENE_ARCHIVE_LOADER(sub_820FE870, 0x820FE870)

#undef SW2E_SCENE_ARCHIVE_LOADER