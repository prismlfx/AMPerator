#pragma once

#include <juce_core/juce_core.h>

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
} // namespace amp::online
