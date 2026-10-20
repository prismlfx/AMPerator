#include "MainWindow.h"

#include "Engine.h"
#include "bridge/Bridge.h"

#include <AmpUiData.h>

#include <cstring>
#include <optional>

namespace amp
{
namespace
{
constexpr auto devServerUrl = "http://localhost:5173";

juce::String mimeTypeFor(const juce::String& fileName)
{
    const auto ext = fileName.fromLastOccurrenceOf(".", false, false).toLowerCase();
    if (ext == "html") return "text/html";
    if (ext == "js" || ext == "mjs") return "text/javascript";
    if (ext == "css") return "text/css";
    if (ext == "json") return "application/json";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "woff2") return "font/woff2";
    if (ext == "ico") return "image/x-icon";
    return "application/octet-stream";
}

/** Serves the embedded ui/dist. Binary data keeps only file names (the build guarantees they are unique), so a
    request path maps to its last component; "/" is index.html. */
std::optional<juce::WebBrowserComponent::Resource> uiResource(const juce::String& url)
{
    auto path = url.upToFirstOccurrenceOf("?", false, false).upToFirstOccurrenceOf("#", false, false);
    auto fileName = path.fromLastOccurrenceOf("/", false, false);
    if (fileName.isEmpty())
        fileName = "index.html";

    for (int i = 0; i < AmpUiData::namedResourceListSize; ++i)
    {
        const auto* name = AmpUiData::namedResourceList[i];
        if (fileName != AmpUiData::getNamedResourceOriginalFilename(name))
            continue;
        int size = 0;
        const auto* data = AmpUiData::getNamedResource(name, size);
        std::vector<std::byte> bytes(static_cast<size_t>(size));
        std::memcpy(bytes.data(), data, bytes.size());
        return juce::WebBrowserComponent::Resource { std::move(bytes), mimeTypeFor(fileName) };
    }
    return std::nullopt;
}

bool useDevServer()
{
    return juce::SystemStats::getEnvironmentVariable("AMP_UI_DEV", {}) == "1";
}
} // namespace

//==============================================================================
class MainWindow::Content final : public juce::Component
{
public:
    explicit Content(Engine& e)
        : engine(e),
          browser(juce::WebBrowserComponent::Options {}
                      .withNativeIntegrationEnabled()
                      .withKeepPageLoadedWhenBrowserIsHidden()
                      .withNativeFunction("amp", [this](const juce::Array<juce::var>& args, auto complete)
                                          { handleBridgeMessage(args, std::move(complete)); })
                      .withNativeFunction("chooseFolder", [this](const juce::Array<juce::var>&, auto complete)
                                          { chooseFolder(std::move(complete)); })
                      .withResourceProvider(uiResource))
    {
        addAndMakeVisible(browser);

        browser.goToURL(useDevServer() ? juce::String(devServerUrl) : juce::WebBrowserComponent::getResourceProviderRoot());
    }

    ~Content() override { engine.bridge().send = nullptr; }

    void resized() override { browser.setBounds(getLocalBounds()); }

private:
    void handleBridgeMessage(const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion complete)
    {
        // One argument: the command envelope as a JSON string (ui/src/bridge.ts), or an already-decoded object.
        // Anything else is handed to the bridge as-is, which answers malformed input with ok:false.
        juce::var message = args.isEmpty() ? juce::var() : args.getFirst();
        if (message.isString())
        {
            juce::var parsed;
            if (juce::JSON::parse(message.toString(), parsed).wasOk())
                message = parsed;
        }
        complete(juce::var()); // the reply travels as an "amp" event
        connectBridge();
        engine.bridge().handle(message);
    }

    /** Connects Bridge::send on the page's first command, i.e. once the page (and JUCE's backend script) is loaded:
        events emitted earlier would be evaluated in a page that cannot receive them. Until then the Bridge drops
        ordinary events and queues `error` events, which it delivers ahead of this first reply. */
    void connectBridge()
    {
        if (engine.bridge().send)
            return;
        engine.bridge().send = [safe = juce::Component::SafePointer<Content>(this)](const juce::var& message)
        {
            auto emit = [safe, message]
            {
                if (safe != nullptr)
                    safe->browser.emitEventIfBrowserIsVisible("amp", message);
            };
            if (juce::MessageManager::getInstance()->isThisTheMessageThread())
                emit();
            else
                juce::MessageManager::callAsync(emit);
        };
    }

    void chooseFolder(juce::WebBrowserComponent::NativeFunctionCompletion complete)
    {
        if (chooser != nullptr)
        {
            complete(juce::String()); // one chooser at a time
            return;
        }
        chooser = std::make_unique<juce::FileChooser>("Choose a folder to watch for models and IRs",
                                                      juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this, done = std::move(complete)](const juce::FileChooser& fc)
                             {
                                 const auto result = fc.getResult();
                                 done(result == juce::File() ? juce::String() : result.getFullPathName());
                                 juce::MessageManager::callAsync(
                                     [safe = juce::Component::SafePointer<Content>(this)]
                                     {
                                         if (safe != nullptr)
                                             safe->chooser.reset();
                                     });
                             });
    }

    Engine& engine;
    juce::WebBrowserComponent browser;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Content)
};

//==============================================================================
MainWindow::MainWindow(Engine& engine)
    : DocumentWindow("AMPerator", juce::Colour(0xff121316), DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new Content(engine), false);
    setResizable(true, true);
    setResizeLimits(900, 600, 10000, 10000);
    centreWithSize(1100, 700);
    setVisible(true);
}

MainWindow::~MainWindow() = default;

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}
} // namespace amp
