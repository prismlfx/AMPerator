#include "KeychainStore.h"

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace amp::online
{
#if JUCE_MAC
namespace
{
template <typename T>
struct CfRef
{
    explicit CfRef(T r = nullptr) : ref(r) {}
    ~CfRef()
    {
        if (ref != nullptr)
            CFRelease(ref);
    }
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    T ref;
};

CfRef<CFStringRef> cfString(const juce::String& s)
{
    return CfRef<CFStringRef>(CFStringCreateWithCString(kCFAllocatorDefault, s.toRawUTF8(), kCFStringEncodingUTF8));
}

// Base query identifying one item: class + service + account.
CfRef<CFMutableDictionaryRef> baseQuery(const juce::String& service, const juce::String& key)
{
    CfRef<CFMutableDictionaryRef> q(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks));
    auto svc = cfString(service);
    auto acct = cfString(key);
    if (q.ref == nullptr || svc.ref == nullptr || acct.ref == nullptr)
        return CfRef<CFMutableDictionaryRef>(nullptr);
    CFDictionarySetValue(q.ref, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q.ref, kSecAttrService, svc.ref);
    CFDictionarySetValue(q.ref, kSecAttrAccount, acct.ref);
    CFRetain(q.ref); // transfer ownership out of the local wrapper
    return CfRef<CFMutableDictionaryRef>(q.ref);
}
} // namespace

KeychainStore::KeychainStore(juce::String s) : service(std::move(s)) {}

bool KeychainStore::set(const juce::String& key, const juce::String& value)
{
    auto query = baseQuery(service, key);
    if (query.ref == nullptr)
        return false;

    const auto utf8 = value.toRawUTF8();
    CfRef<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8),
                                       (CFIndex) value.getNumBytesAsUTF8()));
    if (data.ref == nullptr)
        return false;

    // Update first; add when the item does not exist yet.
    CfRef<CFMutableDictionaryRef> changes(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(changes.ref, kSecValueData, data.ref);
    const auto status = SecItemUpdate(query.ref, changes.ref);
    if (status == errSecSuccess)
        return true;
    if (status != errSecItemNotFound)
        return false;

    CFDictionarySetValue(query.ref, kSecValueData, data.ref);
    CFDictionarySetValue(query.ref, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
    return SecItemAdd(query.ref, nullptr) == errSecSuccess;
}

std::optional<juce::String> KeychainStore::get(const juce::String& key)
{
    auto query = baseQuery(service, key);
    if (query.ref == nullptr)
        return std::nullopt;
    CFDictionarySetValue(query.ref, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query.ref, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    if (SecItemCopyMatching(query.ref, &result) != errSecSuccess || result == nullptr)
        return std::nullopt;
    CfRef<CFDataRef> data(static_cast<CFDataRef>(result));
    return juce::String::fromUTF8(reinterpret_cast<const char*>(CFDataGetBytePtr(data.ref)),
                                  (int) CFDataGetLength(data.ref));
}

bool KeychainStore::remove(const juce::String& key)
{
    auto query = baseQuery(service, key);
    if (query.ref == nullptr)
        return false;
    const auto status = SecItemDelete(query.ref);
    return status == errSecSuccess || status == errSecItemNotFound;
}
#endif

std::unique_ptr<SecureStore> makePlatformSecureStore(const juce::String& service)
{
#if JUCE_MAC
    return std::make_unique<KeychainStore>(service);
#else
    juce::ignoreUnused(service);
    return std::make_unique<MemorySecureStore>();
#endif
}
} // namespace amp::online
