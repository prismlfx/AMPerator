#include "storage/Settings.h"

namespace amp
{
namespace
{
juce::String readString(const juce::var& obj, const char* key, const juce::String& def)
{
    auto v = obj.getProperty(key, juce::var());
    return v.isString() ? v.toString() : def;
}

int readInt(const juce::var& obj, const char* key, int def)
{
    auto v = obj.getProperty(key, juce::var());
    return (v.isInt() || v.isInt64() || v.isDouble()) ? static_cast<int>(static_cast<double>(v)) : def;
}

double readDouble(const juce::var& obj, const char* key, double def)
{
    auto v = obj.getProperty(key, juce::var());
    return (v.isInt() || v.isInt64() || v.isDouble()) ? static_cast<double>(v) : def;
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
    s.inputChannel = readInt(root, "inputChannel", s.inputChannel);
    s.outputFirstChannel = readInt(root, "outputFirstChannel", s.outputFirstChannel);
    s.sampleRate = readDouble(root, "sampleRate", s.sampleRate);
    s.bufferSize = readInt(root, "bufferSize", s.bufferSize);
    s.watchedFolders = readStrings(root, "watchedFolders");
    s.lastRigId = readString(root, "lastRigId", s.lastRigId);
    s.recentRigIds = readStrings(root, "recentRigIds");
    s.favoriteRigIds = readStrings(root, "favoriteRigIds");
    trimRecent(s.recentRigIds);

    if (auto* arr = root.getProperty("globalMappings", juce::var()).getArray())
        for (auto& m : *arr)
            if (m.getDynamicObject() != nullptr)
                s.globalMappings.push_back({ readString(m, "control", {}), readString(m, "action", {}),
                                             readString(m, "node", {}) });
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

    if (!file.getParentDirectory().createDirectory())
        return false;
    juce::TemporaryFile tmp(file, file.getSiblingFile(file.getFileName() + ".tmp"));
    if (!tmp.getFile().replaceWithText(juce::JSON::toString(root)))
        return false;
    return tmp.overwriteTargetFileWithTemporary();
}

void Settings::noteRecent(const juce::String& rigId)
{
    recentRigIds.removeString(rigId);
    recentRigIds.insert(0, rigId);
    trimRecent(recentRigIds);
}
} // namespace amp
