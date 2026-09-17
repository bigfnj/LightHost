#include "../Source/SignalMetering.hpp"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

//==============================================================================
// The level meters, and the probe that carries one.
//
// Two properties here are load-bearing and neither is obvious from reading the
// class:
//
//   The peak is held and decayed by the meter, not by whoever draws it, and
//   reading does not consume it. A UI frame arrives roughly every 40 ms and a
//   block every 10 ms, so reporting only the last block's peak would discard
//   three in four -- including the one that clipped -- and a destructive read
//   meant two components watching one meter each saw half of what arrived.
//
//   The probe must be perfectly transparent. It sits in the live signal path
//   whenever the signal view is open, so a single altered sample would mean
//   opening a diagnostic window changed what the user sounds like.
//
// The RMS gating is tested because it is the reason this is affordable at all:
// the per-sample work only happens while something is watching, and a meter
// that quietly measured everything forever would break the promise that this
// application costs nothing while tray-resident.
//==============================================================================
namespace
{
    using lighthost::metering::Meter;
    using lighthost::metering::Probe;

    constexpr int    kBlockSize  = 128;
    constexpr double kSampleRate = 48000.0;

    /** Feeds one block of a constant value to a meter. */
    void feedConstant (Meter& meter, float value, int numChannels = 2, int numSamples = kBlockSize)
    {
        juce::AudioBuffer<float> audio (numChannels, numSamples);

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::fill (audio.getWritePointer (ch), value, numSamples);

        meter.measure (audio);
    }

    /** Feeds a block that is silent except for one sample, which is the point:
        a peak meter must catch a single sample, and an RMS meter must not
        report one as a level.
    */
    void feedSingleSpike (Meter& meter, float value)
    {
        juce::AudioBuffer<float> audio (2, kBlockSize);
        audio.clear();
        audio.setSample (0, kBlockSize / 2, value);
        meter.measure (audio);
    }
}

//==============================================================================
class SignalMeteringTests final : public juce::UnitTest
{
public:
    SignalMeteringTests()
        : juce::UnitTest ("Signal metering", "Metering") {}

    void runTest() override
    {
        beginTest ("a fresh meter reads silence, not garbage");
        {
            Meter meter;
            const auto r = meter.read();

            expectWithinAbsoluteError (r.peakDb, lighthost::metering::kFloorDb, 0.01f);
            expect (! r.clipped);
            expect (! r.rmsValid, "nothing has watched it, so there is no RMS to report");
        }

        beginTest ("peak is measured whether or not anything is watching");
        {
            // The clip latch depends on this: a level that goes wrong while the
            // window is shut still has to be caught.
            Meter meter;
            expect (! meter.isWatched());

            feedConstant (meter, 0.5f);
            const auto r = meter.read();

            expectWithinAbsoluteError (r.peakDb, -6.02f, 0.05f);
        }

        beginTest ("RMS is only measured while something is watching");
        {
            Meter meter;

            feedConstant (meter, 0.5f);
            expect (! meter.read().rmsValid, "unwatched RMS should not be computed");

            {
                const Meter::Watch watch (&meter);
                expect (meter.isWatched());

                // Several blocks, because the mean square is smoothed towards
                // the target rather than jumping to it.
                for (int i = 0; i < 40; ++i)
                    feedConstant (meter, 0.5f);

                const auto r = meter.read();
                expect (r.rmsValid);
                expectWithinAbsoluteError (r.rmsDb, -6.02f, 0.2f);
            }

            expect (! meter.isWatched(), "the Watch should have released on scope exit");
        }

        beginTest ("RMS follows the loudest channel, not the average of them");
        {
            // A mono signal wired into a stereo probe leaves one channel silent.
            // Averaging across channels would report it 3 dB quiet, which would
            // look like the plugin had done something.
            Meter meter;
            const Meter::Watch watch (&meter);

            juce::AudioBuffer<float> audio (2, kBlockSize);

            for (int i = 0; i < 40; ++i)
            {
                audio.clear();
                juce::FloatVectorOperations::fill (audio.getWritePointer (0), 0.5f, kBlockSize);
                meter.measure (audio);
            }

            const auto r = meter.read();
            expectWithinAbsoluteError (r.rmsDb, -6.02f, 0.2f,
                                       "a silent second channel must not drag the level down");
        }

        beginTest ("the peak survives the blocks after it, not just its own");
        {
            Meter meter;

            feedSingleSpike (meter, 0.8f);      // the one that matters
            feedConstant (meter, 0.01f);        // three quiet blocks after it
            feedConstant (meter, 0.01f);
            feedConstant (meter, 0.01f);

            // The spike was -1.94 dBFS and three quiet blocks have passed, so
            // the held peak has decayed by about 3 x 0.45 dB. What matters is
            // that it is still clearly there: reporting only the last block
            // would have shown -40 and lost the event entirely.
            const auto r = meter.read();
            expectWithinAbsoluteError (r.peakDb, -1.94f - 3.0f * 0.45f, 0.15f,
                                       "the spike did not survive the blocks after it, so a "
                                       "peak between two UI frames would be missed");
        }

        beginTest ("reading does not consume the peak, so two readers agree");
        {
            // The device meters have two readers whenever the signal view is
            // open: the meter under the device, and the matching row of the
            // column. When read() cleared the peak, each stole what the other
            // would have shown and both sagged.
            Meter meter;
            feedSingleSpike (meter, 0.8f);

            const auto first  = meter.read();
            const auto second = meter.read();

            expectWithinAbsoluteError (first.peakDb, -1.94f, 0.05f);
            expectWithinAbsoluteError (second.peakDb, first.peakDb, 1.0e-6f,
                                       "a second reader saw a different level");
        }

        beginTest ("the held peak decays as blocks arrive, not as it is read");
        {
            Meter meter;
            feedSingleSpike (meter, 1.0f);

            const auto atPeak = meter.read().peakDb;
            expectWithinAbsoluteError (atPeak, 0.0f, 0.05f);

            // Twenty quiet blocks is about 9 dB of decay at 0.45 dB per block.
            for (int i = 0; i < 20; ++i)
                feedConstant (meter, 0.0f);

            const auto later = meter.read().peakDb;
            expect (later < atPeak - 5.0f, "the peak did not fall");
            expect (later > atPeak - 20.0f, "the peak fell far too fast to read");
        }

        beginTest ("the per-block factor still means kPeakFallDbPerSecond");
        {
            // One rate, written in two units. kPeakFallDbPerSecond owns it;
            // kPeakDecayPerBlock is the linear factor the audio thread can
            // apply without a log, and it cannot be derived from the owner
            // because a per-block factor needs an assumed block size and sample
            // rate. So this is the only thing stopping the pair drifting.
            // meterscale::fallDbPerTick, the third expression of the same rate,
            // IS derived and cannot drift.
            //
            // Drift is not a crash: the bar the UI draws simply falls at a
            // different rate from the held peak it is drawing, so bars sag
            // below the number beside them or stick above a level that has
            // already gone.
            constexpr double blocksPerSecond =
                lighthost::metering::kReferenceSampleRate
                    / lighthost::metering::kReferenceBlockSize;

            expectWithinAbsoluteError (blocksPerSecond, 100.0, 1.0e-9,
                                       "the reference pair no longer gives a round 100 "
                                       "blocks a second, so the arithmetic below is not "
                                       "the arithmetic the comments describe");

            const double dbPerBlock = 20.0 * std::log10 (
                static_cast<double> (lighthost::metering::kPeakDecayPerBlock));

            // 0.15 dB per second. A four-decimal linear factor can only be
            // steered in steps of about 0.09 dB per second, so the tolerance
            // has to clear one of those grains -- and it deliberately clears
            // barely more than one, which is what makes a wrong digit in the
            // last place fail: 0.9497 lands 0.17 out.
            expectWithinAbsoluteError (dbPerBlock * blocksPerSecond,
                                       -static_cast<double> (lighthost::metering::kPeakFallDbPerSecond),
                                       0.15,
                                       "the block factor and the per-second rate have drifted, "
                                       "so the UI and the meter no longer fall together");
        }

        beginTest ("a held peak really does fall at that rate");
        {
            // The arithmetic above says the two constants agree. This says the
            // factor is applied once per block, which is the other half of the
            // claim and the half arithmetic cannot check.
            Meter meter;
            feedSingleSpike (meter, 1.0f);
            expectWithinAbsoluteError (meter.read().peakDb, 0.0f, 0.05f);

            constexpr int blocksInOneSecond =
                static_cast<int> (lighthost::metering::kReferenceSampleRate)
                    / lighthost::metering::kReferenceBlockSize;

            for (int i = 0; i < blocksInOneSecond; ++i)
                feedConstant (meter, 0.0f);

            expectWithinAbsoluteError (meter.read().peakDb,
                                       -lighthost::metering::kPeakFallDbPerSecond,
                                       0.15f,
                                       "one reference second of silence did not fall by the "
                                       "rate the UI draws it falling at");
        }

        beginTest ("RMS stops being reported once nothing is watching");
        {
            // It used to latch, so a reader arriving later was shown a figure
            // frozen from the last time something watched.
            Meter meter;

            {
                const Meter::Watch watch (&meter);

                for (int i = 0; i < 40; ++i)
                    feedConstant (meter, 0.5f);

                expect (meter.read().rmsValid);
            }

            feedConstant (meter, 0.5f);
            expect (! meter.read().rmsValid,
                    "RMS was still reported with no watcher");
        }

        beginTest ("clipping latches, and survives reads until it is cleared");
        {
            Meter meter;

            feedSingleSpike (meter, 1.0f);
            expect (meter.read().clipped);

            // The latch is the feature: the clip happened, and it stays said
            // until someone acknowledges it.
            expect (meter.read().clipped, "the latch should outlive the peak that set it");
            expect (meter.read().clipped);

            meter.clearClip();
            expect (! meter.read().clipped);
        }

        beginTest ("a level just under full scale does not read as clipped");
        {
            Meter meter;
            feedConstant (meter, 0.99f);

            const auto r = meter.read();
            expect (! r.clipped, "0.99 is -0.09 dBFS, which is hot but not clipped");
        }

        beginTest ("reset forgets the latch and the level");
        {
            Meter meter;
            const Meter::Watch watch (&meter);

            for (int i = 0; i < 10; ++i)
                feedConstant (meter, 1.0f);

            expect (meter.read().clipped);

            meter.reset();

            const auto r = meter.read();
            expect (! r.clipped);
            expect (! r.rmsValid);
            expectWithinAbsoluteError (r.peakDb, lighthost::metering::kFloorDb, 0.01f);
        }

        beginTest ("measuring nothing is harmless");
        {
            // The device callback can hand over zero channels or zero samples
            // while a device is changing, and an input-only device gives a null
            // output pointer array.
            Meter meter;

            meter.measure (nullptr, 2, kBlockSize);
            meter.measure (nullptr, 0, 0);

            juce::AudioBuffer<float> empty (0, 0);
            meter.measure (empty);

            const auto r = meter.read();
            expect (! r.clipped);
        }

        //======================================================================
        beginTest ("the probe passes audio through untouched");
        {
            // It sits in the live path whenever the signal view is open, so one
            // altered sample would mean opening a window changed the sound.
            Meter meter;
            Probe probe (meter);
            probe.prepareToPlay (kSampleRate, kBlockSize);

            juce::AudioBuffer<float> audio (2, kBlockSize);
            juce::MidiBuffer midi;

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < kBlockSize; ++i)
                    audio.setSample (ch, i, std::sin (0.05f * static_cast<float> (i))
                                                * (ch == 0 ? 0.5f : -0.25f));

            const juce::AudioBuffer<float> before (audio);

            probe.processBlock (audio, midi);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < kBlockSize; ++i)
                    expectWithinAbsoluteError (audio.getSample (ch, i),
                                               before.getSample (ch, i), 1.0e-6f);
        }

        beginTest ("the probe reports no latency, so it cannot disturb compensation");
        {
            Meter meter;
            Probe probe (meter);
            probe.prepareToPlay (kSampleRate, kBlockSize);

            expectEquals (probe.getLatencySamples(), 0,
                          "a probe that declared latency would shift the lane it sits in");
            expectWithinAbsoluteError (probe.getTailLengthSeconds(), 0.0, 1.0e-9);
        }

        beginTest ("the probe's meter sees what passed through it");
        {
            Meter meter;
            Probe probe (meter);
            probe.prepareToPlay (kSampleRate, kBlockSize);

            juce::AudioBuffer<float> audio (2, kBlockSize);
            juce::MidiBuffer midi;

            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (audio.getWritePointer (ch), 0.25f, kBlockSize);

            probe.processBlock (audio, midi);

            const auto r = meter.read();
            expectWithinAbsoluteError (r.peakDb, -12.04f, 0.05f);
        }

        beginTest ("preparing a probe clears a stale reading");
        {
            Meter meter;
            Probe probe (meter);
            juce::AudioBuffer<float> audio (2, kBlockSize);
            juce::MidiBuffer midi;

            for (int ch = 0; ch < 2; ++ch)
                juce::FloatVectorOperations::fill (audio.getWritePointer (ch), 1.0f, kBlockSize);

            probe.processBlock (audio, midi);

            // A device change re-prepares the graph. A clip latched against the
            // old device should not be reported against the new one.
            probe.prepareToPlay (kSampleRate, kBlockSize);

            expect (! meter.read().clipped);
        }
    }
};

static SignalMeteringTests signalMeteringTests;
