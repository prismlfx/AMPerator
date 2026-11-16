#include "Placement.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include "assets/AssetIndex.h"
#include "storage/RigStore.h"

namespace amp::online
{
namespace
{
constexpr int maxChars = 80;
constexpr int maxBytes = 100;
const juce::String separator { juce::CharPointer_UTF8(" \xe2\x80\x94 ") };

// Sanitized, at most 80 characters and 100 UTF-8 bytes, without trailing spaces or dots.
juce::String component(const juce::String& raw)
{
    auto s = RigStore::sanitizeFileName(raw);
    if (s.length() > maxChars)
        s = s.substring(0, maxChars);
    while (s.getNumBytesAsUTF8() > static_cast<size_t>(maxBytes))
        s = s.dropLastCharacters(1);
    s = s.trimCharactersAtEnd(". ");
    return s.isEmpty() ? juce::String("Untitled") : s;
}
} // namespace

juce::String validateAssetFile(const juce::File& file, const juce::String& format)
{
    const auto invalid = "not a valid " + format + " file";
    if (! file.existsAsFile())
        return invalid;

    if (format == "nam")
    {
        const auto root = juce::JSON::parse(file);
        return root.getDynamicObject() != nullptr && root.getProperty("version", {}).isString() ? juce::String()
                                                                                                : invalid;
    }

    for (int i = 0; i < 2; ++i)
    {
        auto stream = std::make_unique<juce::FileInputStream>(file);
        if (! stream->openedOk())
            return invalid;
        std::unique_ptr<juce::AudioFormat> fmt;
        if (i == 0)
            fmt = std::make_unique<juce::WavAudioFormat>();
        else
            fmt = std::make_unique<juce::AiffAudioFormat>();
        // The reader takes the stream when it succeeds; on failure createReaderFor deletes it.
        std::unique_ptr<juce::AudioFormatReader> reader(fmt->createReaderFor(stream.release(), true));
        if (reader != nullptr && reader->numChannels >= 1 && reader->lengthInSamples > 0)
            return {};
    }
    return invalid;
}

juce::File placementFor(const juce::File& libraryRoot, const Provenance& p)
{
    const bool nam = p.format == "nam";
    const auto creator = p.creator.username.trim().isEmpty() ? juce::String("unknown") : component(p.creator.username);
    const auto name = component(p.title) + separator + component(p.modelName) + (nam ? ".nam" : ".wav");
    return libraryRoot.getChildFile(nam ? "Models" : "IRs").getChildFile("TONE3000").getChildFile(creator)
        .getChildFile(name);
}

juce::File placeFile(const juce::File& partial, const juce::File& target, const juce::String& sha256,
                     juce::String& error)
{
    error = {};
    if (! partial.existsAsFile())
    {
        error = "Downloaded file is missing: " + partial.getFullPathName();
        return {};
    }
    const auto dir = target.getParentDirectory();
    if (! dir.createDirectory())
    {
        error = "Cannot create " + dir.getFullPathName();
        return {};
    }

    const auto ext = target.getFileExtension();
    const auto stem = target.getFileName().dropLastCharacters(ext.length());
    for (int n = 1;; ++n)
    {
        const auto candidate = n == 1 ? target : dir.getChildFile(stem + " (" + juce::String(n) + ")" + ext);
        if (candidate.existsAsFile())
        {
            if (AssetIndex::sha256Of(candidate).equalsIgnoreCase(sha256))
            {
                partial.deleteFile();
                return candidate;
            }
            continue;
        }
        if (candidate.exists())
            continue; // a directory or other entry owns the name

        if (partial.moveFileTo(candidate))
            return candidate;
        // Across volumes a move may fail: copy, then remove the partial.
        if (partial.copyFileTo(candidate))
        {
            partial.deleteFile();
            return candidate;
        }
        candidate.deleteFile();
        error = "Cannot write " + candidate.getFullPathName();
        return {};
    }
}
} // namespace amp::online
