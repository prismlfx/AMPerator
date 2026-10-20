#pragma once

namespace amp::RtSafety
{
// Debug-build allocation guard for the audio thread. While a ScopedNoAlloc is alive on a thread, every global
// operator new/delete call on that thread (all forms: array, nothrow, aligned, sized) is counted as a violation.
// Violations are only counted, never aborted on. Scopes nest. Release builds (NDEBUG) compile all of this to
// no-ops and violations() is always 0.
struct ScopedNoAlloc
{
    ScopedNoAlloc();
    ~ScopedNoAlloc();
    ScopedNoAlloc(const ScopedNoAlloc&) = delete;
    ScopedNoAlloc& operator=(const ScopedNoAlloc&) = delete;
};

int violations(); // total since the last reset(), across all threads
void reset();

// True when the guard is compiled in (debug builds).
bool active();
} // namespace amp::RtSafety
