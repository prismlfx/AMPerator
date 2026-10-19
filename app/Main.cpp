#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow()
        : DocumentWindow("AMPerator",
                         juce::Colours::black,
                         DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        centreWithSize(1100, 700);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

class AmperatorApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "AMPerator"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }

    void initialise(const juce::String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }

private:
    std::unique_ptr<MainWindow> window;
};
} // namespace

START_JUCE_APPLICATION(AmperatorApp)
