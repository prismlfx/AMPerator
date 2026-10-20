#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace amp
{
/** Writes `text` to a temp sibling (`<name>.tmp`), then atomically replaces `target`.
    `beforeCommit` (optional) runs after the temp write succeeded and before the target is touched;
    returning false aborts, leaving the target and the temp-free directory untouched. */
bool writeFileAtomically(const juce::File& target, const juce::String& text,
                         const std::function<bool()>& beforeCommit = {});
} // namespace amp
