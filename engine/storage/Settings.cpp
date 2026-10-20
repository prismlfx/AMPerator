#include "storage/Settings.h"

#include <cmath>
#include <optional>

#include "storage/AtomicFile.h"

namespace amp
{
namespace
{
juce::String readString(const juce::var& obj, const char* key, const juce::String& def)
{
    auto v = obj.getProperty(key, juce::var());
    return v.isString() ? v.toString() : def;
}

bool isNumber(const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }

double readDouble(const juce::var& obj, const char* key, double def, double lo, double hi)
{
    auto v = obj.getProperty(key, juce::var());
    if (!isNumber(v))
        return def;
    double d = static_cast<double>(v);
    return (std::isfinite(d) && d >= lo && d <= hi) ? d : def;
}

int readInt(const juce::var& obj, const char* key, int def, int lo, int hi)
{
    return static_cast<int>(readDouble(obj, key, def, lo, hi));
}

juce::StringArray readStrings(const juce::var& obj, const char* key)
{
    juce::StringArray out;
    auto v = obj.getProperty(key, juce::var());
    if (auto* arr = v.getArray())
        for (auto& item : *arr)
            if (item.isString())
                out.add(item.toString());
    return out;
}

void trimRecent(juce::StringArray& recent)
{
    recent.removeDuplicates(false);
    while (recent.size() > Settings::maxRecent)
        recent.remove(recent.size() - 1);
}

/** "0x04" or "4" -> 4; nullopt unless it is a HID keyboard usage (0x04..0xFF). */
std::optional<int> parseKeycode(const juce::String& text)
{
    auto t = text.trim();
    bool hex = t.startsWithIgnoreCase("0x");
    auto digits = hex ? t.substring(2) : t;
    if (digits.isEmpty() || digits.length() > 4
        || !digits.containsOnly(hex ? "0123456789abcdefABCDEF" : "0123456789"))
        return std::nullopt;
    int code = hex ? digits.getHexValue32() : digits.getIntValue();
    return (code >= 0x04 && code <= 0xff) ? std::optional<int>(code) : std::nullopt;
}

/** Not an object -> defaults. Inside the object, entries with a bad keycode or a non-string/empty control are skipped. */
std::map<int, juce::String> readFootswitchKeys(const juce::var& obj, const std::map<int, juce::String>& def)
{
    auto* keys = obj.getProperty("footswitchKeys", juce::var()).getDynamicObject();
    if (keys == nullptr)
        return def;
    std::map<int, juce::String> out;
    for (auto& prop : keys->getProperties())
        if (auto code = parseKeycode(prop.name.toString()); code && prop.value.isString() && prop.value.toString().isNotEmpty())
            out[*code] = prop.value.toString();
    return out;
}

juce::var toVarArray(const juce::StringArray& strings)
{
    juce::Array<juce::var> arr;
    for (auto& s : strings)
        arr.add(s);
    return arr;
}
} // namespace

Settings Settings::load(const juce::File& file)
{
    Settings s;
    if (!file.existsAsFile())
        return s;

    auto root = juce::JSON::parse(file.loadFileAsString());
    if (!root.getDynamicObject())
        return s;

    s.audioDeviceType = readString(root, "audioDeviceType", s.audioDeviceType);
    s.audioDeviceName = readString(root, "audioDeviceName", s.audioDeviceName);
    s.inputChannel = readInt(root, "inputChannel", s.inputChannel, 0, 1024);
    s.outputFirstChannel = readInt(root, "outputFirstChannel", s.outputFirstChannel, 0, 1024);
    s.sampleRate = readDouble(root, "sampleRate", s.sampleRate, 8000.0, 768000.0);
    s.bufferSize = readInt(root, "bufferSize", s.bufferSize, 16, 8192);
    s.watchedFolders = readStrings(root, "watchedFolders");
    s.lastRigId = readString(root, "lastRigId", s.lastRigId);
    s.recentRigIds = readStrings(root, "recentRigIds");
    s.favoriteRigIds = readStrings(root, "favoriteRigIds");
    trimRecent(s.recentRigIds);
    s.footswitchKeys = readFootswitchKeys(root, s.footswitchKeys);

    if (auto* arr = root.getProperty("globalMappings", juce::var()).getArray())
        for (auto& m : *arr)
        {
            if (m.getDynamicObject() == nullptr)
                continue;
            Mapping mm { readString(m, "control", {}), readString(m, "action", {}), readString(m, "node", {}) };
            if (mm.control.isNotEmpty() && mm.action.isNotEmpty())
                s.globalMappings.push_back(std::move(mm));
        }
    return s;
}

bool Settings::save(const juce::File& file) const
{
    auto* obj = new juce::DynamicObject();
    juce::var root(obj);
    obj->setProperty("audioDeviceType", audioDeviceType);
    obj->setProperty("audioDeviceName", audioDeviceName);
    obj->setProperty("inputChannel", inputChannel);
    obj->setProperty("outputFirstChannel", outputFirstChannel);
    obj->setProperty("sampleRate", sampleRate);
    obj->setProperty("bufferSize", bufferSize);
    obj->setProperty("watchedFolders", toVarArray(watchedFolders));
    obj->setProperty("lastRigId", lastRigId);
    obj->setProperty("recentRigIds", toVarArray(recentRigIds));
    obj->setProperty("favoriteRigIds", toVarArray(favoriteRigIds));

    juce::Array<juce::var> maps;
    for (auto& m : globalMappings)
    {
        auto* mo = new juce::DynamicObject();
        mo->setProperty("control", m.control);
        mo->setProperty("action", m.action);
        mo->setProperty("node", m.node);
        maps.add(juce::var(mo));
    }
    obj->setProperty("globalMappings", maps);

    auto* keys = new juce::DynamicObject();
    for (auto& [code, control] : footswitchKeys)
        keys->setProperty("0x" + juce::String::toHexString(code).paddedLeft('0', 2), control);
    obj->setProperty("footswitchKeys", juce::var(keys));

    if (!file.getParentDirectory().createDirectory())
        return false;
    return writeFileAtomically(file, juce::JSON::toString(root));
}

void Settings::noteRecent(const juce::String& rigId)
{
    recentRigIds.removeString(rigId);
    recentRigIds.insert(0, rigId);
    trimRecent(recentRigIds);
}
} // namespace amp
