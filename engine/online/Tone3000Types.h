#pragma once

// TONE3000 data types, tolerant JSON parsing (snake_case as the service sends it) and the camelCase bridge shapes
// of the tone-library spec section 7.4.

#include <optional>
#include <vector>

#include <juce_core/juce_core.h>

namespace amp::online
{
struct UserRef
{
    juce::String id, username, displayName, avatarUrl, url;
};

struct ToneSummary
{
    juce::String id, title, gear, format, license, image, url;
    UserRef creator;
    int modelsCount = 0, downloadsCount = 0, favoritesCount = 0;
};

struct ModelInfo
{
    juce::String id, toneId, name, size, architecture /* "1" | "2" | "custom" | "" */, modelUrl;
};

struct Tone : ToneSummary
{
    juce::String description, publishedAt;
    juce::StringArray makes, tags, images;
    std::vector<ModelInfo> models;
};

struct Page
{
    std::vector<ToneSummary> tones;
    int page = 1, totalPages = 1;
};

struct ClientError
{
    enum class Kind
    {
        None,
        NotConfigured,
        NotConnected,
        Network,
        RateLimited,
        NotFound,
        Forbidden,
        Http,
        Parse
    } kind = Kind::None;
    juce::String message;
    int retryAfterSec = 0;

    explicit operator bool() const { return kind != Kind::None; }
};

template <class T>
struct Result
{
    std::optional<T> value;
    ClientError error;
};

// Bridge shapes (camelCase). toVar(Tone) contains everything toVar(ToneSummary) does.
juce::var toVar(const UserRef&);
juce::var toVar(const ToneSummary&);
juce::var toVar(const Tone&);

// Missing or mistyped fields become "" / 0; ids may arrive as numbers or strings and are stored as strings.
UserRef parseUser(const juce::var&);
ToneSummary parseToneSummary(const juce::var&);
Tone parseTone(const juce::var&);
ModelInfo parseModel(const juce::var&);
} // namespace amp::online
