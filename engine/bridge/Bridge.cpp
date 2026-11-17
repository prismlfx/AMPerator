#include "bridge/Bridge.h"

#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

#include "Engine.h"
#include "model/ChainGraph.h"

namespace amp
{
namespace
{
// A missing or malformed argument; answered with ok:false and the message.
struct ArgError : std::runtime_error
{
    explicit ArgError(const juce::String& m) : std::runtime_error(m.toStdString()) {}
};

bool isNumber(const juce::var& v) { return v.isInt() || v.isInt64() || v.isDouble(); }

// juce::var::isObject() is also true for arrays; a JSON object is a DynamicObject.
bool isJsonObject(const juce::var& v) { return v.getDynamicObject() != nullptr; }

juce::String quoted(const char* key) { return "\"" + juce::String(key) + "\""; }

juce::String str(const juce::var& args, const char* key)
{
    const auto v = args[key];
    if (! v.isString())
        throw ArgError("missing or invalid string argument " + quoted(key));
    return v.toString();
}

// Ids and parameter names must not be empty (they become juce::Identifiers / map keys).
juce::String nonEmpty(const juce::var& args, const char* key)
{
    auto s = str(args, key);
    if (s.isEmpty())
        throw ArgError("argument " + quoted(key) + " must not be empty");
    return s;
}

std::optional<juce::String> optStr(const juce::var& args, const char* key)
{
    if (! args.hasProperty(key))
        return std::nullopt;
    return str(args, key);
}

double number(const juce::var& args, const char* key)
{
    const auto v = args[key];
    if (v.isBool())
        return static_cast<bool>(v) ? 1.0 : 0.0;
    if (! isNumber(v) || ! std::isfinite(static_cast<double>(v)))
        throw ArgError("missing or invalid number argument " + quoted(key));
    return static_cast<double>(v);
}

int integer(const juce::var& args, const char* key)
{
    const auto v = args[key];
    const double d = isNumber(v) ? static_cast<double>(v) : NAN;
    if (! std::isfinite(d) || ! juce::exactlyEqual(d, std::floor(d)) || std::abs(d) > 1.0e9)
        throw ArgError("missing or invalid integer argument " + quoted(key));
    return static_cast<int>(d);
}

bool boolean(const juce::var& args, const char* key)
{
    const auto v = args[key];
    if (v.isBool())
        return static_cast<bool>(v);
    if (isNumber(v))
        return ! juce::exactlyEqual(static_cast<double>(v), 0.0);
    throw ArgError("missing or invalid boolean argument " + quoted(key));
}

juce::StringArray stringList(const juce::var& args, const char* key)
{
    auto* arr = args[key].getArray();
    if (arr == nullptr)
        throw ArgError("argument " + quoted(key) + " must be an array of strings");
    juce::StringArray out;
    for (auto& item : *arr)
    {
        if (! item.isString())
            throw ArgError("argument " + quoted(key) + " must be an array of strings");
        out.add(item.toString());
    }
    return out;
}

std::vector<Mapping> mappingList(const juce::var& args, const char* key)
{
    auto* arr = args[key].getArray();
    if (arr == nullptr)
        throw ArgError("argument " + quoted(key) + " must be an array of {control, node} objects");
    std::vector<Mapping> out;
    for (auto& m : *arr)
    {
        if (! isJsonObject(m))
            throw ArgError("argument " + quoted(key) + " must be an array of {control, node} objects");
        Mapping mm { nonEmpty(m, "control"), optStr(m, "action").value_or("toggleBypass"), nonEmpty(m, "node") };
        if (mm.action != "toggleBypass")
            throw ArgError("unsupported mapping action: " + mm.action);
        out.push_back(std::move(mm));
    }
    return out;
}

/** "0x04" or "4" -> HID keyboard usage 0x04..0xFF. */
std::optional<int> keycode(const juce::String& text)
{
    const auto t = text.trim();
    const bool hex = t.startsWithIgnoreCase("0x");
    const auto digits = hex ? t.substring(2) : t;
    if (digits.isEmpty() || digits.length() > 4 || ! digits.containsOnly(hex ? "0123456789abcdefABCDEF" : "0123456789"))
        return std::nullopt;
    const int code = hex ? digits.getHexValue32() : digits.getIntValue();
    return (code >= 0x04 && code <= 0xff) ? std::optional<int>(code) : std::nullopt;
}

std::map<int, juce::String> keyMap(const juce::var& args, const char* key)
{
    auto* obj = args[key].getDynamicObject();
    if (obj == nullptr)
        throw ArgError("argument " + quoted(key) + " must be an object of keycode -> control");
    std::map<int, juce::String> out;
    for (auto& p : obj->getProperties())
    {
        const auto code = keycode(p.name.toString());
        if (! code || ! p.value.isString() || p.value.toString().isEmpty())
            throw ArgError("invalid footswitch key entry: " + p.name.toString());
        out[*code] = p.value.toString();
    }
    return out;
}

SettingsPatch settingsPatch(const juce::var& a)
{
    SettingsPatch p;
    p.audioDeviceType = optStr(a, "audioDeviceType");
    p.audioDeviceName = optStr(a, "audioDeviceName");
    auto optInt = [&](const char* k, int lo) -> std::optional<int>
    {
        if (! a.hasProperty(k))
            return std::nullopt;
        const int v = integer(a, k);
        if (v < lo)
            throw ArgError("argument " + quoted(k) + " out of range");
        return v;
    };
    p.inputChannel = optInt("inputChannel", 0);
    p.outputFirstChannel = optInt("outputFirstChannel", 0);
    p.bufferSize = optInt("bufferSize", 1);
    if (a.hasProperty("sampleRate"))
    {
        p.sampleRate = number(a, "sampleRate");
        if (*p.sampleRate <= 0.0)
            throw ArgError("argument \"sampleRate\" out of range");
    }
    if (a.hasProperty("watchedFolders"))
    {
        p.watchedFolders = stringList(a, "watchedFolders");
        for (auto& f : *p.watchedFolders)
            if (! juce::File::isAbsolutePath(f))
                throw ArgError("watched folder must be an absolute path: " + f);
    }
    if (a.hasProperty("globalMappings"))
        p.globalMappings = mappingList(a, "globalMappings");
    if (a.hasProperty("footswitchKeys"))
        p.footswitchKeys = keyMap(a, "footswitchKeys");
    if (a.hasProperty("footswitchDevice"))
    {
        const auto d = a["footswitchDevice"];
        if (d.isVoid())
            p.footswitchDevice = std::optional<UsbIds>(); // null clears
        else
        {
            if (! isJsonObject(d))
                throw ArgError("argument \"footswitchDevice\" must be {vid, pid} or null");
            const int vid = integer(d, "vid"), pid = integer(d, "pid");
            if (vid < 0 || vid > 0xffff || pid < 0 || pid > 0xffff)
                throw ArgError("argument \"footswitchDevice\" out of range (vid and pid are 16-bit USB ids)");
            p.footswitchDevice = std::optional<UsbIds>(UsbIds { (uint16_t) vid, (uint16_t) pid });
        }
    }
    if (a.hasProperty("tone3000AutoFetch"))
        p.tone3000AutoFetch = boolean(a, "tone3000AutoFetch");
    if (a.hasProperty("tone3000ClientId"))
        p.tone3000ClientId = str(a, "tone3000ClientId").trim();
    return p;
}

using Handler = std::function<CommandResult(Engine&, const juce::var& args)>;

const std::map<juce::String, Handler>& commands()
{
    static const std::map<juce::String, Handler> table {
        { "getState", [](Engine& e, const juce::var&) { return e.getState(); } },
        { "listRigs", [](Engine& e, const juce::var&) { return e.listRigs(); } },
        { "loadRig", [](Engine& e, const juce::var& a) { return e.loadRig(nonEmpty(a, "id")); } },
        { "newRig", [](Engine& e, const juce::var& a)
          { return e.newRig(optStr(a, "name").value_or("New Rig"), optStr(a, "band").value_or("My Tones")); } },
        { "saveRig", [](Engine& e, const juce::var& a)
          {
              return e.saveRig(optStr(a, "name").value_or(""), optStr(a, "band").value_or(""),
                               a.hasProperty("asNew") && boolean(a, "asNew"));
          } },
        { "confirmQuit", [](Engine& e, const juce::var& a) { return e.confirmQuit(str(a, "action")); } },
        { "deleteRig", [](Engine& e, const juce::var& a) { return e.deleteRig(nonEmpty(a, "id")); } },
        { "nextRig", [](Engine& e, const juce::var&) { return e.stepRig(+1); } },
        { "prevRig", [](Engine& e, const juce::var&) { return e.stepRig(-1); } },
        { "addNode", [](Engine& e, const juce::var& a)
          {
              const auto typeName = str(a, "type");
              const auto type = json::parseNodeType(typeName);
              if (! type)
                  throw ArgError("unknown node type: " + typeName);
              const auto plugin = a["plugin"];
              if (! plugin.isVoid() && ! isJsonObject(plugin) && ! plugin.isString())
                  throw ArgError("argument \"plugin\" must be a plugin object or uid string");
              return e.addNode(*type, integer(a, "index"), plugin, optStr(a, "asset").value_or(""));
          } },
        { "moveNode", [](Engine& e, const juce::var& a) { return e.moveNode(nonEmpty(a, "id"), integer(a, "index")); } },
        { "removeNode", [](Engine& e, const juce::var& a) { return e.removeNode(nonEmpty(a, "id")); } },
        { "setParam", [](Engine& e, const juce::var& a)
          { return e.setParam(nonEmpty(a, "id"), nonEmpty(a, "name"), static_cast<float>(number(a, "value"))); } },
        { "toggleBypass", [](Engine& e, const juce::var& a) { return e.toggleBypass(nonEmpty(a, "id")); } },
        { "setAsset", [](Engine& e, const juce::var& a) { return e.setAsset(nonEmpty(a, "id"), str(a, "file")); } },
        { "learnMapping", [](Engine& e, const juce::var& a) { return e.learnMapping(nonEmpty(a, "id")); } },
        { "assignFootswitchKey", [](Engine& e, const juce::var& a) { return e.assignFootswitchKey(nonEmpty(a, "control")); } },
        { "cancelAssignFootswitchKey", [](Engine& e, const juce::var&) { return e.cancelAssignFootswitchKey(); } },
        { "cancelLearn", [](Engine& e, const juce::var&) { return e.cancelLearn(); } },
        { "clearMapping", [](Engine& e, const juce::var& a) { return e.clearMapping(nonEmpty(a, "control")); } },
        { "listAssets", [](Engine& e, const juce::var& a) { return e.listAssets(str(a, "kind")); } },
        { "listPlugins", [](Engine& e, const juce::var&) { return e.listPlugins(); } },
        { "rescanPlugins", [](Engine& e, const juce::var&) { return e.rescanPlugins(); } },
        { "listBlacklisted", [](Engine& e, const juce::var&) { return e.listBlacklisted(); } },
        { "unblacklistPlugin", [](Engine& e, const juce::var& a) { return e.unblacklistPlugin(nonEmpty(a, "id")); } },
        { "listHidKeyboards", [](Engine& e, const juce::var&) { return e.listHidKeyboards(); } },
        { "openPluginEditor", [](Engine& e, const juce::var& a) { return e.openPluginEditor(nonEmpty(a, "id")); } },
        { "getSettings", [](Engine& e, const juce::var&) { return e.getSettings(); } },
        { "setSettings", [](Engine& e, const juce::var& a) { return e.setSettings(settingsPatch(a)); } },
        { "setTuner", [](Engine& e, const juce::var& a) { return e.setTuner(boolean(a, "on")); } },
        { "toggleFavorite", [](Engine& e, const juce::var& a) { return e.toggleFavorite(nonEmpty(a, "id")); } },
        { "setTags", [](Engine& e, const juce::var& a) { return e.setTags(stringList(a, "tags")); } },
        { "listTags", [](Engine& e, const juce::var&) { return e.listTags(); } },
        { "restoreBackup", [](Engine& e, const juce::var& a) { return e.restoreBackup(nonEmpty(a, "id")); } },
        { "setNodeArt", [](Engine& e, const juce::var& a)
          { return e.setNodeArt(nonEmpty(a, "id"), optStr(a, "archetype"), optStr(a, "imageFile")); } },
        { "tone3000Status", [](Engine& e, const juce::var&) { return e.tone3000Status(); } },
        { "tone3000Connect", [](Engine& e, const juce::var&) { return e.tone3000Connect(); } },
        { "tone3000Disconnect", [](Engine& e, const juce::var&) { return e.tone3000Disconnect(); } },
        { "tone3000Browse", [](Engine& e, const juce::var& a)
          {
              const auto format = optStr(a, "format").value_or("");
              if (format.isNotEmpty() && format != "nam" && format != "ir")
                  throw ArgError("argument \"format\" must be \"nam\" or \"ir\"");
              return e.tone3000Browse(format, optStr(a, "nodeId").value_or(""), optStr(a, "loadTone").value_or(""),
                                      a.hasProperty("replace") && boolean(a, "replace"));
          } },
        { "tone3000List", [](Engine& e, const juce::var& a)
          {
              static const juce::StringArray lists { "favorited", "downloaded", "created", "trending", "latest" };
              const auto list = str(a, "list");
              if (! lists.contains(list))
                  throw ArgError("unknown list: " + list
                                 + " (expected favorited, downloaded, created, trending or latest)");
              const int page = a.hasProperty("page") ? integer(a, "page") : 1;
              if (page < 1)
                  throw ArgError("argument \"page\" must be 1 or more");
              return e.tone3000List(list, page);
          } },
        { "tone3000Tone", [](Engine& e, const juce::var& a) { return e.tone3000Tone(nonEmpty(a, "id")); } },
        { "tone3000Download", [](Engine& e, const juce::var& a)
          {
              const auto addNode = a["addNode"];
              if (! addNode.isVoid())
              {
                  if (! isJsonObject(addNode))
                      throw ArgError("argument \"addNode\" must be an object {type, index}");
                  const auto type = str(addNode, "type");
                  if (type != "NamAmp" && type != "CabIR")
                      throw ArgError("addNode type must be \"NamAmp\" or \"CabIR\"");
                  integer(addNode, "index");
              }
              return e.tone3000Download(nonEmpty(a, "modelId"), optStr(a, "toneId").value_or(""),
                                        optStr(a, "nodeId").value_or(""), addNode);
          } },
        { "tone3000Fetch", [](Engine& e, const juce::var& a)
          { return e.tone3000Fetch(a.hasProperty("nodeIds") ? stringList(a, "nodeIds") : juce::StringArray()); } },
        { "tone3000Cancel", [](Engine& e, const juce::var& a) { return e.tone3000Cancel(nonEmpty(a, "jobId")); } },
    };
    return table;
}
} // namespace

Bridge::Bridge(Engine& e) : engine(e) {}

Bridge::~Bridge() { *alive = false; }

Bridge::Completion Bridge::deferReply()
{
    jassert(current.has_value()); // only a running handler may defer its reply
    if (! current)
        return [](CommandResult) {};
    if (current->replied == nullptr)
        current->replied = std::make_shared<bool>(false);
    return [this, aliveFlag = alive, id = current->id, replied = current->replied](CommandResult r)
    {
        JUCE_ASSERT_MESSAGE_THREAD
        if (*replied || ! *aliveFlag)
            return;
        *replied = true;
        if (r.pending) // a completion cannot defer again
            r = CommandResult::failure("internal error: deferred reply completed with a pending result");
        replyWith(id, r);
    };
}

void Bridge::replyWith(const juce::var& id, const CommandResult& r)
{
    if (r.ok)
        reply(id, true, r.result.isVoid() ? json::object() : r.result);
    else
        reply(id, false, r.error);
}

void Bridge::handle(const juce::var& message)
{
    const juce::var id = isJsonObject(message) ? message["id"] : juce::var();

    // While the handler runs, deferReply() binds to this message; restored afterwards (a handler may re-enter handle).
    auto outer = std::exchange(current, Handling { id, nullptr });
    struct Restore
    {
        Bridge& b;
        std::optional<Handling>& outer;
        ~Restore()
        {
            // A completion handed out by a handler that then replied (or threw) is spent: handle() replied.
            if (b.current && b.current->replied)
                *b.current->replied = true;
            b.current = std::move(outer);
        }
    } restore { *this, outer };
    // True once a completion the handler took has replied (handle() must not reply a second time).
    auto alreadyReplied = [this] { return current->replied != nullptr && *current->replied; };

    try
    {
        if (! isJsonObject(message))
            return reply(id, false, "message must be a JSON object");

        const auto v = message["v"];
        if (! isNumber(v) || ! juce::exactlyEqual(static_cast<double>(v), (double) protocolVersion))
            return reply(id, false, "unsupported protocol version " + (v.isVoid() ? juce::String("(missing)") : v.toString())
                                        + "; this engine speaks version " + juce::String(protocolVersion));

        if (! message["cmd"].isString())
            return reply(id, false, "missing or invalid \"cmd\"");
        const auto cmd = message["cmd"].toString();

        auto args = message["args"];
        if (args.isVoid())
            args = json::object();
        else if (! isJsonObject(args))
            return reply(id, false, "\"args\" must be an object");

        const auto& table = commands();
        const auto it = table.find(cmd);
        if (it == table.end())
            return reply(id, false, "unknown command: " + cmd);

        const auto r = it->second(engine, args);
        if (r.pending)
        {
            if (current->replied == nullptr)
                return reply(id, false, "internal error: deferred without a completion");
            current->replied = nullptr; // the completion replies; keep it live past this handle()
            return;
        }
        if (alreadyReplied())
            return; // the handler already replied through a completion
        replyWith(id, r);
    }
    catch (const ArgError& e)
    {
        if (! alreadyReplied())
            reply(id, false, juce::String(e.what()));
    }
    catch (const std::exception& e)
    {
        if (! alreadyReplied())
            reply(id, false, "internal error: " + juce::String(e.what()));
    }
}

void Bridge::flushPending()
{
    auto queued = std::move(pending);
    pending.clear();
    for (auto& m : queued)
        send(m);
}

void Bridge::reply(const juce::var& id, bool ok, const juce::var& payload)
{
    if (! send)
        return;
    flushPending();
    send(json::object({ { "v", protocolVersion }, { "reply", id }, { "ok", ok }, { ok ? "result" : "error", payload } }));
}

void Bridge::emit(const juce::String& name, const juce::var& data)
{
    auto message = json::object({ { "v", protocolVersion }, { "event", name }, { "data", data } });
    if (! send)
    {
        if (name == "error") // e.g. startup problems before the UI attached: delivered with the next message
            pending.push_back(message);
        return;
    }
    flushPending();
    send(message);
}

namespace json
{
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props)
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : props)
        o->setProperty(k, v);
    return juce::var(o);
}

juce::var strings(const juce::StringArray& s)
{
    juce::Array<juce::var> arr;
    for (auto& x : s)
        arr.add(x);
    return arr;
}

juce::var mappings(const std::vector<Mapping>& ms)
{
    juce::Array<juce::var> arr;
    for (auto& m : ms)
        arr.add(object({ { "control", m.control }, { "action", m.action }, { "node", m.node } }));
    return arr;
}

juce::var pluginRef(const PluginRef& p)
{
    return object({ { "format", p.format }, { "uid", p.uid }, { "name", p.name }, { "vendor", p.vendor } });
}

juce::var report(const LoadReport& r)
{
    return object({ { "missingAssets", strings(r.missingAssets) },
                    { "missingPlugins", strings(r.missingPlugins) },
                    { "placeholderNodeIds", strings(r.placeholderNodeIds) },
                    { "resolvedByFilename", strings(r.resolvedByFilename) },
                    { "fetchable", strings(r.fetchable) } });
}

juce::var asset(const AssetEntry& e)
{
    return object({ { "file", e.file.getFullPathName() },
                    { "name", e.file.getFileNameWithoutExtension() },
                    { "kind", e.kind },
                    { "sha256", e.sha256 } });
}

juce::var settings(const Settings& s)
{
    auto keys = object();
    for (auto& [code, control] : s.footswitchKeys)
        keys.getDynamicObject()->setProperty("0x" + juce::String::toHexString(code).paddedLeft('0', 2), control);
    auto device = juce::var();
    if (s.footswitchDevice)
        device = object({ { "vid", (int) s.footswitchDevice->first }, { "pid", (int) s.footswitchDevice->second } });
    return object({ { "audioDeviceType", s.audioDeviceType },
                    { "audioDeviceName", s.audioDeviceName },
                    { "inputChannel", s.inputChannel },
                    { "outputFirstChannel", s.outputFirstChannel },
                    { "sampleRate", s.sampleRate },
                    { "bufferSize", s.bufferSize },
                    { "watchedFolders", strings(s.watchedFolders) },
                    { "globalMappings", mappings(s.globalMappings) },
                    { "footswitchKeys", keys },
                    { "footswitchDevice", device },
                    { "tone3000AutoFetch", s.tone3000AutoFetch },
                    { "tone3000ClientId", s.tone3000ClientId } });
}

juce::var rig(const RigDocument& doc, const std::function<juce::var(const NodeDoc&)>& artFor)
{
    auto v = juce::JSON::parse(serializeRig(doc));
    auto* nodes = v["nodes"].getArray();
    if (nodes == nullptr)
        return v;

    // Signal order first, then anything not on the chain (document order).
    juce::StringArray order;
    for (auto& id : graph::linearOrder(doc))
        order.add(id);
    juce::Array<juce::var> sorted;
    for (auto& id : order)
        for (auto& n : *nodes)
            if (n["id"].toString() == id)
                sorted.add(n);
    for (auto& n : *nodes)
        if (! order.contains(n["id"].toString()))
            sorted.add(n);

    for (auto& n : sorted)
        for (auto& nd : doc.nodes)
            if (nd.id == n["id"].toString())
                n.getDynamicObject()->setProperty("art", artFor(nd));

    v.getDynamicObject()->setProperty("nodes", sorted);
    return v;
}

namespace
{
struct TypeName { NodeType type; const char* name; };
constexpr TypeName kTypeNames[] = { { NodeType::Input, "Input" },   { NodeType::Output, "Output" },
                                    { NodeType::Gate, "Gate" },     { NodeType::EQ, "EQ" },
                                    { NodeType::NamAmp, "NamAmp" }, { NodeType::CabIR, "CabIR" },
                                    { NodeType::Plugin, "Plugin" }, { NodeType::Placeholder, "Placeholder" } };
} // namespace

juce::String nodeTypeName(NodeType t)
{
    for (auto& e : kTypeNames)
        if (e.type == t)
            return e.name;
    return {};
}

std::optional<NodeType> parseNodeType(const juce::String& s)
{
    for (auto& e : kTypeNames)
        if (s == e.name)
            return e.type;
    return std::nullopt;
}
} // namespace json
} // namespace amp
