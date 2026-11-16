#include <catch2/catch_test_macros.hpp>

#include "online/KeychainStore.h"
#include "online/SecureStore.h"

using namespace amp::online;

TEST_CASE("online_secure_store: memory store set/get/remove")
{
    MemorySecureStore store;
    REQUIRE_FALSE(store.get("k").has_value());
    REQUIRE(store.set("k", "v1"));
    REQUIRE(store.get("k") == juce::String("v1"));
    REQUIRE(store.set("k", "v2")); // overwrite
    REQUIRE(store.get("k") == juce::String("v2"));
    REQUIRE(store.remove("k"));
    REQUIRE_FALSE(store.get("k").has_value());
}

#if JUCE_MAC
TEST_CASE("online_secure_store: keychain round trip")
{
    // Unique throwaway service: never touches any other Keychain item.
    KeychainStore store("com.amperator.tests." + juce::Uuid().toString());
    const juce::String key = "account-1";
    const juce::String value = juce::String::fromUTF8("tok\xc3\xa9n-\xe2\x82\xac-123");

    if (! store.set(key, value))
        SKIP("Keychain unavailable");

    struct Cleanup
    {
        KeychainStore& s;
        juce::String k;
        ~Cleanup() { s.remove(k); }
    } cleanup{store, key};

    REQUIRE(store.get(key) == value);
    REQUIRE(store.set(key, "second")); // update path
    REQUIRE(store.get(key) == juce::String("second"));
    REQUIRE(store.remove(key));
    REQUIRE_FALSE(store.get(key).has_value());
}
#endif

TEST_CASE("online_secure_store: makePlatformSecureStore returns a working store")
{
    auto store = makePlatformSecureStore("com.amperator.tests." + juce::Uuid().toString());
    REQUIRE(store != nullptr);
    if (! store->set("k", "v"))
        SKIP("secure store unavailable");
    REQUIRE(store->get("k") == juce::String("v"));
    REQUIRE(store->remove("k"));
}
