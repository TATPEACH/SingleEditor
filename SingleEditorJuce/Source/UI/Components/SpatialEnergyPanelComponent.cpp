#include "SpatialEnergyPanelComponent.h"
#include "UI/LookAndFeel/ModernAppleLookAndFeel.h"
#include "UI/LookAndFeel/ModernTypography.h"
#include "UI/Renderer/RendererVisualStyle.h"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace singleeditor
{
namespace
{
class SpatialOutputRow final : public juce::Label
{
public:
    explicit SpatialOutputRow(juce::Viewport& target) : viewport(target) {}
    void focusGained(FocusChangeType) override
    {
        const auto visible=viewport.getViewArea();
        auto y=visible.getY();
        if (getY()<visible.getY()) y=getY();
        else if (getBottom()>visible.getBottom()) y=getBottom()-visible.getHeight();
        viewport.setViewPosition(visible.getX(),y);
        repaint();
    }
    void focusLost(FocusChangeType) override { repaint(); }
    void paintOverChildren(juce::Graphics& g) override
    {
        if (!hasKeyboardFocus(false)) return;
        g.setColour(ModernAppleLookAndFeel::primaryText().withAlpha(0.65f));
        g.drawRect(getLocalBounds().reduced(1),1);
    }
private:
    juce::Viewport& viewport;
};
class SpatialValue final : public juce::AccessibilityTextValueInterface
{
public:
    explicit SpatialValue(const SpatialEnergyPanelComponent& p) : panel(p) {}
    bool isReadOnly() const override { return true; }
    juce::String getCurrentValueAsString() const override { return panel.getAccessibilityValueSummary(); }
    void setValueAsString(const juce::String&) override {}
private:
    const SpatialEnergyPanelComponent& panel;
};
juce::String channelText(const SpatialEnergyMeter::Snapshot& s, std::size_t i)
{
    const auto& c = s.layout->channels[i]; const auto& v = s.channels[i];
    const auto db = [](double x) { return std::isfinite(x) ? juce::String(x,1) : juce::String("-inf"); };
    const auto placement = c.placement == SpatialEnergyMeter::Placement::lfe ? "LFE (non-directional)"
        : c.placement == SpatialEnergyMeter::Placement::unplaced ? "unplaced" : "directional";
    return "Out " + juce::String(c.physicalOutput+1) + " " + juce::String(c.label)
        + " | " + placement + (c.ambiguous ? " / ambiguous" : "")
        + (c.mapped ? " / mapped" : " / no mapped feed")
        + " | K " + db(v.contributionDb) + " dB | RMS " + db(v.rmsDbfs)
        + " dBFS | sample peak " + db(v.samplePeakDbfs) + " dBFS";
}
}
class SpatialEnergyPanelComponent::VisibilityWatcher final : public juce::ComponentMovementWatcher
{
public:
    explicit VisibilityWatcher(SpatialEnergyPanelComponent& p) : ComponentMovementWatcher(&p), panel(p) {}
    void componentMovedOrResized(bool, bool) override {}
    void componentPeerChanged() override { panel.refreshActivity(); }
    void componentVisibilityChanged() override { panel.refreshActivity(); }
private:
    SpatialEnergyPanelComponent& panel;
};
class SpatialEnergyPanelComponent::VisualWorker final
{
public:
    explicit VisualWorker(std::shared_ptr<const SpatialEnergyVisualField> plane = {}
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        , OptionalLifecycleToken lifetime = {}
#endif
    )
        :
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
          observedLifetime(std::move(lifetime)),
#endif
          cachedPlane(std::move(plane)), thread([this] { run(); })
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.created(); observedLifetime.complete = true;
#endif
    }
    ~VisualWorker()
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.note(OptionalLifecycleAction::cancelRequested);
#endif
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_one(); thread.join();
    }
    void request(std::shared_ptr<const SpatialEnergyMeter::Layout> next, bool sphere)
    {
        std::lock_guard lock(mutex);
        if (layout == next && includeSphere == sphere) return;
        layout = std::move(next); includeSphere = sphere; pending = true; failed = false;
        published.reset(); wake.notify_one();
    }
    std::shared_ptr<const SpatialEnergyVisualField> result(bool& failure)
    { std::lock_guard lock(mutex); failure = failed; return published; }
    std::shared_ptr<const SpatialEnergyVisualField> plane()
    { std::lock_guard lock(mutex); return cachedPlane; }
private:
    void run()
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        observedLifetime.token.note(OptionalLifecycleAction::taskStarted);
#endif
        for (;;)
        {
            std::shared_ptr<const SpatialEnergyMeter::Layout> target; bool sphere;
            std::shared_ptr<const SpatialEnergyVisualField> base;
            {
                std::unique_lock lock(mutex); wake.wait(lock,[&]{return stopping || pending;});
                if (stopping)
                {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
                    observedLifetime.token.note(OptionalLifecycleAction::taskCompleted);
#endif
                    return;
                }
                target = layout; sphere = includeSphere; pending = false; base=cachedPlane;
            }
            std::shared_ptr<const SpatialEnergyVisualField> artifact;
            try
            {
                base=SpatialEnergyVisualField::prepare(target,false,std::move(base));
                artifact=SpatialEnergyVisualField::prepare(target,sphere,base);
            }
            catch (...) {}
            std::lock_guard lock(mutex);
            if (target==layout) cachedPlane=std::move(base);
            if (target == layout && sphere == includeSphere)
            { failed = !artifact; published = std::move(artifact); }
        }
    }
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
    struct ObservedLifetime
    {
        explicit ObservedLifetime(OptionalLifecycleToken value) noexcept : token(std::move(value)) {}
        ~ObservedLifetime() { if (complete) token.release(); else token.failed(); }
        OptionalLifecycleToken token;
        bool complete = false;
    } observedLifetime; // destroyed after joined thread and retained visual resources
#endif
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false, pending = false, includeSphere = false, failed = false;
    std::shared_ptr<const SpatialEnergyMeter::Layout> layout;
    std::shared_ptr<const SpatialEnergyVisualField> published;
    std::shared_ptr<const SpatialEnergyVisualField> cachedPlane;
    std::thread thread;
};
SpatialEnergyPanelComponent::SpatialEnergyPanelComponent(AudioEngine& e) : engine(e)
{
    setName("Rendered Output Spatial Energy"); setTitle(getName());
    setDescription("Read-only speaker-feed energy estimate. Not an acoustic room simulation.");
    setWantsKeyboardFocus(true);
    heading.setText("Rendered Energy Estimate", juce::dontSendNotification);
    heading.setFont(ModernTypography::font(14,juce::Font::bold));
    for (auto* label : { &heading, &loudness, &diagnostics, &warnings })
    { label->setInterceptsMouseClicks(false,false); addAndMakeVisible(*label); }
    reading.addItem("M / 400 ms",1); reading.addItem("S / 3 s",2); reading.addItem("Integrated",3);
    reading.setSelectedId(1); reading.setName("Loudness readout window");
    reading.onChange = [this]{ updateReadouts(); };
    addAndMakeVisible(reading);
    sphereButton.setClickingTogglesState(true); sphereButton.setName("Inspect 3D Sphere (display only)");
    sphereButton.onClick = [this]{ setSphereInspection(sphereButton.getToggleState()); };
    addChildComponent(sphereButton);
    floorControl.setRange(-120,-10,1); ceilingControl.setRange(-9,20,1);
    floorControl.setValue(response.floorDb); ceilingControl.setValue(response.ceilingDb);
    floorControl.setName("Display floor dB"); ceilingControl.setName("Display ceiling dB");
    floorControl.setTextValueSuffix(" dB floor"); ceilingControl.setTextValueSuffix(" dB ceiling");
    for (auto* slider : { &floorControl, &ceilingControl })
    { slider->setSliderStyle(juce::Slider::IncDecButtons); slider->setTextBoxStyle(juce::Slider::TextBoxLeft,false,105,24); addChildComponent(*slider); }
    floorControl.onValueChange = [this]{ response.floorDb=floorControl.getValue(); rebuildImage(); repaint(); };
    ceilingControl.onValueChange = [this]{ response.ceilingDb=ceilingControl.getValue(); rebuildImage(); repaint(); };
    viewport.setViewedComponent(&table,false); viewport.setScrollBarsShown(true,true);
    viewport.setName("Physical output values (read only)"); addChildComponent(viewport);
    watcher = std::make_unique<VisibilityWatcher>(*this);
    updateReadouts();
}
SpatialEnergyPanelComponent::~SpatialEnergyPanelComponent()
{ watcher.reset(); stopTimer();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
  if (timerLifecycle.isOpen()) { timerLifecycle.quiesce(); timerLifecycle.release(); }
#endif
  worker.reset(); lease = {}; }
void SpatialEnergyPanelComponent::setCoreReady(bool ready)
{ coreReady=ready; refreshActivity(); }
void SpatialEnergyPanelComponent::applyWorkspaceVisualContract(RendererDensity,RendererMagSize,RendererInformationTier tier,bool)
{ requestedTier=tier; resized(); }
void SpatialEnergyPanelComponent::chooseView()
{
    const auto compact = requestedTier == RendererInformationTier::summary
        || getWidth() < (view == View::compact ? 264 : 240) || getHeight() < (view == View::compact ? 254 : 230);
    if (compact) view=View::compact;
    else if (requestedTier == RendererInformationTier::detail
        && getWidth() >= (view == View::expanded ? 456 : 480) && getHeight() >= (view == View::expanded ? 416 : 440)) view=View::expanded;
    else view=View::standard;
    if (view != View::expanded && inspectSphere)
    {
        inspectSphere=false; sphereButton.setToggleState(false,juce::dontSendNotification);
        // Drop every owner of the old sphere synchronously at the breakpoint.
        // The measurement lease and its I window remain untouched.
        releaseSpherePreservingPlane();
    }
}
void SpatialEnergyPanelComponent::refreshActivity()
{
    if (!coreReady || !isShowing())
    {
        if (snapshot) retainedSummary=getAccessibilityValueSummary();
        stopTimer();
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        if (timerLifecycle.isOpen()) { timerLifecycle.quiesce(); timerLifecycle.release(); }
#endif
        worker.reset(); field.reset(); mapImage={}; displayedPowers.clear(); snapshot.reset(); lease={};
        inspectSphere=false; sphereButton.setToggleState(false,juce::dontSendNotification);
        lastTickMs=stoppedAtMs=0; return;
    }
    if (!lease) lease=engine.acquireSpatialEnergyAnalysisLease(true);
    if (view == View::compact) { worker.reset(); field.reset(); mapImage={}; }
    else if (!worker)
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        auto lifetime = OptionalLifecycleToken::request(optionalLifecycleContext,
            OptionalLifecycleOwner::spatialEnergy, OptionalLifecycleResourceKind::spatialVisualWorker);
        try
        {
#endif
        worker=std::make_unique<VisualWorker>(field
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
            , std::move(lifetime)
#endif
        );
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        }
        catch (...)
        {
            if (lifetime.isOpen()) lifetime.failed();
            throw;
        }
#endif
    }
    if (lease)
    {
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        const auto starting = !isTimerRunning();
        if (starting) timerLifecycle = OptionalLifecycleToken::request(optionalLifecycleContext,
            OptionalLifecycleOwner::spatialEnergy, OptionalLifecycleResourceKind::viewTimer);
        try
        {
#endif
        startTimerHz(overloadUntilMs > juce::Time::getMillisecondCounterHiRes() ? 10 : response.framesPerSecond);
#if SINGLE_EDITOR_ENABLE_TEST_OBSERVERS
        if (starting)
        {
            if (isTimerRunning()) { timerLifecycle.created(); timerLifecycle.activate(); }
            else timerLifecycle.failed();
        }
        }
        catch (...)
        {
            if (starting && timerLifecycle.isOpen()) timerLifecycle.failed();
            throw;
        }
#endif
    }
}
void SpatialEnergyPanelComponent::setSphereInspection(bool enabled)
{
    const auto next=enabled && view == View::expanded && coreReady && isShowing();
    if (inspectSphere && !next) releaseSpherePreservingPlane();
    inspectSphere=next;
    sphereButton.setToggleState(inspectSphere,juce::dontSendNotification);
    if (worker && snapshot) worker->request(snapshot->layout,inspectSphere);
    resized(); repaint();
}
void SpatialEnergyPanelComponent::releaseSpherePreservingPlane()
{
    auto plane=worker ? worker->plane() : nullptr;
    if (!plane && field) plane=field->planarBase ? field->planarBase : field;
    worker.reset(); // joins in-flight sphere preparation before returning
    field=std::move(plane);
    if (field && snapshot && field->layout!=snapshot->layout) { field.reset(); mapImage={}; }
}
bool SpatialEnergyPanelComponent::hasSphereResource() const noexcept
{ return field && !field->sphere.empty(); }
void SpatialEnergyPanelComponent::timerCallback()
{
    const auto started=juce::Time::getMillisecondCounterHiRes();
    if (!coreReady || !isShowing()) { refreshActivity(); return; }
    engine.refreshSpatialEnergyAnalysis();
    const auto next=engine.getSpatialEnergySnapshot();
    const auto now=juce::Time::getMillisecondCounterHiRes();
    const auto dt=lastTickMs == 0 ? 50.0 : std::min(250.0,now-lastTickMs); lastTickMs=now;
    const auto changedLayout=!next || !snapshot || next->layout != snapshot->layout || next->epoch != snapshot->epoch;
    if (changedLayout) { displayedPowers.clear(); field.reset(); mapImage={}; stoppedAtMs=0; }
    snapshot=next;
    if (changedLayout) resized();
    if (snapshot)
    {
        if (snapshot->transport == SpatialEnergyMeter::Transport::stopped)
        { if (stoppedAtMs == 0) stoppedAtMs=now; }
        else stoppedAtMs=0;
        const auto decay=stoppedAtMs > 0 ? std::max(0.0,now-stoppedAtMs-response.stopHoldMs)*response.stopDecayDbPerSecond/1000 : 0;
        if (displayedPowers.size()!=snapshot->channels.size()) displayedPowers.assign(snapshot->channels.size(),0);
        for (std::size_t i=0;i<displayedPowers.size();++i)
        {
            const auto target=snapshot->channels[i].kWeightedMeanSquare*std::pow(10.0,-decay/10);
            const auto time=target>displayedPowers[i] ? response.attackMs : response.releaseMs;
            const auto alpha=RendererVisualStyle::isReduceMotion() || changedLayout ? 1.0 : 1-std::exp(-dt/time);
            if (snapshot->transport != SpatialEnergyMeter::Transport::paused || changedLayout)
                displayedPowers[i]+=alpha*(target-displayedPowers[i]);
        }
        if (worker)
        {
            worker->request(snapshot->layout,inspectSphere);
            if (auto prepared=worker->result(geometryFailure)) field=std::move(prepared);
            else if (!field || field->layout!=snapshot->layout)
            {
                auto plane=worker->plane();
                field=plane && plane->layout==snapshot->layout ? std::move(plane) : nullptr;
            }
        }
    }
    if (overloadUntilMs > now) mapImage={}; else rebuildImage();
    updateReadouts(); repaint();
    // A bounded visual fallback never changes meter cadence/windows or audio.
    if (juce::Time::getMillisecondCounterHiRes()-started > 15)
    {
        overloadUntilMs=now+1500;
        if (inspectSphere) setSphereInspection(false);
        startTimerHz(10);
    }
    else if (overloadUntilMs > 0 && now >= overloadUntilMs)
    { overloadUntilMs=0; startTimerHz(response.framesPerSecond); }
}
juce::String SpatialEnergyPanelComponent::number(double v)
{ return std::isfinite(v) ? juce::String(v,1) : juce::String("-inf"); }
void SpatialEnergyPanelComponent::updateReadouts()
{
    const auto text=ModernAppleLookAndFeel::primaryText();
    const auto background=ModernAppleLookAndFeel::panelBackground();
    reading.setColour(juce::ComboBox::textColourId,text);
    reading.setColour(juce::ComboBox::arrowColourId,text);
    reading.setColour(juce::ComboBox::backgroundColourId,background);
    reading.setColour(juce::ComboBox::outlineColourId,ModernAppleLookAndFeel::borderColour());
    sphereButton.setColour(juce::TextButton::textColourOffId,text);
    for (auto* slider : {&floorControl,&ceilingControl})
    {
        slider->setColour(juce::Slider::textBoxTextColourId,text);
        slider->setColour(juce::Slider::textBoxBackgroundColourId,background);
        slider->setColour(juce::Slider::textBoxOutlineColourId,ModernAppleLookAndFeel::borderColour());
    }
    for (auto* label : { &heading,&loudness,&diagnostics,&warnings }) label->setColour(juce::Label::textColourId,text);
    if (!snapshot || !snapshot->layout)
    {
        loudness.setText("Waiting for output",juce::dontSendNotification);
        diagnostics.setText(coreReady ? "Play to measure / no inferred geometry" : "Waiting for CoreReady",juce::dontSendNotification);
        warnings.setText("Read-only / final physical outputs",juce::dontSendNotification); return;
    }
    const auto& s=*snapshot; const auto summary=SpatialEnergyVisualSummary::from(s);
    const auto selected=reading.getSelectedId()==2 ? s.shortTermLufs : reading.getSelectedId()==3 ? s.integratedLufs : s.momentaryLufs;
    loudness.setText((selected ? number(*selected)+" LUFS" : juce::String("LUFS unavailable / warming"))
        +(view==View::compact ? juce::String() : " / sample peak " + number(summary.peakDbfs)+" dBFS"),juce::dontSendNotification);
    diagnostics.setFont(ModernTypography::font(11)); warnings.setFont(ModernTypography::font(11));
    diagnostics.setText("Out "+juce::String(summary.maximumOutput+1)+" max / "
        +(view==View::compact ? "peak "+number(summary.peakDbfs)+" dBFS"
        : (summary.directional ? "Az "+juce::String(juce::roundToInt(summary.azimuth))+" El "+juce::String(juce::roundToInt(summary.elevation))+" deg" : "Direction unavailable")
        +" / spread "+juce::String(juce::roundToInt(summary.spread*100))+"%"),juce::dontSendNotification);
    juce::String status=s.transport==SpatialEnergyMeter::Transport::unavailable ? "Output unavailable"
        : s.transport==SpatialEnergyMeter::Transport::paused ? "Paused / held"
        : s.transport==SpatialEnergyMeter::Transport::stopped ? "Stopped / display decay" : "Momentary K power";
    status+=" / LFE "+juce::String(summary.lfe)+" / unplaced "+juce::String(summary.unplaced)
        +" / unmapped "+juce::String(s.layout->unmappedSpeakerCount);
    if (!s.layout->geometryCurrent) status+=" / geometry missing/stale";
    if (s.discontinuity) status+=" / measurement gap";
    if (overloadUntilMs>juce::Time::getMillisecondCounterHiRes()) status+=" / visual degraded: anchors only";
    else if (geometryFailure) status+=mapImage.isValid() ? " / 3D unavailable: 2D retained" : " / visual degraded: anchors only";
    else if (worker && !field && s.layout->geometryCurrent) status+=" / preparing visual";
    warnings.setText(status,juce::dontSendNotification);
    const auto count=view==View::compact ? std::size_t{0} : std::min(s.channels.size(),s.layout->channels.size());
    if (rows.size()!=count)
    {
        rows.clear();
        for (std::size_t i=0;i<count;++i)
        {
            auto row=std::make_unique<SpatialOutputRow>(viewport); row->setFont(ModernTypography::font(11));
            row->setEditable(false); row->setWantsKeyboardFocus(true); table.addAndMakeVisible(*row); rows.push_back(std::move(row));
        }
        resized();
    }
    for (std::size_t i=0;i<count;++i)
    {
        const auto value=channelText(s,i); rows[i]->setText(value,juce::dontSendNotification);
        rows[i]->setName(value); rows[i]->setTooltip(value); rows[i]->setColour(juce::Label::textColourId,text);
    }
}
juce::String SpatialEnergyPanelComponent::getAccessibilityValueSummary() const
{
    if (!snapshot) return retainedSummary.isNotEmpty() ? "Inactive. Last measurement: "+retainedSummary : "Spatial Energy unavailable. No measurement.";
    juce::String text="Rendered Energy Estimate. "+loudness.getText()+". "+diagnostics.getText()+". "+warnings.getText();
    const auto direction=SpatialEnergyVisualSummary::from(*snapshot);
    text+=". Dominant direction "+(direction.directional ? "azimuth "+number(direction.azimuth)+", elevation "+number(direction.elevation)+" degrees" : juce::String("unavailable"))
        +", spread "+number(direction.spread*100)+" percent.";
    const auto value=[](const std::optional<double>& v){return v ? number(*v) : juce::String("unavailable");};
    text+=". M "+value(snapshot->momentaryLufs)+", S "+value(snapshot->shortTermLufs)+", I "+value(snapshot->integratedLufs)+" LUFS.";
    if (snapshot->layout) for (std::size_t i=0;i<snapshot->channels.size() && i<snapshot->layout->channels.size();++i) text+=" "+channelText(*snapshot,i)+".";
    return text;
}
std::unique_ptr<juce::AccessibilityHandler> SpatialEnergyPanelComponent::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(*this,juce::AccessibilityRole::group,
        juce::AccessibilityActions{},juce::AccessibilityHandler::Interfaces{std::make_unique<SpatialValue>(*this)});
}
juce::Colour SpatialEnergyPanelComponent::colour(double db,const SpatialEnergyVisualResponseProfile& p)
{
    const auto t=static_cast<float>(std::clamp((db-p.floorDb)/(p.ceilingDb-p.floorDb),0.0,1.0));
    const auto cold=juce::Colour::fromRGB(32,93,141), mid=juce::Colour::fromRGB(42,187,182), hot=juce::Colour::fromRGB(255,177,69);
    return t<0.55f ? cold.interpolatedWith(mid,t/0.55f) : mid.interpolatedWith(hot,(t-0.55f)/0.45f);
}
void SpatialEnergyPanelComponent::rebuildImage()
{
    if (!field || !snapshot || field->layout != snapshot->layout || field->mapPoints().empty()) { mapImage={}; return; }
    if (!mapImage.isValid()) mapImage=juce::Image(juce::Image::ARGB,SpatialEnergyVisualField::width,SpatialEnergyVisualField::height,true);
    juce::Image::BitmapData pixels(mapImage,juce::Image::BitmapData::writeOnly);
    for (std::size_t i=0;i<field->mapPoints().size();++i)
    {
        double power=0;
        for (std::size_t c=0;c<displayedPowers.size();++c) power+=field->mapPoints()[i].weights[c]*displayedPowers[c];
        const auto pixel=field->mapPixels()[i]; const auto db=SpatialEnergyMeter::powerDb(power);
        pixels.setPixelColour(pixel%SpatialEnergyVisualField::width,pixel/SpatialEnergyVisualField::width,
            colour(db,response).withAlpha(0.12f+0.78f*static_cast<float>(std::clamp((db-response.floorDb)/12.0,0.0,1.0))));
    }
}
void SpatialEnergyPanelComponent::resized()
{
    chooseView();
    auto r=getLocalBounds().reduced(10); heading.setBounds(r.removeFromTop(22));
    auto line=r.removeFromTop(26); reading.setBounds(line.removeFromRight(std::min(120,line.getWidth()/2))); loudness.setBounds(line);
    diagnostics.setBounds(r.removeFromTop(22)); warnings.setBounds(r.removeFromBottom(view==View::compact ? 34 : 42));
    warnings.setMinimumHorizontalScale(0.85f);
    const auto expanded=view==View::expanded;
    sphereButton.setVisible(expanded); floorControl.setVisible(expanded); ceilingControl.setVisible(expanded);
    if (expanded)
    {
        auto controls=r.removeFromTop(26); sphereButton.setBounds(controls.removeFromLeft(100));
        floorControl.setBounds(controls.removeFromLeft(150)); ceilingControl.setBounds(controls.removeFromLeft(160));
    }
    legendBounds=r.removeFromBottom(24).toFloat();
    const auto showTable=expanded;
    viewport.setVisible(showTable);
    if (showTable) viewport.setBounds(r.removeFromBottom(std::min(expanded ? 130 : 90,r.getHeight()/2)));
    table.setSize(std::max(680,viewport.getWidth()-12),static_cast<int>(rows.size())*24);
    for (std::size_t i=0;i<rows.size();++i) rows[i]->setBounds(0,static_cast<int>(i)*24,table.getWidth(),24);
    sphereBounds={}; mapBounds=r.toFloat().reduced(4);
    if (inspectSphere) sphereBounds=mapBounds.removeFromRight(mapBounds.getWidth()*0.42f).reduced(4);
    refreshActivity(); repaint();
}
void SpatialEnergyPanelComponent::drawMap(juce::Graphics& g,juce::Rectangle<float> area)
{
    if (area.isEmpty() || !snapshot || !snapshot->layout) return;
    const auto& layout=*snapshot->layout;
    const auto count=std::min(layout.channels.size(),snapshot->channels.size());
    if (count==0) return;
    if (view==View::compact)
    {
        // A discrete output grid preserves identities at small sizes without
        // pretending unknown/LFE positions belong on a directional map.
        const auto columns=std::min(static_cast<int>(count),std::max(1,static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count)*area.getWidth()/area.getHeight())))));
        const auto lines=(static_cast<int>(count)+columns-1)/columns;
        const auto cellWidth=area.getWidth()/columns, cellHeight=area.getHeight()/lines;
        g.setFont(ModernTypography::font(10));
        for (std::size_t i=0;i<count;++i)
        {
            const auto& c=layout.channels[i];
            const juce::Rectangle<float> cell(area.getX()+static_cast<int>(i)%columns*cellWidth,
                area.getY()+static_cast<int>(i)/columns*cellHeight,cellWidth,cellHeight);
            const auto power=i<displayedPowers.size() ? displayedPowers[i] : 0;
            const auto background=colour(SpatialEnergyMeter::powerDb(power),response);
            g.setColour(background);
            g.fillRoundedRectangle(cell.reduced(2,1),3);
            g.setColour(background.getPerceivedBrightness()>0.55f ? juce::Colour::fromRGB(12,35,48) : juce::Colours::white);
            const auto suffix=c.placement==SpatialEnergyMeter::Placement::lfe ? "L"
                : !layout.geometryCurrent || c.placement!=SpatialEnergyMeter::Placement::directional ? "?" : "";
            g.drawText(juce::String(c.physicalOutput+1)+suffix,cell.reduced(2,0),juce::Justification::centred);
        }
        return;
    }
    auto diagnosticArea=area;
    const auto diagnosticCount=static_cast<int>(std::count_if(layout.channels.begin(),layout.channels.end(),[&](const auto& c)
        { return !layout.geometryCurrent || c.placement!=SpatialEnergyMeter::Placement::directional; }));
    const auto diagnosticColumns=std::max(1,std::min(diagnosticCount,static_cast<int>(area.getWidth()/86)));
    if (diagnosticCount>0 && layout.geometryCurrent)
        diagnosticArea=area.removeFromBottom(static_cast<float>((diagnosticCount+diagnosticColumns-1)/diagnosticColumns)*34);
    if (layout.geometryCurrent)
    {
        const auto w=std::min(area.getWidth(),area.getHeight()*2); area=juce::Rectangle<float>(w,w/2).withCentre(area.getCentre());
        if (mapImage.isValid()) {g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);g.drawImage(mapImage,area);}
        g.setColour(ModernAppleLookAndFeel::borderColour()); g.drawEllipse(area,1);
        g.drawLine(area.getX(),area.getCentreY(),area.getRight(),area.getCentreY(),0.6f);
        g.drawLine(area.getCentreX(),area.getY(),area.getCentreX(),area.getBottom(),0.6f);
        g.setColour(ModernAppleLookAndFeel::secondaryText()); g.setFont(ModernTypography::font(10));
        g.drawText("Front / 0 deg",area.withHeight(16),juce::Justification::centred);
    }
    g.setFont(ModernTypography::font(10));
    int diagnosticIndex=0;
    std::array<juce::Rectangle<float>,SpatialEnergyMeter::maxChannels> placedLabels {};
    std::size_t placedLabelCount=0;
    for (std::size_t i=0;i<count;++i)
    {
        const auto& c=layout.channels[i]; const bool placed=layout.geometryCurrent && c.placement==SpatialEnergyMeter::Placement::directional;
        juce::Point<float> point;
        if (placed)
        {
            const auto p=SpatialEnergyVisualField::project(c.azimuthDegrees,c.elevationDegrees);
            point={area.getCentreX()+p[0]*area.getWidth()*0.5f,area.getCentreY()-p[1]*area.getHeight()*0.5f};
        }
        else
        {
            // Explicit diagnostics outside the directional field. Never invent
            // a speaker position from an output count.
            const auto cellWidth=diagnosticArea.getWidth()/diagnosticColumns;
            point={diagnosticArea.getX()+(diagnosticIndex%diagnosticColumns+0.5f)*cellWidth,
                   diagnosticArea.getY()+6+static_cast<float>(diagnosticIndex/diagnosticColumns)*34};
            ++diagnosticIndex;
        }
        const auto power=i<displayedPowers.size() ? displayedPowers[i] : 0;
        g.setColour(colour(SpatialEnergyMeter::powerDb(power),response)); g.fillEllipse(point.x-4,point.y-4,8,8);
        g.setColour(ModernAppleLookAndFeel::primaryText());
        const auto label="Out "+juce::String(c.physicalOutput+1)+(c.placement==SpatialEnergyMeter::Placement::lfe ? " LFE" : !placed ? " ?" : "");
        auto labelBounds=juce::Rectangle<float>(point.x-32,point.y+4,64,16);
        if (placed)
        {
            const auto width=juce::GlyphArrangement::getStringWidth(g.getCurrentFont(),label)+6;
            for (const float offset : {4.0f,18.0f,-18.0f,32.0f,-32.0f})
            {
                labelBounds={std::clamp(point.x-width/2,area.getX(),std::max(area.getX(),area.getRight()-width)),
                    std::clamp(point.y+offset,area.getY(),std::max(area.getY(),area.getBottom()-16)),width,16};
                if (std::none_of(placedLabels.begin(),placedLabels.begin()+placedLabelCount,
                    [&](const auto& previous){return previous.expanded(2).intersects(labelBounds);})) break;
            }
            if (placedLabelCount<placedLabels.size()) placedLabels[placedLabelCount++]=labelBounds;
            if (std::abs(labelBounds.getY()-(point.y+4))>1)
            {
                g.setColour(ModernAppleLookAndFeel::secondaryText().withAlpha(0.5f));
                g.drawLine(point.x,point.y,point.x,labelBounds.getY()>point.y ? labelBounds.getY() : labelBounds.getBottom(),0.6f);
                g.setColour(ModernAppleLookAndFeel::primaryText());
            }
        }
        g.drawText(label,labelBounds,juce::Justification::centred);
        if (!placed)
            g.drawText("K "+number(snapshot->channels[i].contributionDb)+" dB",juce::Rectangle<float>(point.x-42,point.y+17,84,14),juce::Justification::centred);
    }
}
void SpatialEnergyPanelComponent::drawSphere(juce::Graphics& g,juce::Rectangle<float> area)
{
    if (!field || !snapshot || field->sphere.empty() || area.isEmpty()) return;
    const auto size=std::min(area.getWidth(),area.getHeight())*0.9f; const auto centre=area.getCentre();
    g.setColour(ModernAppleLookAndFeel::borderColour()); g.drawEllipse(centre.x-size/2,centre.y-size/2,size,size,1);
    const auto cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
    for (const auto& p:field->sphere)
    {
        const auto x=cy*p.direction.x+sy*p.direction.z, z=-sy*p.direction.x+cy*p.direction.z;
        const auto y=cp*p.direction.y-sp*z, depth=sp*p.direction.y+cp*z;
        if (depth<0) continue;
        double power=0; for(std::size_t i=0;i<displayedPowers.size();++i) power+=p.weights[i]*displayedPowers[i];
        g.setColour(colour(SpatialEnergyMeter::powerDb(power),response).withAlpha(0.85f));
        const auto radius=std::max(2.0f,size/24); g.fillEllipse(static_cast<float>(centre.x+x*size*zoom/2-radius/2),
            static_cast<float>(centre.y-y*size*zoom/2-radius/2),radius,radius);
    }
    g.setColour(ModernAppleLookAndFeel::secondaryText()); g.setFont(ModernTypography::font(10));
    g.drawText("Drag / arrows / zoom +/-",area.removeFromBottom(16),juce::Justification::centred);
}
void SpatialEnergyPanelComponent::paint(juce::Graphics& g)
{
    g.setColour(ModernAppleLookAndFeel::panelBackground()); g.fillRoundedRectangle(getLocalBounds().toFloat(),ModernAppleLookAndFeel::cardRadius());
    drawMap(g,mapBounds); if(inspectSphere) drawSphere(g,sphereBounds);
    if (!legendBounds.isEmpty())
    {
        const auto bar=legendBounds.withHeight(5);
        for(int i=0;i<64;++i) {g.setColour(colour(response.floorDb+(response.ceilingDb-response.floorDb)*i/63,response));g.fillRect(bar.getX()+bar.getWidth()*i/64,bar.getY(),bar.getWidth()/64+1,5.0f);}
        g.setColour(ModernAppleLookAndFeel::secondaryText()); g.setFont(ModernTypography::font(10));
        g.drawText(number(response.floorDb)+" .. "+number(response.ceilingDb)+" dB / fixed K-power scale",
            legendBounds.withTrimmedTop(6),juce::Justification::centred);
    }
}
void SpatialEnergyPanelComponent::mouseDown(const juce::MouseEvent& e)
{
    dragOrigin=e.position; dragYaw=yaw; dragPitch=pitch;
    if (inspectSphere && sphereBounds.contains(e.position)) grabKeyboardFocus();
}
void SpatialEnergyPanelComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (!inspectSphere || !sphereBounds.contains(dragOrigin)) return;
    yaw=dragYaw+(e.position.x-dragOrigin.x)*0.01; pitch=std::clamp(dragPitch+(e.position.y-dragOrigin.y)*0.01,-1.5,1.5); repaint();
}
void SpatialEnergyPanelComponent::mouseWheelMove(const juce::MouseEvent& e,const juce::MouseWheelDetails& w)
{ if(inspectSphere && sphereBounds.contains(e.position)) {zoom=std::clamp(zoom+w.deltaY,0.5,1.4); repaint();} }
bool SpatialEnergyPanelComponent::keyPressed(const juce::KeyPress& key)
{
    if (!inspectSphere || !hasKeyboardFocus(false)) return false;
    if(key==juce::KeyPress::leftKey) yaw-=0.1;
    else if(key==juce::KeyPress::rightKey) yaw+=0.1;
    else if(key==juce::KeyPress::upKey) pitch=std::min(1.5,pitch+0.1);
    else if(key==juce::KeyPress::downKey) pitch=std::max(-1.5,pitch-0.1);
    else if(key.getTextCharacter()=='+') zoom=std::min(1.4,zoom+0.1);
    else if(key.getTextCharacter()=='-') zoom=std::max(0.5,zoom-0.1);
    else return false;
    repaint(); return true;
}
}
