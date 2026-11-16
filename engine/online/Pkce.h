#pragma once

// PKCE (RFC 7636) and query-string helpers for the OAuth flow.

#include <juce_core/juce_core.h>

namespace amp::online::pkce
{
// base64url (no padding) of random bytes; 32 bytes -> 43 chars.
juce::String randomUrlSafe(int numBytes);
// base64url(SHA-256(verifier)), RFC 7636 S256.
juce::String challengeFor(const juce::String& verifier);
// k=v&k2=v2, percent-encoded (juce::URL::addEscapeChars, '+' and '/' included).
juce::String encodeQuery(const juce::StringPairArray&);
// Text after '?' (or the whole string if none); decoded; last value wins.
juce::StringPairArray parseQuery(const juce::String& queryOrUrl);
} // namespace amp::online::pkce
