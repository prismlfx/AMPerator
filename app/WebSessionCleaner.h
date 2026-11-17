#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace amp
{
/** Removes the web view data (cookies, local storage, caches) stored for the sites whose name equals
    `domainSuffix` or ends with it (e.g. "tone3000.com"), so the next sign-in window starts signed out. Also clears
    JUCE's cookie store. `done` (optional) is called on the message thread when finished. */
void clearWebSessionFor(const juce::String& domainSuffix, std::function<void()> done = {});

/** Replaces the process-wide URL cache (macOS: NSURLCache.sharedURLCache, which JUCE's NSURLSession transport uses)
    with one that keeps nothing in memory or on disk, so TONE3000 responses and request headers never land in
    ~/Library/Caches/<bundle id>/Cache.db. Call once at startup, before any request. No-op elsewhere. */
void disableSharedUrlCache();
} // namespace amp
