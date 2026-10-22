#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
namespace osgui::detail {
// Immutable snapshots share storage. Writers detach before modifying pixels.
class Pixels {
    inline static uint64_t allocated_ = 0, peak_ = 0;
    struct Storage {
        uint32_t references, width, height;
        uint32_t* data;
    };
    Storage* storage_ = nullptr;
    static void release(Storage* value) {
        if (value && __atomic_sub_fetch(&value->references, 1, __ATOMIC_ACQ_REL) == 0) {
            __atomic_sub_fetch(&allocated_, uint64_t(value->width) * value->height * 4,
                               __ATOMIC_RELAXED);
            free(value->data);
            free(value);
        }
    }

   public:
    static uint64_t peakBytes() { return __atomic_load_n(&peak_, __ATOMIC_RELAXED); }
    Pixels() = default;
    Pixels(const Pixels& other) : storage_(other.storage_) {
        if (storage_) __atomic_add_fetch(&storage_->references, 1, __ATOMIC_RELAXED);
    }
    Pixels& operator=(const Pixels& other) {
        Storage* next = other.storage_;
        if (next) __atomic_add_fetch(&next->references, 1, __ATOMIC_RELAXED);
        release(storage_);
        storage_ = next;
        return *this;
    }
    ~Pixels() { release(storage_); }
    bool resize(uint32_t width, uint32_t height) {
        if (!width || !height || width > 704 || height > 448) return false;
        auto* next = static_cast<Storage*>(malloc(sizeof(Storage)));
        if (!next) return false;
        next->data = static_cast<uint32_t*>(malloc(uint64_t(width) * height * 4));
        if (!next->data) {
            free(next);
            return false;
        }
        next->references = 1;
        next->width = width;
        next->height = height;
        const auto current = __atomic_add_fetch(
            &allocated_, uint64_t(width) * height * 4, __ATOMIC_RELAXED);
        auto peak = __atomic_load_n(&peak_, __ATOMIC_RELAXED);
        while (current > peak &&
               !__atomic_compare_exchange_n(&peak_, &peak, current, false,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
        for (uint32_t i = 0; i < width * height; ++i) next->data[i] = 0x00f4f4f4;
        release(storage_);
        storage_ = next;
        return true;
    }
    bool writable() {
        if (!storage_) return false;
        if (__atomic_load_n(&storage_->references, __ATOMIC_ACQUIRE) == 1) return true;
        Pixels next;
        if (!next.resize(width(), height())) return false;
        memcpy(next.storage_->data, storage_->data, uint64_t(width()) * height() * 4);
        *this = next;
        return true;
    }
    uint32_t width() const { return storage_ ? storage_->width : 0; }
    uint32_t height() const { return storage_ ? storage_->height : 0; }
    const uint32_t* data() const { return storage_ ? storage_->data : nullptr; }
    uint32_t* mutableData() { return storage_ ? storage_->data : nullptr; }
};
}  // namespace osgui::detail
