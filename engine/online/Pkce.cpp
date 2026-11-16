#include "Pkce.h"

#include <juce_cryptography/juce_cryptography.h>

namespace amp::online::pkce
{
// juce::MemoryBlock::toBase64Encoding is JUCE's own length-prefixed dialect, not RFC 4648, so encode by hand.
static juce::String base64Url(const void* data, size_t size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const auto* in = static_cast<const juce::uint8*>(data);
    juce::String out;
    for (size_t i = 0; i < size; i += 3)
    {
        const size_t n = juce::jmin<size_t>(3, size - i);
        const juce::uint32 v = (juce::uint32(in[i]) << 16) | (n > 1 ? juce::uint32(in[i + 1]) << 8 : 0)
                             | (n > 2 ? juce::uint32(in[i + 2]) : 0);
        out << alphabet[(v >> 18) & 63] << alphabet[(v >> 12) & 63];
        if (n > 1)
            out << alphabet[(v >> 6) & 63];
        if (n > 2)
            out << alphabet[v & 63];
    }
    return out;
}

juce::String randomUrlSafe(int numBytes)
{
    juce::MemoryBlock bytes(static_cast<size_t>(juce::jmax(0, numBytes)));
    auto& rng = juce::Random::getSystemRandom();
    for (size_t i = 0; i < bytes.getSize(); ++i)
        bytes[i] = static_cast<char>(rng.nextInt(256));
    return base64Url(bytes.getData(), bytes.getSize());
}

juce::String challengeFor(const juce::String& verifier)
{
    const auto utf8 = verifier.toUTF8();
    juce::SHA256 hash(utf8.getAddress(), utf8.sizeInBytes() - 1);
    const auto raw = hash.getRawData();
    return base64Url(raw.getData(), raw.getSize());
}

juce::String encodeQuery(const juce::StringPairArray& params)
{
    juce::StringArray parts;
    const auto& keys = params.getAllKeys();
    const auto& values = params.getAllValues();
    for (int i = 0; i < keys.size(); ++i)
        parts.add(juce::URL::addEscapeChars(keys[i], true, true) + "=" + juce::URL::addEscapeChars(values[i], true, true));
    return parts.joinIntoString("&");
}

juce::StringPairArray parseQuery(const juce::String& queryOrUrl)
{
    juce::StringPairArray out;
    auto text = queryOrUrl.contains("?") ? queryOrUrl.fromFirstOccurrenceOf("?", false, false) : queryOrUrl;
    text = text.upToFirstOccurrenceOf("#", false, false);
    for (const auto& pair : juce::StringArray::fromTokens(text, "&", ""))
    {
        if (pair.isEmpty())
            continue;
        const auto key = pair.upToFirstOccurrenceOf("=", false, false);
        const auto value = pair.contains("=") ? pair.fromFirstOccurrenceOf("=", false, false) : juce::String();
        out.set(juce::URL::removeEscapeChars(key.replaceCharacter('+', ' ')),
                juce::URL::removeEscapeChars(value.replaceCharacter('+', ' ')));
    }
    return out;
}
} // namespace amp::online::pkce
