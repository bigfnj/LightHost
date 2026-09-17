#pragma once

#include "SignalMetering.hpp"

#include <juce_audio_devices/juce_audio_devices.h>

//==============================================================================
// Metering the device's own input and output.
//
// Separate from SignalMetering.hpp because this is the only part that needs
// juce_audio_devices, and the unit test target does not link it. Keeping Meter
// and Probe free of that dependency is what lets them be tested at all.
//
// Always active. The input clip latch has to catch a level that went wrong
// while nobody was looking -- that is the incident this whole feature exists
// for -- and the peak half of the measurement is SIMD and effectively free.
// The expensive half, RMS, only runs while a visible meter holds a Meter::Watch.
//==============================================================================
namespace lighthost::metering
{
    /** Wraps the audio callback to measure the device input and output.

        A decorator rather than a second callback registered alongside the
        player, because AudioDeviceManager hands the FIRST callback the real
        output buffer and gives every later one a temporary buffer that it sums.
        A second callback would therefore measure its own silence and report an
        output level of nothing at all.
    */
    class DeviceTap final : public juce::AudioIODeviceCallback
    {
    public:
        explicit DeviceTap (juce::AudioIODeviceCallback& next) : wrapped (next) {}

        [[nodiscard]] Meter& getInputMeter()  noexcept { return inputMeter; }
        [[nodiscard]] Meter& getOutputMeter() noexcept { return outputMeter; }

        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                               int numInputChannels,
                                               float* const* outputChannelData,
                                               int numOutputChannels,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext& context) override
        {
            inputMeter.measure (inputChannelData, numInputChannels, numSamples);

            wrapped.audioDeviceIOCallbackWithContext (inputChannelData, numInputChannels,
                                                      outputChannelData, numOutputChannels,
                                                      numSamples, context);

            // After the forwarded call, so this is what the device will play
            // rather than whatever was left in the buffer.
            outputMeter.measure (outputChannelData, numOutputChannels, numSamples);
        }

        void audioDeviceAboutToStart (juce::AudioIODevice* device) override
        {
            // The device knows its rate and buffer size, so the peak decay is
            // pointed at them rather than left on the reference literal. Without
            // this the device meters fell at 45 dB/s only at 480 samples and
            // 48 kHz, and buffer size is a setting the user picks -- see
            // Meter::setTimebase. Before the wrapped callback, so the meters are
            // configured and cleared prior to any audio arriving.
            if (device != nullptr)
            {
                const auto rate = device->getCurrentSampleRate();
                const auto size = device->getCurrentBufferSizeSamples();

                inputMeter.setTimebase (rate, size);
                outputMeter.setTimebase (rate, size);
            }

            inputMeter.reset();
            outputMeter.reset();
            wrapped.audioDeviceAboutToStart (device);
        }

        void audioDeviceStopped() override
        {
            wrapped.audioDeviceStopped();
            inputMeter.reset();
            outputMeter.reset();
        }

        void audioDeviceError (const juce::String& errorMessage) override
        {
            wrapped.audioDeviceError (errorMessage);
        }

    private:
        juce::AudioIODeviceCallback& wrapped;
        Meter inputMeter;
        Meter outputMeter;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeviceTap)
    };
}
