#include "UI/Components/PlaylistSidebarComponent.h"
#include "UI/Components/ModernFileSummaryComponent.h"
#include "UI/Components/TypedMetadataInspectorComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernPanelPainter.h"
#include "Metadata/LocalArtworkLoader.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace
{
using namespace singleeditor;
void check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "ModernGeneralPanelTests failed: " << message << '\n';
    std::exit(1);
}

class SummaryPanel final : public juce::Component
{
public:
    ModernFileSummaryComponent summary;
    SummaryPanel() { addAndMakeVisible(summary); }
    void resized() override
    {
        const auto body = getLocalBounds().withTrimmedTop(32);
        summary.setAvailableHeight(body.getHeight());
        summary.setBounds(body.withHeight(ModernFileSummaryComponent::layout(body.getWidth(), body.getHeight()).height));
    }
    void paint(juce::Graphics& g) override
    {
        ModernPanelPainter::paint(g, getLocalBounds(), "File information", ModernPanelPainter::Icon::information);
    }
};

class GeneralSurface final : public juce::Component
{
public:
    void paint(juce::Graphics& g) override { g.fillAll(RendererVisualStyle::windowBackground()); }
};

void writeImage(const juce::File& directory, const juce::String& name, juce::Component& component)
{
    const auto image = component.createComponentSnapshot(component.getLocalBounds(), true, 1.5f);
    auto stream = directory.getChildFile(name + ".png").createOutputStream();
    check(stream != nullptr && stream->setPosition(0), "open evidence output");
    check(juce::PNGImageFormat().writeImageToStream(image, *stream) && stream->truncate().wasOk(), "replace screenshot evidence");
}

void checkLocalArtwork(const juce::File& directory, const SingleTrackDocument::FileInfo& info,
                       ModernFileSummaryComponent& summary)
{
    const auto sidecar = info.file.withFileExtension("png");
    const auto fallback = directory.getChildFile("cover.jpg");
    juce::Image red(juce::Image::RGB, 1200, 600, true);
    { juce::Graphics g(red); g.fillAll(juce::Colours::red); }
    juce::Image blue(juce::Image::RGB, 32, 32, true);
    { juce::Graphics g(blue); g.fillAll(juce::Colours::blue); }
    juce::MemoryOutputStream png, jpeg;
    check(juce::PNGImageFormat().writeImageToStream(red, png)
        && juce::JPEGImageFormat().writeImageToStream(blue, jpeg), "encode genuine cover fixtures");
    check(sidecar.replaceWithData(png.getData(), png.getDataSize())
        && fallback.replaceWithData(jpeg.getData(), jpeg.getDataSize()), "write isolated local artwork fixtures");
    auto image = LocalArtworkLoader::readLocalCover(info.file);
    check(image.getWidth() == 512 && image.getHeight() == 256
        && image.getPixelAt(200, 100).getRed() > 240, "same-name PNG wins and is bounded to a 512 pixel thumbnail");
    check(!LocalArtworkLoader::readLocalCover(info.file, [] { return true; }).isValid(),
        "cancelled generation cannot decode or publish a cover");
    summary.setArtwork(info.file, image);
    for (const int width : { 140, 200, 360 })
        for (const int height : { 110, 280 })
        {
            summary.setAvailableHeight(height);
            const auto layout = ModernFileSummaryComponent::layout(width, height);
            summary.setSize(width, layout.height);
            const auto rendered = summary.createComponentSnapshot(summary.getLocalBounds());
            const auto centre = layout.emblem.getCentre();
            check(rendered.getPixelAt(centre.x, centre.y).getRed() > 240
                && rendered.getPixelAt(centre.x, centre.y).getBlue() < 10,
                "real image paints into every responsive emblem slot");
        }
    auto secondInfo = info;
    secondInfo.file = directory.getChildFile("different.wav");
    summary.setInfo(secondInfo);
    summary.setArtwork(info.file, image);
    const auto centre = ModernFileSummaryComponent::layout(summary.getWidth(), 280).emblem.getCentre();
    check(summary.createComponentSnapshot(summary.getLocalBounds()).getPixelAt(centre.x, centre.y).getBlue() > 20,
        "file change clears old artwork and refuses late completion for the old file");
    summary.setInfo(info);
    // A plausible PNG header with enormous dimensions must be rejected before decoding.
    juce::MemoryBlock oversized(png.getData(), png.getDataSize());
    auto* header = static_cast<unsigned char*>(oversized.getData());
    header[16] = 0x7f; header[17] = 0xff; header[18] = 0xff; header[19] = 0xff;
    check(sidecar.replaceWithData(oversized.getData(), oversized.getSize()), "write oversized-header fixture");
    image = LocalArtworkLoader::readLocalCover(info.file);
    check(image.getWidth() == 32 && image.getPixelAt(16, 16).getBlue() > 240,
        "invalid same-name image safely falls back to a genuine JPEG");
    check(sidecar.replaceWithText("truncated image"), "write truncated fixture");
    check(LocalArtworkLoader::readLocalCover(info.file).isValid(), "truncated image cannot prevent fallback");
    check(sidecar.deleteFile() && fallback.deleteFile(), "remove only owned cover fixtures");
    check(!LocalArtworkLoader::readLocalCover(info.file).isValid(), "missing cover retains the document emblem");
    std::cout << "Local artwork: PNG/JPEG precedence, bounds, cancellation, stale source and six responsive paints passed\n";
}
}

void runModernGeneralPanelTests()
{
    using namespace singleeditor;
    for (int width = 100; width <= 600; ++width)
        for (int height : { 60, 110, 164, 165, 189, 190, 220, 280 })
        {
            const auto layout = ModernFileSummaryComponent::layout(width, height);
            const juce::Rectangle<int> content(0, 0, width, layout.height);
            check(content.contains(layout.emblem) && content.contains(layout.title)
                      && !layout.emblem.intersects(layout.title), "responsive file header fits without overlap");
            if (width < 220 || height < 190)
                check(layout.emblem.getWidth() <= 24 && layout.titleEm <= 12,
                      "narrow or short panels reduce emblem and filename together");
            for (int row = 0; row < 6; ++row)
            {
                const auto label = layout.labels[static_cast<size_t>(row)];
                const auto value = layout.values[static_cast<size_t>(row)];
                check(content.contains(label) && content.contains(value) && !label.intersects(value)
                          && !value.intersects(layout.title) && !value.intersects(layout.emblem),
                      "all six source facts retain separate readable layout slots");
            }
            check(layout.height <= 156, "compact facts do not retain a large empty cover area");
        }
    check(ModernFileSummaryComponent::layout(360, 110).height
              < ModernFileSummaryComponent::layout(240, 110).height,
          "a short wide panel uses two fact columns to conserve height");
    const auto standardSidebar = ModernFileSummaryComponent::layout(230, 220);
    check(!standardSidebar.compact && standardSidebar.emblem.getWidth() >= 40
              && standardSidebar.titleEm == 13.0f
              && standardSidebar.labels.front().getX() == standardSidebar.title.getX(),
          "a standard tall sidebar retains the cover and aligned facts beside it");
    const auto shortSidebar = ModernFileSummaryComponent::layout(230, 150);
    check(shortSidebar.compact && shortSidebar.emblem.getWidth() <= 24
              && shortSidebar.titleEm <= 12.0f,
          "the same sidebar still reduces the icon and title when height is scarce");
    const auto fixtureDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("single-editor-general-test", {}, false);
    check(fixtureDirectory.createDirectory().wasOk(), "create isolated fixture directory");
    std::vector<juce::File> files;
    for (const auto* name : { "Field recording.wav", "Dialogue take 02.wav", "A longer descriptive audio filename.wav" })
    {
        auto file = fixtureDirectory.getChildFile(name);
        // A real one-second 48 kHz mono PCM file. This test paints existing facts;
        // it never opens a reader or starts playback.
        juce::MemoryOutputStream wav;
        wav.write("RIFF", 4); wav.writeInt(96036); wav.write("WAVEfmt ", 8); wav.writeInt(16);
        wav.writeShort(1); wav.writeShort(1); wav.writeInt(48000); wav.writeInt(96000);
        wav.writeShort(2); wav.writeShort(16); wav.write("data", 4); wav.writeInt(96000);
        for (int i = 0; i < 48000; ++i) wav.writeShort(0);
        check(file.replaceWithData(wav.getData(), wav.getDataSize()), "create PCM fixture");
        files.push_back(file);
    }
    PlaylistModel model;
    model.addFiles(files);
    PlaylistSidebarComponent playlist(model);
    SummaryPanel metadata;
    SingleTrackDocument::FileInfo info;
    info.file = files[0]; info.formatName = "WAV / PCM"; info.sampleRate = 48000;
    info.bitsPerSample = 16; info.numChannels = 1; info.numSamples = 48000; info.durationSeconds = 1;
    metadata.summary.setInfo(info);
    checkLocalArtwork(fixtureDirectory, info, metadata.summary);
    check(metadata.summary.getTitle() == files[0].getFileName()
            && metadata.summary.getDescription().contains("48.0 kHz"), "summary projects the supplied source facts");
    playlist.updateLoadedDuration(files[0], 1);
    ModernAppleLookAndFeel lookAndFeel;
    GeneralSurface root;
    root.setLookAndFeel(&lookAndFeel);
    root.addAndMakeVisible(playlist);
    root.addAndMakeVisible(metadata);
    auto* list = dynamic_cast<juce::ListBox*>(playlist.getChildComponent(0));
    check(list != nullptr, "exercise the production playlist ListBox");
    const auto output = juce::SystemStats::getEnvironmentVariable("SINGLEEDITOR_GENERAL_OFFSCREEN_DIR", {});
    if (output.isNotEmpty()) check(juce::File(output).createDirectory().wasOk(), "create General evidence directory");
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
    {
        RendererVisualStyle::setModernTheme(theme);
        ModernAppleLookAndFeel::refreshTheme(root);
        for (const int width : { 140, 200, 230, 240, 360 })
            for (const int height : { 150, 222, 280 })
            {
                root.setSize(width * 2 + 12, 280);
                playlist.setBounds(0, 0, width, height);
                metadata.setBounds(width + 12, 0, width, height);
                check(playlist.getLocalBounds().contains(list->getBounds()), "dense playlist stays inside short and narrow Mags");
                check(list->getRowHeight() == 31 && list->getListBoxModel()->getNumRows() == 3, "Modern shows real entries in 31 px rows");
                if (output.isNotEmpty())
                    writeImage(juce::File(output), juce::String(theme == ModernTheme::light ? "light-" : "night-")
                        + juce::String(width) + "x" + juce::String(height), root);
            }
    }
    int activated = -1;
    playlist.onEntryActivated = [&] (int row) { activated = row; model.setCurrentIndex(row); };
    const juce::Point<float> point(60, 12);
    const juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), point,
        juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
        list, list, juce::Time(100), point, juce::Time(0), 1, false);
    list->getListBoxModel()->listBoxItemClicked(1, event);
    check(activated == 1 && model.getCurrentIndex() == 1 && list->getSelectedRow() == 1,
          "a production list click retains the existing activation and selected-row contract");
    int activationRequests = 0;
    playlist.onEntryActivated = [&] (int row) { activated = row; ++activationRequests; };
    check(list->keyPressed(juce::KeyPress(juce::KeyPress::upKey))
              && list->getSelectedRow() == 0 && model.getCurrentIndex() == 1 && activationRequests == 0,
          "arrow navigation selects a candidate without loading or changing current audio");
    for (const auto theme : { ModernTheme::light, ModernTheme::night })
        for (const int width : { 140, 360 })
        {
            RendererVisualStyle::setModernTheme(theme);
            juce::Image selected(juce::Image::ARGB, width, 31, true);
            juce::Image current(juce::Image::ARGB, width, 31, true);
            { juce::Graphics g(selected); list->getListBoxModel()->paintListBoxItem(0, g, width, 31, true); }
            { juce::Graphics g(current); list->getListBoxModel()->paintListBoxItem(1, g, width, 31, false); }
            check(selected.getPixelAt(width / 2, 4).getAlpha() > 0
                      && current.getPixelAt(width / 2, 4).isTransparent()
                      && current.getPixelAt(4, 15).getAlpha() > 0,
                  "only the keyboard selection has a filled row; current audio retains its own indicator");
        }
    check(list->keyPressed(juce::KeyPress(juce::KeyPress::returnKey))
              && activated == 0 && activationRequests == 1 && model.getCurrentIndex() == 1,
          "Enter requests the selected file without bypassing the application's confirmation callback");
    list->getListBoxModel()->returnKeyPressed(-1);
    list->getListBoxModel()->returnKeyPressed(999);
    check(activationRequests == 1 && model.getCurrentIndex() == 1, "invalid rows cannot activate a source");
    list->deselectAllRows();
    list->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    check(activationRequests == 1, "Return without a selection does not activate a stale row");
    playlist.onEntryActivated = [&] (int row) { activated = row; model.setCurrentIndex(row); };
    list->selectRow(0);
    list->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    check(model.getCurrentIndex() == 0 && model.getCurrentEntry()->file == files[0],
          "accepted keyboard activation changes the same model source as mouse activation");
    list->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
    check(model.getCurrentIndex() == 0 && list->getSelectedRow() == 1,
          "continued Down navigation does not implicitly open audio");
    list->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    check(model.getCurrentIndex() == 1 && activated == 1, "Down and Return activate the next existing source");
    check(model.moveEntry(1, 0) && model.getCurrentEntry()->file == files[1], "reordering keeps the selected source identity");
    model.removeAt(2);
    check(list->getListBoxModel()->getNumRows() == 2, "removal refreshes the actual list");
    model.clear();
    check(list->getListBoxModel()->getNumRows() == 0, "empty state contains no decorative fake entries");
    metadata.summary.setInfo({});
    check(metadata.summary.getTitle() == "File information", "generation reset clears old file identity");
    if (output.isNotEmpty()) writeImage(juce::File(output), "empty", root);
    root.setLookAndFeel(nullptr);
    RendererSelection::resetForTests(RendererKind::classic);
    playlist.setSize(361, 280);
    check(list->getRowHeight() == 52, "Classic retains its existing row density");
    model.addFiles(files);
    model.setCurrentIndex(0);
    playlist.onEntryActivated = {};
    list->selectRow(1);
    list->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
    check(model.getCurrentIndex() == 1,
          "Classic and standalone lists retain the shared keyboard activation fallback");
    model.clear();
    RendererSelection::resetForTests(RendererKind::modern);
    RendererVisualStyle::setModernTheme(ModernTheme::light);
    // Only these explicitly created fixture files are removed.
    for (const auto& file : files) check(file.deleteFile(), "remove owned PCM fixture");
    check(fixtureDirectory.deleteFile(), "remove empty owned fixture directory");
}

// Runs last: this owns the normal JUCE message loop and stops it on completion.
void runLocalArtworkAsyncTests()
{
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("single-editor-artwork-async", {}, false);
    check(directory.createDirectory().wasOk(), "create asynchronous artwork fixture directory");
    const auto first = directory.getChildFile("first.wav");
    const auto second = directory.getChildFile("second.wav");
    const auto cover = first.withFileExtension("png");
    juce::Image image(juce::Image::RGB, 32, 32, true);
    { juce::Graphics g(image); g.fillAll(juce::Colours::red); }
    juce::MemoryOutputStream bytes;
    check(juce::PNGImageFormat().writeImageToStream(image, bytes)
        && cover.replaceWithData(bytes.getData(), bytes.getDataSize()), "write asynchronous PNG fixture");
    class Deadline final : private juce::Timer
    {
    public:
        Deadline() { startTimer(5000); }
        ~Deadline() override { stopTimer(); }
    private:
        void timerCallback() override { juce::MessageManager::getInstance()->stopDispatchLoop(); }
    } deadline;
    int callbacks = 0;
    {
        LocalArtworkLoader loader;
        loader.request(first, [&](const juce::File& source, const juce::Image& loaded)
        {
            check(juce::MessageManager::getInstance()->isThisTheMessageThread(), "artwork completion runs on UI thread");
            check(source == first && loaded.isValid(), "worker delivers actual local cover");
            ++callbacks;
            loader.request(first, [](const juce::File&, const juce::Image&)
            { check(false, "cancelled callback must never run"); });
            loader.cancel();
            loader.request(second, [&](const juce::File& next, const juce::Image& missing)
            {
                check(next == second && !missing.isValid(), "new generation delivers empty cover without reusing old result");
                ++callbacks;
                juce::MessageManager::getInstance()->stopDispatchLoop();
            });
        });
        check(callbacks == 0, "post-CoreReady request performs no synchronous decode or callback");
        juce::MessageManager::getInstance()->runDispatchLoop();
        check(callbacks == 2, "worker completion and cancellation finish within deadline");
        // Exercise destruction while more work may be in flight; no detached worker.
        loader.request(first, [](const juce::File&, const juce::Image&)
        { check(false, "destroyed loader must not publish"); });
    }
    check(cover.deleteFile() && directory.deleteFile(), "remove only owned asynchronous fixture files");
    std::cout << "Local artwork async: UI delivery, cancelled generation, source switch and joined shutdown passed\n";
}
