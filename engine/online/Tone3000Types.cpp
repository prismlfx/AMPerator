#include "Tone3000Types.h"

#include <cmath>

namespace amp::online
{
namespace
{
// Strings pass through; integers print without a decimal point; anything else is "".
juce::String text(const juce::var& v)
{
    if (v.isString())
        return v.toString();
    if (v.isInt() || v.isInt64())
        return juce::String(static_cast<juce::int64>(v));
    if (v.isDouble())
    {
        const double d = v;
        return d == std::floor(d) ? juce::String(static_cast<juce::int64>(d)) : juce::String(d);
    }
    return {};
}

juce::String field(const juce::var& obj, const char* name) { return text(obj.getProperty(name, {})); }

int count(const juce::var& obj, const char* name)
{
    const auto v = obj.getProperty(name, {});
    return v.isInt() || v.isInt64() || v.isDouble() ? static_cast<int>(static_cast<juce::int64>(v)) : 0;
}

// An array of strings or of {name} objects.
juce::StringArray names(const juce::var& v)
{
    juce::StringArray out;
    if (auto* arr = v.getArray())
        for (const auto& item : *arr)
        {
            const auto s = item.isObject() ? field(item, "name") : text(item);
            if (s.isNotEmpty())
                out.add(s);
        }
    return out;
}

juce::StringArray strings(const juce::var& v)
{
    juce::StringArray out;
    if (auto* arr = v.getArray())
        for (const auto& item : *arr)
            if (item.isString() && item.toString().isNotEmpty())
                out.add(item.toString());
    return out;
}

juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (const auto& p : props)
        o->setProperty(p.first, p.second);
    return juce::var(o);
}

juce::var stringArrayVar(const juce::StringArray& list)
{
    juce::Array<juce::var> out;
    for (const auto& s : list)
        out.add(s);
    return out;
}
} // namespace

juce::var toVar(const UserRef& u)
{
    return obj({ { "username", u.username },
                 { "displayName", u.displayName },
                 { "avatarUrl", u.avatarUrl },
                 { "url", u.url } });
}

juce::var toVar(const ToneSummary& t)
{
    return obj({ { "id", t.id },
                 { "title", t.title },
                 { "gear", t.gear },
                 { "format", t.format },
                 { "license", t.license },
                 { "creator", toVar(t.creator) },
                 { "image", t.image },
                 { "url", t.url },
                 { "modelsCount", t.modelsCount },
                 { "downloadsCount", t.downloadsCount },
                 { "favoritesCount", t.favoritesCount } });
}

juce::var toVar(const Tone& t)
{
    auto result = toVar(static_cast<const ToneSummary&>(t));
    auto* o = result.getDynamicObject();
    o->setProperty("description", t.description);
    o->setProperty("makes", stringArrayVar(t.makes));
    o->setProperty("tags", stringArrayVar(t.tags));
    o->setProperty("images", stringArrayVar(t.images));
    o->setProperty("publishedAt", t.publishedAt);

    juce::Array<juce::var> models;
    for (const auto& m : t.models)
        models.add(obj({ { "id", m.id }, { "name", m.name }, { "size", m.size }, { "architecture", m.architecture } }));
    o->setProperty("models", models);
    return result;
}

UserRef parseUser(const juce::var& v)
{
    UserRef u;
    u.id = field(v, "id");
    u.username = field(v, "username");
    u.displayName = field(v, "display_name");
    u.avatarUrl = field(v, "avatar_url");
    u.url = field(v, "url");
    return u;
}

ToneSummary parseToneSummary(const juce::var& v)
{
    ToneSummary t;
    t.id = field(v, "id");
    t.title = field(v, "title");
    t.gear = field(v, "gear");
    t.format = field(v, "format");
    if (t.format.isEmpty())
        t.format = field(v, "platform");
    t.license = field(v, "license");
    const auto images = strings(v.getProperty("images", {}));
    t.image = images.isEmpty() ? juce::String() : images[0];
    t.url = field(v, "url");
    t.creator = parseUser(v.getProperty("user", {}));
    t.modelsCount = count(v, "models_count");
    t.downloadsCount = count(v, "downloads_count");
    t.favoritesCount = count(v, "favorites_count");
    return t;
}

Tone parseTone(const juce::var& v)
{
    Tone t;
    static_cast<ToneSummary&>(t) = parseToneSummary(v);
    t.description = field(v, "description");
    t.publishedAt = field(v, "published_at");
    t.makes = names(v.getProperty("makes", {}));
    t.tags = names(v.getProperty("tags", {}));
    t.images = strings(v.getProperty("images", {}));
    return t;
}

ModelInfo parseModel(const juce::var& v)
{
    ModelInfo m;
    m.id = field(v, "id");
    m.toneId = field(v, "tone_id");
    m.name = field(v, "name");
    m.size = field(v, "size");
    m.architecture = field(v, "architecture_version");
    m.modelUrl = field(v, "model_url");
    return m;
}
} // namespace amp::online
