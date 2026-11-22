#pragma once

// MoltenVK's SPIRV-Cross turns every NoContraction OpFAdd, OpFSub and OpFMul into a call to a [[clang::optnone]]
// helper (spvFAdd, spvFSub, spvFMul), which made a translated shader 7 to 15 times slower on an Apple M3 Max
// (docs/design.md, "NoContraction on MoltenVK"). When MoltenVK compiles a shader with Metal's safe math, Metal
// rounds separate statements separately, so the decoration is only needed where Metal still fuses: inside one
// expression, `a * b + c` becomes llvm.fmuladd even in safe math, and SPIRV-Cross writes a multiply whose result
// only an add or subtract uses into that add's expression. Those multiplies keep NoContraction.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace leonida {

// When MoltenVK compiles a shader with MTLMathModeSafe (MVKDevice::getMTLCompileOptions).
enum class MetalSafeMath {
    never,        // not MoltenVK, or its fast math is always on
    if_declared,  // fast math on demand: safe when the entry point declares SignedZeroInfNanPreserve
    always,       // MVK_CONFIG_FAST_MATH_ENABLED=0
};

// `fast_math_env` is MVK_CONFIG_FAST_MATH_ENABLED (null when unset). MoltenVK 1.4.0 made "on demand" (2) the
// default; before that it was "always" (1). Any other value keeps every decoration.
inline MetalSafeMath moltenvk_safe_math(VkDriverId driver, std::uint32_t driver_version, const char* fast_math_env) {
    if (driver != VK_DRIVER_ID_MOLTENVK) return MetalSafeMath::never;
    constexpr std::uint32_t kOnDemandDefault = 10400;  // MVK_MAKE_VERSION(1, 4, 0), MoltenVK's driverVersion
    if (!fast_math_env || !*fast_math_env)
        return driver_version >= kOnDemandDefault ? MetalSafeMath::if_declared : MetalSafeMath::never;
    if (!std::strcmp(fast_math_env, "0")) return MetalSafeMath::always;
    if (!std::strcmp(fast_math_env, "2")) return MetalSafeMath::if_declared;
    return MetalSafeMath::never;
}

inline MetalSafeMath moltenvk_safe_math(VkDriverId driver, std::uint32_t driver_version) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: read once per device
#endif
    const char* env = std::getenv("MVK_CONFIG_FAST_MATH_ENABLED");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return moltenvk_safe_math(driver, driver_version, env);
}

// The module MoltenVK should get: without the NoContraction decorations safe math makes redundant, or unchanged
// when MoltenVK would not compile it with safe math (or the words are not a module this can read).
inline std::vector<std::uint32_t> drop_no_contraction(std::span<const std::uint32_t> words, MetalSafeMath math) {
    const std::vector<std::uint32_t> unchanged(words.begin(), words.end());
    if (math == MetalSafeMath::never || words.size() < 5 || words[0] != 0x07230203 || !words[3]) return unchanged;
    constexpr std::uint32_t kEntryPoint = 15, kExecutionMode = 16, kDecorate = 71, kCopyObject = 83,
                            kBitcast = 124, kFNegate = 127, kFAdd = 129, kFSub = 131, kFMul = 133;
    constexpr std::uint32_t kNoContraction = 42, kSignedZeroInfNanPreserve = 4461;
    const std::uint32_t bound = words[3];
    std::vector<std::uint32_t> source(bound, 0);  // a negate, copy or bitcast's operand
    std::vector<bool> multiply(bound, false), fused(bound, false), preserves(bound, false);
    std::vector<std::uint32_t> entries, summands;
    bool decorated = false;
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t op = words[i] & 0xffff, count = words[i] >> 16;
        if (!count || i + count > words.size()) return unchanged;
        const auto id = [&](std::size_t k) { return k < count && words[i + k] < bound ? words[i + k] : 0; };
        if (op == kEntryPoint) entries.push_back(id(2));
        if (op == kExecutionMode && count >= 3 && words[i + 2] == kSignedZeroInfNanPreserve) preserves[id(1)] = true;
        if (op == kDecorate && count == 3 && words[i + 2] == kNoContraction) decorated = true;
        if (op == kFMul) multiply[id(2)] = true;
        if (op == kFNegate || op == kCopyObject || op == kBitcast) source[id(2)] = id(3);
        if (op == kFAdd || op == kFSub) summands.insert(summands.end(), {id(3), id(4)});
        i += count;
    }
    if (!decorated || entries.empty()) return unchanged;
    if (math == MetalSafeMath::if_declared)
        for (const std::uint32_t entry : entries)
            if (!preserves[entry]) return unchanged;
    for (std::uint32_t value : summands) {
        for (int hops = 0; value && !multiply[value] && source[value] && hops < 8; ++hops) value = source[value];
        if (multiply[value]) fused[value] = true;
    }
    std::vector<std::uint32_t> out(words.begin(), words.begin() + 5);
    out.reserve(words.size());
    for (std::size_t i = 5; i < words.size();) {
        const std::uint32_t count = words[i] >> 16;
        const bool drop = (words[i] & 0xffff) == kDecorate && count == 3 && words[i + 2] == kNoContraction &&
                          !(words[i + 1] < bound && fused[words[i + 1]]);
        if (!drop) out.insert(out.end(), words.begin() + i, words.begin() + i + count);
        i += count;
    }
    return out;
}

}  // namespace leonida
