// CPU-only tests for moltenvk_no_contraction.h: which devices get NoContraction dropped, and which decorations a
// module keeps. The modules are built by hand; no Vulkan instance or GPU is needed.
#include "../src/moltenvk_no_contraction.h"

#include <cstdio>
#include <initializer_list>
#include <set>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
}

using leonida::MetalSafeMath;

// Ids of the module below.
enum : std::uint32_t {
    kVoid = 1, kFloat, kFunctionType, kA, kB, kC, kMain, kLabel,
    kProduct,       // a * b, stored on its own
    kSum,           // a + b
    kFusedProduct,  // a * c, an operand of kFusedSum
    kFusedSum,      // (a * c) + b
    kNegProduct,    // b * c, negated by kNeg
    kNeg,           // -(b * c)
    kNegSum,        // a - -(b * c)
    kBound,
};
constexpr std::uint32_t kSzinp = 4461, kNoContraction = 42;

struct Module {
    bool signed_zero_inf_nan_preserve = true;
    std::set<std::uint32_t> decorated{kProduct, kSum, kFusedProduct, kFusedSum, kNegProduct, kNeg, kNegSum};

    std::vector<std::uint32_t> words() const {
        std::vector<std::uint32_t> w{0x07230203, 0x00010300, 0, kBound, 0};
        const auto op = [&](std::uint32_t code, std::initializer_list<std::uint32_t> operands) {
            w.push_back(std::uint32_t(operands.size() + 1) << 16 | code);
            w.insert(w.end(), operands);
        };
        op(17, {1});                                 // OpCapability Shader
        op(14, {0, 1});                              // OpMemoryModel Logical GLSL450
        op(15, {5, kMain, 0x6e69616d, 0});           // OpEntryPoint GLCompute %main "main"
        op(16, {kMain, 17, 1, 1, 1});                // OpExecutionMode %main LocalSize 1 1 1
        if (signed_zero_inf_nan_preserve) op(16, {kMain, kSzinp, 32});
        for (const std::uint32_t id : decorated) op(71, {id, kNoContraction});
        op(19, {kVoid});                             // OpTypeVoid
        op(22, {kFloat, 32});                        // OpTypeFloat 32
        op(33, {kFunctionType, kVoid});              // OpTypeFunction
        op(43, {kFloat, kA, 0x3f800000});            // OpConstant
        op(43, {kFloat, kB, 0x40000000});
        op(43, {kFloat, kC, 0x40400000});
        op(54, {kVoid, kMain, 0, kFunctionType});    // OpFunction
        op(248, {kLabel});                           // OpLabel
        op(133, {kFloat, kProduct, kA, kB});         // OpFMul
        op(129, {kFloat, kSum, kA, kB});             // OpFAdd
        op(133, {kFloat, kFusedProduct, kA, kC});
        op(129, {kFloat, kFusedSum, kFusedProduct, kB});
        op(133, {kFloat, kNegProduct, kB, kC});
        op(127, {kFloat, kNeg, kNegProduct});        // OpFNegate
        op(131, {kFloat, kNegSum, kA, kNeg});        // OpFSub
        op(253, {});                                 // OpReturn
        op(56, {});                                  // OpFunctionEnd
        return w;
    }
};

std::vector<std::uint32_t> dropped(const Module& m, MetalSafeMath math) {
    return leonida::drop_no_contraction(m.words(), math);
}

void test_policy() {
    using leonida::moltenvk_safe_math;
    constexpr std::uint32_t v142 = 10402, v133 = 10303;
    expect(moltenvk_safe_math(VK_DRIVER_ID_MESA_RADV, v142, nullptr) == MetalSafeMath::never, "RADV");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MESA_RADV, v142, "0") == MetalSafeMath::never, "RADV, MVK env set");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v142, nullptr) == MetalSafeMath::if_declared, "1.4.2 default");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v142, "") == MetalSafeMath::if_declared, "1.4.2, empty env");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v142, "0") == MetalSafeMath::always, "1.4.2 fast math off");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v142, "1") == MetalSafeMath::never, "1.4.2 fast math always");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v142, "2") == MetalSafeMath::if_declared, "1.4.2 on demand");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v142, "yes") == MetalSafeMath::never, "1.4.2 unknown value");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v133, nullptr) == MetalSafeMath::never, "1.3.3 default");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v133, "0") == MetalSafeMath::always, "1.3.3 fast math off");
    expect(moltenvk_safe_math(VK_DRIVER_ID_MOLTENVK, v133, "2") == MetalSafeMath::if_declared, "1.3.3 on demand");
}

void test_drop() {
    const Module declared;
    Module kept = declared;
    kept.decorated = {kFusedProduct, kNegProduct};  // the multiplies an add or subtract uses directly
    expect(dropped(declared, MetalSafeMath::if_declared) == kept.words(), "declared, on demand: keep fused muls");
    expect(dropped(declared, MetalSafeMath::always) == kept.words(), "declared, fast math off");
    expect(dropped(declared, MetalSafeMath::never) == declared.words(), "declared, not MoltenVK");

    Module undeclared;
    undeclared.signed_zero_inf_nan_preserve = false;
    Module undeclared_kept = undeclared;
    undeclared_kept.decorated = kept.decorated;
    expect(dropped(undeclared, MetalSafeMath::if_declared) == undeclared.words(), "undeclared, on demand: unchanged");
    expect(dropped(undeclared, MetalSafeMath::always) == undeclared_kept.words(), "undeclared, fast math off");

    Module bare;
    bare.decorated.clear();
    expect(dropped(bare, MetalSafeMath::always) == bare.words(), "no decorations");

    std::vector<std::uint32_t> truncated = declared.words();
    truncated.resize(truncated.size() - 3);  // OpFunctionEnd, OpReturn and the last word of the OpFSub
    expect(leonida::drop_no_contraction(truncated, MetalSafeMath::always) == truncated, "truncated module");
    const std::vector<std::uint32_t> not_spirv{1, 2, 3, 4, 5, 6};
    expect(leonida::drop_no_contraction(not_spirv, MetalSafeMath::always) == not_spirv, "not SPIR-V");
}

}  // namespace

int main() {
    test_policy();
    test_drop();
    if (failures) return 1;
    std::printf("moltenvk_no_contraction: all tests passed\n");
    return 0;
}
