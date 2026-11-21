// Unit tests for the bindless registry (P3/P5): key -> (index, kind), the 0 fallback, kind changes, release and reuse.

#include <cstdint>
#include <cstdio>

#include "bindless_registry.h"

namespace {

int failures = 0;

#define EXPECT(condition)                                                \
    do {                                                                 \
        if (!(condition)) {                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

leonida::DescriptorKey key(std::uint32_t seed) {
    leonida::DescriptorKey out{};
    for (std::uint32_t i = 0; i < 8; ++i) out[i] = seed * 2654435761u + i;
    return out;
}

int test_ensure_find_release() {
    leonida::BindlessRegistry registry(8);
    const leonida::DescriptorKey a = key(1);
    EXPECT(registry.lookup(a) == 0);  // miss is 0, the shader's fallback
    const leonida::BindlessEntry entry = registry.ensure(a, leonida::BindlessKind::image_2d);
    EXPECT(entry.index != 0 && entry.index != 0);
    EXPECT(registry.lookup(a) == entry.index);
    EXPECT(registry.ensure(a, leonida::BindlessKind::image_2d).index == entry.index);  // sticky
    EXPECT(registry.release(a));
    EXPECT(registry.lookup(a) == 0);
    EXPECT(!registry.in_use(entry.index));
    return failures;
}

int test_kinds_and_slots() {
    leonida::BindlessRegistry registry(8);
    const leonida::DescriptorKey image = key(2), sampler = key(3);
    const auto i = registry.ensure(image, leonida::BindlessKind::image_2d);
    const auto s = registry.ensure(sampler, leonida::BindlessKind::sampler);
    EXPECT(i.index != 0 && s.index != 0);
    EXPECT(i.index != s.index);
    EXPECT(registry.allocated() == 2);

    // A key registered with another kind moves to a fresh slot; the old one is released.
    const auto moved = registry.ensure(image, leonida::BindlessKind::image_storage);
    EXPECT(moved.index != 0);
    EXPECT(registry.lookup(image) == moved.index);
    EXPECT(registry.allocated() == 2);  // one image slot + one sampler slot, no leak
    return failures;
}

int test_full_pool_and_reuse() {
    leonida::BindlessRegistry registry(2);
    const leonida::DescriptorKey a = key(4), b = key(5), c = key(6);
    EXPECT(registry.ensure(a, leonida::BindlessKind::image_2d).index != 0);
    EXPECT(registry.ensure(b, leonida::BindlessKind::image_2d).index != 0);
    EXPECT(registry.ensure(c, leonida::BindlessKind::image_2d).index == 0);  // full
    EXPECT(registry.release(a));
    EXPECT(registry.ensure(c, leonida::BindlessKind::image_2d).index != 0);  // a freed slot is reused
    EXPECT(registry.allocated() == 2);
    return failures;
}

int test_release_unknown_key_is_rejected() {
    leonida::BindlessRegistry registry(4);
    EXPECT(!registry.release(key(7)));
    EXPECT(!registry.release_index(0));  // the fallback is never released
    return failures;
}

}  // namespace

int main() {
    test_ensure_find_release();
    test_kinds_and_slots();
    test_full_pool_and_reuse();
    test_release_unknown_key_is_rejected();

    if (failures) {
        std::fprintf(stderr, "bindless_registry_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("bindless_registry_test: all tests passed\n");
    return 0;
}
