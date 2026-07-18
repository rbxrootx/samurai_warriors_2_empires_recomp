#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace sw2e::hooks {

enum class FunctionConfidence : uint8_t {
  Generated,
  Observed,
  Confirmed,
};

struct FunctionInfo {
  uint32_t address;
  std::string_view symbol;
  std::string_view label;
  FunctionConfidence confidence;
  std::string_view notes;
};

std::span<const FunctionInfo> KnownFunctions();
const FunctionInfo* FindKnownFunction(uint32_t address);
std::string_view ConfidenceName(FunctionConfidence confidence);

}  // namespace sw2e::hooks
