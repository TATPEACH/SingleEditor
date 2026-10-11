// Included by the device-native test fixture after its controlled IODevice.
void runSpatialEnergyVisualMathTests()
{
    auto layout=std::make_shared<SpatialEnergyMeter::Layout>(); layout->geometryCurrent=true;
    SpatialEnergyMeter::Channel channel; channel.physicalOutput=3;
    channel.placement=SpatialEnergyMeter::Placement::directional;
    layout->channels={channel,channel};
    SpatialEnergyMeter::Snapshot snapshot; snapshot.layout=layout; snapshot.channels.resize(2);
    snapshot.channels[0].kWeightedMeanSquare=0.01; snapshot.channels[1].kWeightedMeanSquare=0.01;
    const auto field=SpatialEnergyVisualField::prepare(layout,false);
    require(!field->map.empty() && field->sphere.empty(), "2D creates no sphere resources");
    SpatialEnergyVisualField::Point point; point.weights[0]=1; point.weights[1]=1;
    require(std::abs(SpatialEnergyMeter::powerDb(SpatialEnergyVisualField::power(point,snapshot))+16.9897000434)<1e-8,
        "independent feed powers sum to +3.0103 dB, not coherent amplitudes");
    const auto summary=SpatialEnergyVisualSummary::from(snapshot);
    require(summary.directional && std::abs(summary.azimuth)<1e-6 && summary.spread<1e-6,
        "coincident feed anchors yield forward concentrated direction");
    layout->channels[1].azimuthDegrees=180;
    const auto diffuse=SpatialEnergyVisualSummary::from(snapshot);
    require(!diffuse.directional && diffuse.spread>0.999, "opposite equal feeds have no invented dominant direction");
    const auto pole=SpatialEnergyVisualField::project(0,90), front=SpatialEnergyVisualField::project(0,0);
    require(SpatialEnergyVisualField::project(30,0)[0]<0 && SpatialEnergyVisualField::project(-30,0)[0]>0,
        "positive azimuth is listener-left, matching the project's existing geometry convention");
    require(std::abs(pole[0])<1e-6 && std::abs(pole[1]-1)<1e-6 && std::abs(front[0])+std::abs(front[1])<1e-6,
        "Hammer projection preserves poles and front reference");
    require(!SpatialEnergyVisualField::prepare(layout,true)->sphere.empty(), "explicit 3D prepares separate inspection geometry");
    layout->geometryCurrent=false;
    require(SpatialEnergyVisualField::prepare(layout,true)->map.empty()
        && !SpatialEnergyVisualSummary::from(snapshot).directional, "stale geometry creates no inferred directional field");
}

void runSpatialEnergyEngineTests(const juce::File& artifacts)
{
    runSpatialEnergyVisualMathTests();
    using Transport = SpatialEnergyMeter::Transport;
    Controls controls; controls.managed = true;
    SingleTrackDocument document;
    auto engine = std::make_unique<AudioEngine>(document);
    require(!engine->acquireSpatialEnergyAnalysisLease(false) && !engine->getSpatialEnergyDiagnostics().workerRunning,
        "Spatial Energy pre-Core request creates no producer");
    require(engine->reserveExplicitStartupOutput(), "reserve fixture without warmup");
    const auto source = artifacts.getChildFile("spatial-energy-source.wav");
    constexpr int frames = 480000;
    juce::MemoryOutputStream wave;
    wave.write("RIFF", 4); wave.writeInt(36 + frames * 4); wave.write("WAVEfmt ", 8);
    wave.writeInt(16); wave.writeShort(1); wave.writeShort(2); wave.writeInt(48000);
    wave.writeInt(192000); wave.writeShort(4); wave.writeShort(16); wave.write("data", 4); wave.writeInt(frames * 4);
    for (int frame = 0; frame < frames; ++frame)
    {
        const auto value = static_cast<short>(std::lround(32767.0 * std::pow(10.0, -23.0 / 20.0)
            * std::sin(juce::MathConstants<double>::twoPi * 1000.0 * frame / 48000.0)));
        wave.writeShort(value); wave.writeShort(value);
    }
    require(source.replaceWithData(wave.getData(), wave.getDataSize()) && engine->loadFile(source), "use real waveform Document");
    engine->setMonitorGainLinear(1);
    const auto geometry = AmbisonicStandardMatrixCatalog::standardProfile("standard-stereo-v1");
    require(geometry && geometry->speakers.size() == 2, "use actual standard geometry");
    DeviceRoutePlan::Bus bus {geometry->profileId.toStdString(), geometry->revision, {}};
    for (const auto& speaker : geometry->speakers) bus.speakers.push_back({speaker.stableId.toStdString(), speaker.role, true});
    FixtureOutput* view = nullptr;
    const auto install = [&](double rate)
    {
        auto lease = engine->takeDeviceLeaseForSwitch();
        require(lease && lease->closeForSwitch(), "take exact output owner");
        DeviceOutput::Snapshot target {"Fixture Direct", "spatial:device", "Spatial fixture", rate, 128, 16,
            {1, 3}, {"0", "1", "2", "3"}, 0, 96};
        auto direct = std::make_unique<FixtureOutput>(controls, target); view = direct.get();
        auto draft = DeviceRoutePlan::propose(bus, target); draft.outputsBySpeaker = {{1}, {3}};
        const auto plan = DeviceRoutePlan::validate(draft, target, bus).approved();
        std::unique_ptr<DeviceOutput> output = std::move(direct);
        require(plan && AudioEngineDeviceTestAccess::commit(*engine, lease, output, plan, bus), "commit real final route");
        lease.reset();
    };
    install(48000);
    require(!engine->getSpatialEnergyDiagnostics().workerRunning && engine->getSpatialEnergyDiagnostics().preparations == 0,
        "ordinary source/route/device setup keeps optional producer dormant");
    auto first = engine->acquireSpatialEnergyAnalysisLease(true);
    auto second = engine->acquireSpatialEnergyAnalysisLease(true);
    const auto wait = [&](const auto& predicate, const char* message)
    {
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 4000;
        while (!predicate() && juce::Time::getMillisecondCounterHiRes() < deadline) juce::Thread::sleep(2);
        require(predicate(), message);
    };
    wait([&] { return engine->getSpatialEnergyDiagnostics().liveMeters == 1; }, "one shared meter for two leases");
    const auto before = document.getState();
    engine->play();
    const auto render = [&](int blocks) { for (int block = 0; block < blocks; ++block) view->render(); };
    const auto pause = [&]
    {
        std::jthread callbacks([&](std::stop_token stop) { while (!stop.stop_requested()) { view->render(); juce::Thread::sleep(1); } });
        engine->pause(); callbacks.request_stop(); callbacks.join(); render(1);
    };
    render(1200); // 3.2 s through actual source-player callback
    wait([&] { const auto s = engine->getSpatialEnergySnapshot(); return s && s->shortTermReady; }, "real output supplies full M/S windows");
    auto measured = engine->getSpatialEnergySnapshot();
    require(measured->layout->geometryCurrent && measured->channels.size() == 2
        && measured->layout->channels[0].physicalOutput == 1 && measured->layout->channels[1].physicalOutput == 3,
        "snapshot identifies noncontiguous physical ports and active geometry");
    require(measured->momentaryLufs && measured->integratedLufs && std::abs(*measured->momentaryLufs + 23) < .1
        && std::abs(*measured->integratedLufs + 23) < .15 && !measured->discontinuity && document.getState() == before,
        "calibrated post-route signal preserves Document");
    const auto preparations = engine->getSpatialEnergyDiagnostics().preparations;
    for (int poll = 0; poll < 20; ++poll) engine->refreshSpatialEnergyAnalysis();
    require(engine->getSpatialEnergyDiagnostics().preparations == preparations, "unchanged polling never rebuilds filters/geometry");
    pause();
    wait([&] { const auto s = engine->getSpatialEnergySnapshot(); return s && s->transport == Transport::paused; }, "pause reaches worker");
    const auto paused = engine->getSpatialEnergySnapshot();
    render(200); juce::Thread::sleep(60);
    require(engine->getSpatialEnergySnapshot()->measuredSamples == paused->measuredSamples
        && engine->getSpatialEnergySnapshot()->momentaryLufs == paused->momentaryLufs, "pause retains trusted measurement");
    require(document.saveToFile(source), "save real fixture document in place");
    render(1); juce::Thread::sleep(60);
    require(engine->getSpatialEnergySnapshot()->epoch == paused->epoch, "Save without audio change preserves I");
    engine->seekToSeconds(1); render(1);
    wait([&] { const auto s = engine->getSpatialEnergySnapshot(); return s && s->epoch != paused->epoch; }, "Seek changes epoch while paused");
    require(!engine->getSpatialEnergySnapshot()->momentaryReady && !engine->getSpatialEnergySnapshot()->integratedLufs,
        "Seek clears old windows and integral");
    engine->play(); render(225);
    wait([&] { const auto s = engine->getSpatialEnergySnapshot(); return s && s->momentaryReady; }, "resume fills new window");
    engine->setSourceChannelMuted(1, true); render(225);
    wait([&] { const auto s = engine->getSpatialEnergySnapshot(); return s && s->channels[1].rmsDbfs < -100; }, "mute changes real final output energy");
    engine->setSourceChannelMuted(1, false);
    first = {};
    require(engine->getSpatialEnergyDiagnostics().consumers == 1 && engine->getSpatialEnergyDiagnostics().workerRunning,
        "first release retains second view's producer");
    pause();
    const auto oldLayout = engine->getSpatialEnergySnapshot()->layout->generation;
    install(96000); engine->refreshSpatialEnergyAnalysis();
    require(!engine->getSpatialEnergySnapshot(), "new device preparation hides old snapshot");
    wait([&] { render(1); const auto s = engine->getSpatialEnergySnapshot(); return s && s->layout->sampleRate == 96000; },
        "new clock is actually published before starting its run");
    engine->play(); render(450); // .6 s at 96 kHz
    wait([&] { const auto s = engine->getSpatialEnergySnapshot(); return s && s->momentaryReady && s->layout->sampleRate == 96000; }, "new device clock measured");
    measured = engine->getSpatialEnergySnapshot();
    require(measured->layout->generation != oldLayout && !measured->shortTermReady
        && measured->momentaryLufs && std::abs(*measured->momentaryLufs + 23) < .1, "switch never mixes window histories");
    {
        std::jthread callbacks([&](std::stop_token stop) { while (!stop.stop_requested()) { view->render(); juce::Thread::sleep(1); } });
        second = {}; // last visible view disappears while real callbacks continue
        callbacks.request_stop(); callbacks.join();
    }
    require(!engine->getSpatialEnergyDiagnostics().workerRunning && engine->getSpatialEnergyDiagnostics().liveMeters == 0
        && engine->getSpatialEnergyDiagnostics().consumers == 0 && !engine->getSpatialEnergySnapshot(), "last release destroys worker/filters/snapshots");
    const auto processed = engine->getSpatialEnergyDiagnostics().callbackBlocks;
    render(20);
    require(engine->getSpatialEnergyDiagnostics().callbackBlocks == processed, "zero consumers means no measurement work");
    {
        SpatialEnergyPanelComponent panel(*engine);
        panel.setSize(640,560); panel.addToDesktop(juce::ComponentPeer::windowHasTitleBar); panel.setVisible(true);
        require(!panel.hasViewLease() && !panel.hasVisualWorker() && !panel.isViewTimerRunning(), "visible panel cannot bypass actual CoreReady admission");
        panel.applyWorkspaceVisualContract(RendererDensity::expanded,RendererMagSize::large,RendererInformationTier::detail,true);
        panel.setCoreReady(true);
        const auto tick=[&] { render(30); SpatialEnergyPanelTestAccess::tick(panel); juce::Thread::sleep(55); };
        const auto until=[&](const auto& predicate,const char* message)
        { const auto deadline=juce::Time::getMillisecondCounterHiRes()+4000; while(!predicate() && juce::Time::getMillisecondCounterHiRes()<deadline) tick(); require(predicate(),message); };
        until([&]{ return panel.getAccessibilityValueSummary().contains("-23.0 LUFS"); }, "native panel reads the real latest physical snapshot");
        require(panel.currentView()==SpatialEnergyPanelComponent::View::expanded && panel.hasVisualWorker() && !panel.hasSphereResource(), "Expanded defaults to 2D with no sphere allocation");
        auto* handler=panel.getAccessibilityHandler();
        require(handler && handler->getValueInterface() && handler->getValueInterface()->isReadOnly(), "UIA panel exposes read-only measurement value");
        const auto savePicture=[&](const char* name,float scale=1.0f)
        { auto file=artifacts.getChildFile(name).createOutputStream(); require(file && juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds(),true,scale),*file), "save real native spatial panel screenshot"); };
        const auto identity=document.getState(); const auto epoch=engine->getSpatialEnergySnapshot()->epoch;
        until([&]{return SpatialEnergyPanelTestAccess::fieldReady(panel);},"2D field prepared before visual evidence");
        savePicture("spatial-expanded-2d.png");
        const auto plane=SpatialEnergyPanelTestAccess::planeIdentity(panel);
        panel.setSphereInspection(true);
        require(SpatialEnergyPanelTestAccess::planeIdentity(panel)==plane && SpatialEnergyPanelTestAccess::fieldReady(panel),"requesting 3D retains the ready 2D view");
        until([&]{return panel.hasSphereResource();},"3D is lazy and becomes inspectable");
        require(plane && SpatialEnergyPanelTestAccess::planeIdentity(panel)==plane,"3D adds resources without rebuilding the immutable 2D kernel");
        const auto sphereWeak=SpatialEnergyPanelTestAccess::visual(panel);
        panel.setSphereInspection(false);
        require(!panel.hasSphereResource() && sphereWeak.expired() && SpatialEnergyPanelTestAccess::planeIdentity(panel)==plane,"explicit 3D close immediately retires sphere and retains 2D identity");
        panel.setSphereInspection(true); until([&]{return panel.hasSphereResource();},"3D may reopen on the same plane");
        panel.grabKeyboardFocus();
        require(panel.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)),"focused sphere accepts local camera keys");
        SpatialEnergyPanelTestAccess::focusReading(panel);
        require(!panel.keyPressed(juce::KeyPress(juce::KeyPress::leftKey)),"camera does not capture a display control's arrow key");
        savePicture("spatial-expanded-sphere.png");
        require(document.getState()==identity && engine->getSpatialEnergySnapshot()->epoch==epoch, "display inspection never edits audio or resets I");
        const auto originalTheme=RendererVisualStyle::modernTheme();
        RendererVisualStyle::setModernTheme(ModernTheme::night); tick(); savePicture("spatial-night-150.png",1.5f);
        RendererVisualStyle::setModernTheme(originalTheme);
        panel.setSize(400,330); tick();
        require(panel.currentView()==SpatialEnergyPanelComponent::View::standard && !panel.hasSphereResource(), "shrink releases 3D and preserves the same measurement lease");
        require(SpatialEnergyPanelTestAccess::planeIdentity(panel)==plane,"breakpoint shrink reuses exact 2D artifact");
        until([&]{return SpatialEnergyPanelTestAccess::fieldReady(panel);},"Standard retains a real equal-area main view after shrink");
        savePicture("spatial-standard.png");
        SpatialEnergyPanelTestAccess::overload(panel); tick();
        require(panel.getAccessibilityValueSummary().contains("visual degraded") && !SpatialEnergyPanelTestAccess::fieldReady(panel)
            && engine->getSpatialEnergySnapshot()->epoch==epoch,"visual overload preserves numerical truth and warns while dropping raster");
        savePicture("spatial-degraded.png");
        const auto reduced=RendererVisualStyle::isReduceMotion(); RendererVisualStyle::setReduceMotion(true);
        panel.setSize(232,224); tick();
        require(panel.currentView()==SpatialEnergyPanelComponent::View::compact && !panel.hasVisualWorker()
            && engine->getSpatialEnergyDiagnostics().consumers==1 && engine->getSpatialEnergySnapshot()->epoch==epoch,
            "Compact owns no geometry worker/mesh and does not restart meter");
        savePicture("spatial-compact.png");
        RendererVisualStyle::setReduceMotion(reduced);
        panel.setVisible(false);
        require(!panel.hasViewLease() && !panel.hasVisualWorker() && !panel.isViewTimerRunning()
            && !engine->getSpatialEnergyDiagnostics().workerRunning, "hidden native window releases every producer/timer/visual resource");
        require(panel.getAccessibilityValueSummary().contains("Last measurement"), "inactive UIA retains bounded text without retaining snapshot graphs");
        panel.setVisible(true); until([&]{return panel.hasViewLease();}, "visible window reacquires a fresh lease");
        panel.setCoreReady(false); require(!panel.hasViewLease(), "new document CoreReady reset immediately withdraws the view lease");
        panel.setVisible(false); panel.removeFromDesktop();
    }
    require(document.getState()!=nullptr && !engine->getSpatialEnergyDiagnostics().workerRunning,"native UI teardown leaves no producer");
    auto survivingLease = engine->acquireSpatialEnergyAnalysisLease(true);
    engine.reset(); survivingLease = {};
    require(controls.aliveDevices == 0, "lease may outlive engine teardown without device retention");
    std::cout << "Spatial Energy engine passed: post-Core gate, final signal, leases, pause/Save/Seek/mute, 48-to-96k switch, zero-consumer and teardown\n";
}

void runSpatialEnergyMultichannelTests(const juce::File& artifacts)
{
    Controls controls; controls.managed=true;
    SingleTrackDocument document; AudioEngine engine(document);
    require(engine.reserveExplicitStartupOutput(),"multichannel fixture reserves its own output");
    const auto source=artifacts.getChildFile("spatial-multichannel-source.wav");
    constexpr int channels=12, rate=48000, frames=rate*12;
    juce::MemoryOutputStream wav;
    wav.write("RIFF",4); wav.writeInt(36+frames*channels*2); wav.write("WAVEfmt ",8);
    wav.writeInt(16); wav.writeShort(1); wav.writeShort(channels); wav.writeInt(rate);
    wav.writeInt(rate*channels*2); wav.writeShort(channels*2); wav.writeShort(16);
    wav.write("data",4); wav.writeInt(frames*channels*2);
    for(int frame=0;frame<frames;++frame) for(int channel=0;channel<channels;++channel)
        wav.writeShort(static_cast<short>(std::lround(32767*std::pow(10.0,(-22.0-channel)/20)
            *std::sin(juce::MathConstants<double>::twoPi*(700.0+channel*50)*frame/rate))));
    require(source.replaceWithData(wav.getData(),wav.getDataSize()) && engine.loadFile(source),"load real twelve-channel PCM source");
    engine.setMonitorGainLinear(1);
    const auto documentIdentity=document.getState();
    const auto originalTheme=RendererVisualStyle::modernTheme();
    const auto originalMotion=RendererVisualStyle::isReduceMotion();
    struct RestoreVisualPreferences
    {
        ModernTheme theme; bool motion;
        ~RestoreVisualPreferences(){RendererVisualStyle::setModernTheme(theme);RendererVisualStyle::setReduceMotion(motion);}
    } restore {originalTheme,originalMotion};
    for (int scenario=0;scenario<6;++scenario)
    {
        const auto id=scenario==0 ? "standard-5.1-v1" : scenario==1 ? "standard-7.1-v1" : "standard-7.1.4-v1";
        const auto standard=AmbisonicStandardMatrixCatalog::standardProfile(id);
        require(standard!=nullptr,"use actual shipped multichannel geometry");
        auto geometry=*standard;
        if(scenario>=3)
        {
            geometry.profileId="spatial-custom-native"; geometry.revision=10;
            geometry.builtInReadOnly=false;
            geometry.speakers[0].azimuthDegrees=42;
            geometry.speakers[1].azimuthDegrees=-21;
            const auto generated=AmbisonicSpeakerGeometryGenerator::generate({1,geometry.revision,1,geometry});
            require(generated.succeeded() && engine.activateAmbisonicDecodeMatrixArtifact(generated.artifact),"activate real custom geometry artifact");
        }
        else engine.clearAmbisonicDecodeMatrixArtifact();
        auto lease=engine.takeDeviceLeaseForSwitch(); require(lease && lease->closeForSwitch(),"prepare exact multichannel output owner");
        DeviceOutput::Snapshot device {"Fixture Direct","spatial:multichannel","Multichannel fixture",rate,128,16,{},{},0,96};
        DeviceRoutePlan::Bus bus {geometry.profileId.toStdString(),geometry.revision,{}};
        for(std::size_t i=0;i<geometry.speakers.size();++i)
        {
            const auto& speaker=geometry.speakers[i]; bus.speakers.push_back({speaker.stableId.toStdString(),speaker.role,true});
            device.physicalOutputs.push_back(static_cast<int>(i*2+1));
            device.outputNames.push_back("Inactive"); device.outputNames.push_back(speaker.label.toStdString());
        }
        auto draft=DeviceRoutePlan::propose(bus,device);
        for(std::size_t i=0;i<bus.speakers.size();++i) draft.outputsBySpeaker[i]={static_cast<int>(i*2+1)};
        if(scenario==4)
        { draft.outputsBySpeaker[1]={1}; draft.outputsBySpeaker.back().clear(); }
        const auto route=DeviceRoutePlan::validate(draft,device,bus).approved(true);
        auto direct=std::make_unique<FixtureOutput>(controls,device); auto* outputView=direct.get();
        std::unique_ptr<DeviceOutput> output=std::move(direct);
        require(route && AudioEngineDeviceTestAccess::commit(engine,lease,output,route,bus),"commit approved many-channel physical route");
        lease.reset();
        engine.seekToSeconds(0); engine.play();
        const auto assertBinding=[&](bool stale)
        {
            const auto before=engine.getSpatialEnergyDiagnostics();
            const auto opens=controls.opens.load(), scans=controls.scans.load();
            const auto captured=snapshot(engine,document,"audio",true);
            const auto fields=captured["fields"];
            require(fields["outputBindingState"]["value"].toString()==(stale
                ? "approved-route-unresolved-geometry" : "approved-route-current-geometry"),
                "serialized diagnostic binding identifies exact current or stale geometry");
            require(static_cast<int>(fields["physicalRouteSourceChannels"]["value"])==static_cast<int>(bus.speakers.size())
                && static_cast<juce::int64>(fields["outputBindingRevision"]["value"])==bus.revision,
                "diagnostics retain the approved bus revision and source width");
            const auto assertNumbers=[&](const char* name,const juce::Array<juce::var>& expected)
            {
                const auto value=fields[name]["value"]; const auto* actual=value.getArray();
                require(actual && actual->size()==expected.size(),"physical diagnostic array has its complete expected shape");
                for(int i=0;i<expected.size();++i)
                    require(std::abs(static_cast<double>((*actual)[i])-static_cast<double>(expected[i]))<1e-9,
                        "physical diagnostic array matches the actual configured connection and geometry");
            };
            juce::Array<juce::var> mapping,connections,angles,lfe,unplaced;
            for(const auto physical:device.physicalOutputs) mapping.add(physical);
            for(std::size_t row=0;row<draft.outputsBySpeaker.size();++row)
                for(const auto physical:draft.outputsBySpeaker[row])
                {connections.add(static_cast<int>(row));connections.add(physical);}
            for(std::size_t row=0;row<geometry.speakers.size();++row)
            {
                const auto physical=static_cast<int>(row*2+1);
                if(stale || (scenario==4 && (row==0 || row==1 || row==geometry.speakers.size()-1)))
                {unplaced.add(physical);continue;}
                const auto& speaker=geometry.speakers[row];
                if(speaker.role==SpeakerRole::LFE) lfe.add(physical);
                else {angles.add(physical);angles.add(speaker.azimuthDegrees);angles.add(speaker.elevationDegrees);}
            }
            assertNumbers("channelMapping",mapping); assertNumbers("physicalRouteConnections",connections);
            assertNumbers("lfePhysicalOutputs",lfe); assertNumbers("unplacedPhysicalOutputs",unplaced);
            if(stale)
                require(fields["speakerGeometry"]["unavailable"].toString()=="not-reported"
                    && fields["speakerLayout"]["unavailable"].toString()=="not-reported",
                    "new custom revision cannot relabel the old approved physical binding");
            else assertNumbers("speakerGeometry",angles);
            require(fields["speakerLayoutSource"]["value"].toString()=="approved-output-binding"
                && fields["speakerMaskSource"]["unavailable"].toString()=="not-reported"
                && fields["deviceMetadataState"]["value"].toString()=="not-supported",
                "native endpoint enrichment coexists with binding geometry without duplicate fields");
            const auto spatial=snapshot(engine,document,"spatial");
            require(spatial["fields"]["outputMatrix"]["unavailable"].toString()=="not-supported"
                && spatial["fields"]["routingObservation"]["value"].toString()=="direct-render-not-observed",
                "direct physical binding is not misrepresented as a measured Document render matrix");
            const auto after=engine.getSpatialEnergyDiagnostics();
            require(before.consumers==after.consumers && before.workerRunning==after.workerRunning
                && before.preparations==after.preparations && before.callbackBlocks==after.callbackBlocks
                && controls.opens==opens && controls.scans==scans && document.getState()==documentIdentity,
                "passive route export neither starts optional analysis nor enumerates, opens or renders a device");
            const auto text=juce::JSON::toString(captured,true);
            require(!text.contains("spatial-custom-native") && !text.contains("Multichannel fixture")
                && !text.contains("spatial:multichannel"),"default route export omits custom and device identifiers");
            require(artifacts.getChildFile("physical-binding-"+juce::String(scenario)+(stale ? "-stale" : "")+".json").replaceWithText(text),
                "retain real native physical-binding diagnostic receipt");
        };
        require(engine.getSpatialEnergyDiagnostics().consumers==0 && !engine.getSpatialEnergyDiagnostics().workerRunning,
            "physical binding capture is exercised before any optional spatial view exists");
        assertBinding(false);
        {
            SpatialEnergyPanelComponent panel(engine); panel.setSize(860,660);
            panel.addToDesktop(juce::ComponentPeer::windowHasTitleBar); panel.setVisible(true);
            panel.applyWorkspaceVisualContract(RendererDensity::expanded,RendererMagSize::large,RendererInformationTier::detail,true);
            panel.setCoreReady(true);
            const auto tick=[&]{for(int block=0;block<50;++block)outputView->render(); SpatialEnergyPanelTestAccess::tick(panel);juce::Thread::sleep(55);};
            const auto until=[&](const auto& predicate,const char* message)
            {const auto deadline=juce::Time::getMillisecondCounterHiRes()+5000;while(!predicate() && juce::Time::getMillisecondCounterHiRes()<deadline)tick();require(predicate(),message);};
            until([&]{const auto s=engine.getSpatialEnergySnapshot();return s && s->momentaryReady && SpatialEnergyPanelTestAccess::fieldReady(panel);},"real multichannel signal reaches spatial view");
            tick(); // allow the UI to consume the ready worker publication
            const auto measured=engine.getSpatialEnergySnapshot();
            require(measured->channels.size()==geometry.speakers.size() && measured->layout->geometryCurrent,"measured physical outputs match the current multichannel geometry");
            require(SpatialEnergyPanelTestAccess::rowCount(panel)==static_cast<int>(geometry.speakers.size()),"every physical output has a row");
            require(SpatialEnergyPanelTestAccess::rowsReadOnly(panel),"physical rows expose readonly keyboard/UIA values");
            require(SpatialEnergyPanelTestAccess::rowTraversalKeepsFocusVisible(panel),"forward and backward keyboard traversal reveals every physical row");
            int lfe=-1;
            for(std::size_t i=0;i<measured->layout->channels.size();++i)
                if(measured->layout->channels[i].placement==SpatialEnergyMeter::Placement::lfe) lfe=static_cast<int>(i);
            require(lfe>=0 && std::isfinite(measured->channels[static_cast<std::size_t>(lfe)].contributionDb)
                && panel.getAccessibilityValueSummary().contains("LFE (non-directional)"),"actual LFE feed retains independent numerical truth");
            const auto visual=SpatialEnergyVisualField::prepare(measured->layout,false);
            for(const auto& point:visual->mapPoints()) require(point.weights[static_cast<std::size_t>(lfe)]==0,"LFE never colours directional field");
            if(scenario==4)
                require(measured->layout->unmappedSpeakerCount==1 && measured->layout->channels[0].ambiguous
                    && measured->layout->channels[0].placement==SpatialEnergyMeter::Placement::unplaced
                    && !measured->momentaryLufs && panel.getAccessibilityValueSummary().contains("ambiguous"),
                    "many-to-one and unmapped/unplaced stay explicit; unknown physical weights cannot invent global LUFS");
            if(scenario==5)
            {
                ++geometry.revision; geometry.speakers[0].azimuthDegrees+=5;
                const auto replacement=AmbisonicSpeakerGeometryGenerator::generate({2,geometry.revision,1,geometry});
                require(replacement.succeeded() && engine.activateAmbisonicDecodeMatrixArtifact(replacement.artifact),"publish a real later geometry revision without rewriting binding");
                until([&]{const auto s=engine.getSpatialEnergySnapshot();return s && !s->layout->geometryCurrent
                    && panel.getAccessibilityValueSummary().contains("geometry missing/stale");},"stale binding reaches honest non-directional UI");
                require(!SpatialEnergyPanelTestAccess::fieldReady(panel),"stale geometry withdraws directional raster");
                assertBinding(true);
            }
            const auto label=scenario==0 ? "5-1" : scenario==1 ? "7-1" : scenario==2 ? "7-1-4" : scenario==3 ? "custom" : scenario==4 ? "unplaced" : "stale";
            const auto capture=[&](const juce::String& suffix)
            {auto stream=artifacts.getChildFile("spatial-"+juce::String(label)+suffix+".png").createOutputStream();require(stream && juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds()),*stream),"retain actual multichannel native view");};
            capture("-expanded");
            if(scenario==2)
            {
                const auto initialEpoch=engine.getSpatialEnergySnapshot()->epoch;
                const auto preparations=engine.getSpatialEnergyDiagnostics().preparations;
                juce::Array<juce::var> views;
                for(int mode=0;mode<4;++mode)
                {
                    panel.setSize(mode==0 ? 232 : mode==1 ? 460 : 860,mode==0 ? 224 : mode==1 ? 380 : 660);
                    panel.setSphereInspection(mode==3); tick();
                    if(mode>0) until([&]{return SpatialEnergyPanelTestAccess::fieldReady(panel) && (mode!=3 || panel.hasSphereResource());},"resource probe uses a prepared visible field");
                    juce::Image target(juce::Image::ARGB,panel.getWidth(),panel.getHeight(),true);
                    juce::Array<juce::var> samples;
                    for(int frame=0;frame<12;++frame)
                    {
                        for(int block=0;block<25;++block) outputView->render();
                        const auto t0=juce::Time::getMillisecondCounterHiRes();
                        SpatialEnergyPanelTestAccess::tick(panel);
                        const auto t1=juce::Time::getMillisecondCounterHiRes();
                        { juce::Graphics graphics(target); panel.paintEntireComponent(graphics,true); }
                        const auto t2=juce::Time::getMillisecondCounterHiRes();
                        juce::DynamicObject::Ptr sample=new juce::DynamicObject;
                        sample->setProperty("tickMs",t1-t0); sample->setProperty("softwarePaintMs",t2-t1);
                        samples.add(juce::var(sample.get())); juce::Thread::sleep(55);
                    }
                    auto facts=SpatialEnergyPanelTestAccess::visualResources(panel);
                    facts.getDynamicObject()->setProperty("view",mode==0 ? "compact" : mode==1 ? "standard" : mode==2 ? "expanded-2d" : "expanded-sphere");
                    facts.getDynamicObject()->setProperty("samples",samples); views.add(facts);
                }
                require(engine.getSpatialEnergySnapshot()->epoch==initialEpoch
                    && engine.getSpatialEnergyDiagnostics().preparations==preparations
                    && document.getState()==documentIdentity,"active view resource sampling never prepares a second meter or resets programme/audio state");
                juce::DynamicObject::Ptr report=new juce::DynamicObject;
                report->setProperty("schema","singleeditor.spatial-energy.active-view-resources.v1");
                report->setProperty("scope","instrumented native UI tick and software paint, real 12-channel signal; not hardware/compositor/CoreReady or G020 deadline acceptance");
                report->setProperty("views",views);
                require(artifacts.getChildFile("spatial-active-view-resources.json").replaceWithText(juce::JSON::toString(juce::var(report.get()))),"retain raw active-view resource samples");
                panel.setSphereInspection(false); tick();
            }
            const auto epoch=engine.getSpatialEnergySnapshot()->epoch;
            SpatialEnergyPanelTestAccess::displayControls(panel); tick();
            require(engine.getSpatialEnergySnapshot()->epoch==epoch && document.getState()==documentIdentity,"M/S and display floor controls never change audio/Document/I epoch");
            SpatialEnergyPanelTestAccess::momentaryReadout(panel);
            panel.setSize(460,380); tick(); capture("-standard");
            panel.setSize(232,224); tick(); capture("-compact");
            require(!panel.hasVisualWorker() && !panel.hasSphereResource() && engine.getSpatialEnergyDiagnostics().consumers==1,"compact multichannel view retains only shared measurement");
            panel.setVisible(false); panel.removeFromDesktop();
        }
        require(!engine.getSpatialEnergyDiagnostics().workerRunning,"multichannel panel teardown releases producer");
    }
    std::cout << "Spatial Energy multichannel passed: real twelve-channel source, 5.1/7.1/7.1.4/custom/stale/convergence, LFE exclusion, readonly UIA and display controls\n";
}
