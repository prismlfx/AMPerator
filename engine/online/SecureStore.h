#pragma once

// Secret storage for OAuth tokens (never written to settings or any file). Implementations are thread-safe.

#include <juce_core/juce_core.h>

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

// macOS: KeychainStore; elsewhere: MemorySecureStore.
std::unique_ptr<SecureStore> makePlatformSecureStore(const juce::String& service);
} // namespace amp::online
