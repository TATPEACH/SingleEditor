# D109 opt-in event bridge, applied to the same pinned build-local source.
# Application owner integration must arm the bridge only once it can revalidate.
function(single_editor_patch_asio_events input_variable output_variable)
    set(source "${${input_variable}}")
    single_editor_asio_replace_exact(source [=[    void updateSampleRates()
]=] [=[    std::shared_ptr<const singleeditor::DeviceOutput::DriverIdentity> copyDriverIdentityForSingleEditor() const
    {
        return managedDriverIdentity.load (std::memory_order_acquire);
    }

    bool armEventsForSingleEditor (std::uint64_t generation)
    {
        if (deviceIsOpen || currentCallback != nullptr) return false;
        stopTimer();
        return managedEvents.arm (generation);
    }

    singleeditor::AsioDriverEvents::Snapshot readEventsForSingleEditor (std::uint64_t generation, bool consume, bool includeCounters)
    {
        return managedEvents.read (generation, consume, includeCounters);
    }

    bool requestResetForSingleEditor (std::uint64_t generation)
    {
        if (deviceIsOpen || currentCallback != nullptr || generation == 0 || managedEvents.generation() != generation) return false;
        needToReset = true;
        return true;
    }

    bool releaseDriverForSingleEditor()
    {
        if (deviceIsOpen || currentCallback != nullptr) return false;
        managedEvents.retire();
        stopTimer();
        if (asioObject != nullptr)
        {
            asioObject->stop();
            disposeBuffers();
        }
        return removeCurrentDriver();
    }

    bool prepareDriverForSingleEditor()
    {
        if (deviceIsOpen || currentCallback != nullptr) return false;
        return asioObject != nullptr || (openDevice().isEmpty() && asioObject != nullptr);
    }

    bool refreshCapabilitiesForSingleEditor()
    {
        if (deviceIsOpen || currentCallback != nullptr) return false;
        const auto actual = readActualStateForSingleEditor();
        if (! actual) return false;
        inputChannelNames.clear(); outputChannelNames.clear();
        for (const auto& channelName : actual->inputNames) inputChannelNames.add (channelName);
        for (const auto& channelName : actual->outputNames) outputChannelNames.add (channelName);
        totalNumInputChans = (long) actual->inputNames.size(); totalNumOutputChans = (long) actual->outputNames.size();
        currentSampleRate = actual->sampleRate;
        currentBlockSizeSamples = 0; // No playback stream is open during inspection.
        updateSampleRates();
        return refreshBufferSizes() == ASE_OK;
    }

    std::optional<std::pair<int, int>> readLatenciesForSingleEditor() const
    {
        long input = 0, output = 0;
        if (asioObject == nullptr || asioObject->getLatencies (&input, &output) != ASE_OK || input < 0 || output < 0) return {};
        return std::pair<int, int>{(int) input, (int) output};
    }

    bool showManagedPanelForSingleEditor (std::uint64_t generation)
    {
        if (generation == 0 || managedEvents.generation() != generation || asioObject == nullptr) return false;
        managedEvents.publish (generation, singleeditor::AsioDriverEvents::controlPanel,
            (std::uint64_t) Time::getHighResolutionTicks());
        const auto result = asioObject->controlPanel();
        managedEvents.publish (generation, singleeditor::AsioDriverEvents::controlPanel,
            (std::uint64_t) Time::getHighResolutionTicks());
        return result == ASE_OK;
    }

    void sampleRateChangedForSingleEditor() noexcept
    {
        if (! publishManagedEvent (singleeditor::AsioDriverEvents::sampleRate)) resetRequest();
    }

    bool acknowledgeRevalidationForSingleEditor (std::uint64_t generation)
    {
        return managedEvents.acknowledgeRevalidation (generation);
    }

    std::optional<singleeditor::AsioDriverActualState> readActualStateForSingleEditor() const
    {
        if (asioObject == nullptr) return {};
        singleeditor::AsioDriverActualState result;
        long inputs = 0, outputs = 0, minimum = 0, maximum = 0, preferred = 0, granularity = 0;
        if (asioObject->getSampleRate (&result.sampleRate) != ASE_OK || ! std::isfinite (result.sampleRate) || result.sampleRate <= 0
            || asioObject->getChannels (&inputs, &outputs) != ASE_OK || inputs < 0 || inputs > 512 || outputs <= 0 || outputs > 512
            || asioObject->getBufferSize (&minimum, &maximum, &preferred, &granularity) != ASE_OK
            || minimum <= 0 || maximum < minimum || preferred < minimum || preferred > maximum) return {};
        result.minimumBuffer = (int) minimum; result.maximumBuffer = (int) maximum; result.preferredBuffer = (int) preferred;
        for (int direction = 0; direction < 2; ++direction)
        {
            auto& names = direction == 0 ? result.inputNames : result.outputNames;
            StringArray observedNames;
            for (long index = 0; index < (direction == 0 ? inputs : outputs); ++index)
            {
                ASIOChannelInfo channel = {}; channel.channel = index; channel.isInput = direction == 0 ? ASIOTrue : ASIOFalse;
                if (asioObject->getChannelInfo (&channel) != ASE_OK) return {};
                observedNames.add (convertASIOString (channel.name, sizeof (channel.name)));
            }
            observedNames.trim();
            observedNames.appendNumbersToDuplicates (false, true);
            for (const auto& observedName : observedNames) names.push_back (observedName.toStdString());
        }
        long input = 0, output = 0;
        if (asioObject->getLatencies (&input, &output) == ASE_OK)
        {
            if (input >= 0) result.inputLatency = (int) input;
            if (output >= 0) result.outputLatency = (int) output;
        }
        result.clock = readClockStateForSingleEditor();
        return result;
    }

    void updateSampleRates()
]=])
    single_editor_asio_replace_exact(source [=[    void close() override
    {
        error.clear();
]=] [=[    void close() override
    {
        managedEvents.retire();
        error.clear();
]=])
    single_editor_asio_replace_exact(source [=[    void resetRequest() noexcept
    {
        startTimer (500);
]=] [=[    void resetRequest() noexcept
    {
        if (publishManagedEvent (singleeditor::AsioDriverEvents::reset)) return;
        startTimer (500);
]=])
    single_editor_asio_replace_exact(source [=[    IASIO* asioObject = {};
]=] [=[    IASIO* asioObject = {};
    singleeditor::AsioDriverEvents managedEvents;
    std::atomic<bool> managedEventMode { false };
    std::atomic<std::shared_ptr<const singleeditor::DeviceOutput::DriverIdentity>> managedDriverIdentity;

    void clearDriverIdentityForSingleEditor()
    {
        managedDriverIdentity.store ({}, std::memory_order_release);
    }

    bool publishManagedEvent (singleeditor::AsioDriverEvents::Flag event) noexcept
    {
        if (! managedEventMode.load (std::memory_order_acquire)) return false;
        managedEvents.publish (managedEvents.generation(), event, (std::uint64_t) Time::getHighResolutionTicks());
        return true; // Retired notifications may not fall back to JUCE's timer.
    }
]=])
    single_editor_asio_replace_exact(source [=[    bool removeCurrentDriver()
    {
]=] [=[    bool removeCurrentDriver()
    {
        clearDriverIdentityForSingleEditor();
]=])
    single_editor_asio_replace_exact(source [=[    String initDriver()
    {
]=] [=[    String initDriver()
    {
        clearDriverIdentityForSingleEditor();
]=])
    single_editor_asio_replace_exact(source [=[            asioObject->getDriverName (buffer); // just in case any flimsy drivers expect this to be called
]=] [=[            asioObject->getDriverName (buffer); // preserve JUCE's initialization call
            auto identity = std::make_shared<singleeditor::DeviceOutput::DriverIdentity>();
            identity->product = convertASIOString (buffer, sizeof (buffer)).toStdString();
            try { identity->rawAsioVersion = (std::int32_t) asioObject->getDriverVersion(); }
            catch (...) { identity->versionReadFailed = true; }
            managedDriverIdentity.store (std::move (identity), std::memory_order_release);
]=])
    single_editor_asio_replace_exact(source [=[        return managedEvents.arm (generation);
]=] [=[        const auto armed = managedEvents.arm (generation);
        if (armed) managedEventMode.store (true, std::memory_order_release);
        return armed;
]=])
    single_editor_asio_replace_exact(source [=[            if (currentCallback != nullptr)
            {
                for (int i = 0; i < numActiveInputChans; ++i)
]=] [=[            if (currentCallback != nullptr && ! managedEvents.outputSuspended())
            {
                for (int i = 0; i < numActiveInputChans; ++i)
]=])
    single_editor_asio_replace_exact(source [=[    long asioMessagesCallback (long selector, long value)
    {
        switch (selector)
]=] [=[    long asioMessagesCallback (long selector, long value)
    {
        if (managedEventMode.load (std::memory_order_acquire))
        {
            using Event = singleeditor::AsioDriverEvents;
            switch (selector)
            {
                case kAsioBufferSizeChange: publishManagedEvent (Event::bufferSize); return 1;
                case kAsioResetRequest: publishManagedEvent (Event::reset); return 1;
                case kAsioResyncRequest: publishManagedEvent (Event::resync); return 1;
                case kAsioLatenciesChanged: publishManagedEvent (Event::latency); return 1;
                case kAsioOverload: publishManagedEvent (Event::overload); return 1;
                case kAsioSelectorSupported:
                    if (value == kAsioBufferSizeChange) return 1;
                    break;
            }
        }
        switch (selector)
]=])
    single_editor_asio_replace_exact(source [=[                d->resetRequest();
]=] [=[                d->sampleRateChangedForSingleEditor();
]=])
    set(${output_variable} "${source}" PARENT_SCOPE)
endfunction()
