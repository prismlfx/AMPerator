#include "engine/RtSafety.h"

// This translation unit is the only place that replaces the global allocation functions. RealtimeEngine (and
// anything using ScopedNoAlloc) references symbols defined here, so the linker always pulls this object out of
// the amp_engine static library together with the replacements.

#ifdef NDEBUG

namespace amp::RtSafety
{
ScopedNoAlloc::ScopedNoAlloc() = default;
ScopedNoAlloc::~ScopedNoAlloc() = default;
int violations() { return 0; }
void reset() {}
bool active() { return false; }
} // namespace amp::RtSafety

#else

#include <atomic>
#include <cstdlib>
#include <new>

namespace
{
// Constant-initialised and trivially destructible: no TLS initialiser, safe to touch from operator new.
thread_local int noAllocDepth = 0;
std::atomic<int> violationCount { 0 };

inline void noteAllocation() noexcept
{
    if (noAllocDepth > 0)
        violationCount.fetch_add(1, std::memory_order_relaxed);
}

void* allocate(std::size_t size) noexcept
{
    noteAllocation();
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateAligned(std::size_t size, std::align_val_t al) noexcept
{
    noteAllocation();
    std::size_t align = static_cast<std::size_t>(al);
    if (size == 0)
        size = 1;
#ifdef _WIN32
    return _aligned_malloc(size, align);
#else
    if (align < sizeof(void*))
        align = sizeof(void*);
    void* p = nullptr;
    return posix_memalign(&p, align, size) == 0 ? p : nullptr;
#endif
}

// Throwing forms: retry through the new-handler as the standard requires, then throw.
void* allocateOrThrow(std::size_t size)
{
    for (;;)
    {
        if (void* p = allocate(size))
            return p;
        if (auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}

void* allocateAlignedOrThrow(std::size_t size, std::align_val_t al)
{
    for (;;)
    {
        if (void* p = allocateAligned(size, al))
            return p;
        if (auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}

void release(void* p) noexcept
{
    if (p == nullptr)
        return;
    noteAllocation(); // freeing on a guarded thread is just as unsafe as allocating
    std::free(p);
}

void releaseAligned(void* p) noexcept
{
    if (p == nullptr)
        return;
    noteAllocation();
#ifdef _WIN32
    _aligned_free(p);
#else
    std::free(p);
#endif
}
} // namespace

void* operator new(std::size_t s) { return allocateOrThrow(s); }
void* operator new[](std::size_t s) { return allocateOrThrow(s); }
void* operator new(std::size_t s, const std::nothrow_t&) noexcept { return allocate(s); }
void* operator new[](std::size_t s, const std::nothrow_t&) noexcept { return allocate(s); }
void* operator new(std::size_t s, std::align_val_t a) { return allocateAlignedOrThrow(s, a); }
void* operator new[](std::size_t s, std::align_val_t a) { return allocateAlignedOrThrow(s, a); }
void* operator new(std::size_t s, std::align_val_t a, const std::nothrow_t&) noexcept { return allocateAligned(s, a); }
void* operator new[](std::size_t s, std::align_val_t a, const std::nothrow_t&) noexcept { return allocateAligned(s, a); }

void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { releaseAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }

namespace amp::RtSafety
{
ScopedNoAlloc::ScopedNoAlloc() { ++noAllocDepth; }
ScopedNoAlloc::~ScopedNoAlloc() { --noAllocDepth; }
int violations() { return violationCount.load(std::memory_order_relaxed); }
void reset() { violationCount.store(0, std::memory_order_relaxed); }
bool active() { return true; }
} // namespace amp::RtSafety

#endif
