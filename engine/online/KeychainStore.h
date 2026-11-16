#pragma once

#include "SecureStore.h"

#if JUCE_MAC
namespace amp::online
{
// Generic-password Keychain items: kSecAttrService = service, kSecAttrAccount = key.
class KeychainStore final : public SecureStore
{
public:
    explicit KeychainStore(juce::String service);
    bool set(const juce::String& key, const juce::String& value) override;
    std::optional<juce::String> get(const juce::String& key) override;
    bool remove(const juce::String& key) override;

private:
    juce::String service;
};
} // namespace amp::online
#endif
