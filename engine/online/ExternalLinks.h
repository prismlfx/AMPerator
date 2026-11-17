#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace amp::online
{
/** The only URLs the app opens in the user's default browser on the page's behalf (the `openExternal` native
    function): the TONE3000 site, "https://www.tone3000.com/..." or the bare root. A case-sensitive prefix match
    on a full "https://www.tone3000.com" followed by "/" or the end of the string, so look-alike hosts
    ("...com.evil.example", "...com@evil.example") and other schemes are refused. */
inline bool isAllowedExternalUrl(const juce::String& url)
{
    static const juce::String site("https://www.tone3000.com");
    return url == site || url.startsWith(site + "/");
}

/** The query of an OAuth callback navigation. Matches only when `url` is `prefix` followed by the end of the string,
    "?" or "#" (so "<prefix>evil", "<prefix>.evil" and "<prefix>/" do not match). The fragment (from the first "#")
    is discarded; the query is the text after a "?" that immediately follows the prefix, empty when there is none. */
inline std::optional<juce::String> callbackQuery(const juce::String& url, const juce::String& prefix)
{
    if (prefix.isEmpty() || ! url.startsWith(prefix))
        return std::nullopt;
    auto rest = url.substring(prefix.length());
    if (rest.isNotEmpty() && rest[0] != '?' && rest[0] != '#')
        return std::nullopt;
    rest = rest.upToFirstOccurrenceOf("#", false, false); // the fragment is never part of the query
    return rest.isEmpty() ? juce::String() : rest.substring(1);
}
} // namespace amp::online
