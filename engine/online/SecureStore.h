#pragma once

// Secret storage for OAuth tokens (never written to settings or any file). Implementations are thread-safe.

#include <juce_core/juce_core.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>

namespace amp::online
{
class SecureStore
{
public:
    virtual ~SecureStore() = default;
    virtual bool set(const juce::String& key, const juce::String& value) = 0;
    virtual std::optional<juce::String> get(const juce::String& key) = 0;
    virtual bool remove(const juce::String& key) = 0;
};

// Process-lifetime only; used for tests and platforms without a native store.
class MemorySecureStore final : public SecureStore
{
public:
    bool set(const juce::String& key, const juce::String& value) override
    {
        const juce::ScopedLock lock(cs);
        values[key] = value;
        return true;
    }
    std::optional<juce::String> get(const juce::String& key) override
    {
        const juce::ScopedLock lock(cs);
        auto it = values.find(key);
        if (it == values.end())
            return std::nullopt;
        return it->second;
    }
    bool remove(const juce::String& key) override
    {
        const juce::ScopedLock lock(cs);
        values.erase(key);
        return true;
    }

private:
    juce::CriticalSection cs;
    std::map<juce::String, juce::String> values;
};

// Writes to `primary` (the platform store) until it refuses one; from then on keeps every value in memory for the
// rest of the session and remembered() is false (tone-library spec section 11: "sign-in will not be remembered").
// get() prefers the in-memory value; remove() clears both. Does not own `primary`.
class FallbackSecureStore final : public SecureStore
{
public:
    explicit FallbackSecureStore(SecureStore& primaryStore) : primary(primaryStore) {}

    bool set(const juce::String& key, const juce::String& value) override
    {
        if (! fellBack.load())
        {
            if (primary.set(key, value))
                return true;
            fellBack = true;
        }
        return memory.set(key, value);
    }
    std::optional<juce::String> get(const juce::String& key) override
    {
        if (auto v = memory.get(key))
            return v;
        return primary.get(key);
    }
    bool remove(const juce::String& key) override
    {
        const bool fromMemory = memory.remove(key);
        return primary.remove(key) && fromMemory;
    }

    bool remembered() const { return ! fellBack.load(); }

private:
    SecureStore& primary;
    MemorySecureStore memory;
    std::atomic<bool> fellBack { false };
};

// macOS: KeychainStore; elsewhere: MemorySecureStore.
std::unique_ptr<SecureStore> makePlatformSecureStore(const juce::String& service);
} // namespace amp::online
