# D107 / D108. Applied only to the hash-pinned 8.0.12 build-local ASIO copy.
# Real IASIO fixture evidence: asio-clock-driver-red-2.log. Shared JUCE is not edited.
function(single_editor_asio_replace_exact variable old_text new_text)
    string(FIND "${${variable}}" "${old_text}" position)
    if(position LESS 0)
        message(FATAL_ERROR "Reviewed JUCE ASIO clock patch anchor changed; review upstream before rebuilding")
    endif()
    string(REPLACE "${old_text}" "${new_text}" replaced "${${variable}}")
    set(${variable} "${replaced}" PARENT_SCOPE)
endfunction()

function(single_editor_patch_asio_clock input_variable output_variable)
    set(source "${${input_variable}}")
    string(REPLACE "\r\n" "\n" source "${source}")
    single_editor_asio_replace_exact(source [=[            if (err == ASE_NoClock && numClockSources > 0)
            {
                JUCE_ASIO_LOG ("trying to set a clock source");
                err = asioObject->setClockSource (clocks[0].index);
                JUCE_ASIO_LOG_ERROR ("setClockSource2", err);
                Thread::sleep (10);
                err = asioObject->setSampleRate (newRate);
                JUCE_ASIO_LOG_ERROR ("setSampleRate", err);
                Thread::sleep (10);
            }
]=] [=[            // Single Editor D107: preserve the driver-selected clock on NoClock.
            // The caller validates the actual rate; it never authorizes another clock.
]=])
    single_editor_asio_replace_exact(source [=[            JUCE_ASIO_LOG ("setting clock source");
            auto err = asioObject->setClockSource (clocks[0].index);
            JUCE_ASIO_LOG_ERROR ("setClockSource1", err);
            Thread::sleep (20);
]=] [=[            // An unreported current clock is unknown, not permission to select one.
            JUCE_ASIO_LOG ("current clock source is not reported");
]=])
    single_editor_asio_replace_exact(source [=[                        if (currentRate < 1.0 || currentRate > 192001.0)
                        {
                            JUCE_ASIO_LOG ("setting default sample rate");
                            err = asioObject->setSampleRate (44100.0);
                            JUCE_ASIO_LOG_ERROR ("setting sample rate", err);

                            currentRate = getSampleRate();
                        }
]=] [=[                        if (! std::isfinite (currentRate) || currentRate <= 0.0)
                        {
                            error = "Driver reported an invalid sample rate";
                            removeCurrentDriver();
                            return error;
                        }
]=])
    single_editor_asio_replace_exact(source [=[    void updateSampleRates()
]=] [=[    // Control-thread, read-only Single Editor binding. No cached/default clock guess.
    std::optional<singleeditor::AsioClockState> readClockStateForSingleEditor() const
    {
        if (asioObject == nullptr) return {};
        ASIOClockSource reported[32] = {};
        long count = 32;
        if (asioObject->getClockSources (reported, &count) != ASE_OK || count < 0 || count > 32) return {};
        singleeditor::AsioClockState result;
        int currentCount = 0;
        for (long i = 0; i < count; ++i)
        {
            auto& clock = reported[i];
            const auto isCurrent = clock.isCurrentSource != ASIOFalse;
            result.sources.push_back ({ (int) clock.index,
                convertASIOString (clock.name, sizeof (clock.name)).toStdString(), isCurrent });
            if (isCurrent) { ++currentCount; result.currentIndex = (int) clock.index; }
        }
        if (currentCount != 1) result.currentIndex.reset();
        return result;
    }

    void updateSampleRates()
]=])
    set(${output_variable} "${source}" PARENT_SCOPE)
endfunction()
