#include "storage/AtomicFile.h"

namespace amp
{
bool writeFileAtomically(const juce::File& target, const juce::String& text,
                         const std::function<bool()>& beforeCommit)
{
    juce::TemporaryFile tmp(target, target.getSiblingFile(target.getFileName() + ".tmp"));
    if (!tmp.getFile().replaceWithText(text))
        return false; // ~TemporaryFile removes any partial temp
    if (beforeCommit && !beforeCommit())
        return false;
    return tmp.overwriteTargetFileWithTemporary();
}
} // namespace amp
