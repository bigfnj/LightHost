#include "../Source/PluginState.hpp"
#include "StubProcessors.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

//==============================================================================
// Regression tests for the preset-destroying-state-restore bug.
//
// Before this fix, a plugin whose setStateInformation threw was left with
// factory defaults, and the next save wrote those defaults over the user's good
// saved blob. The fix reports whether the restore succeeded so the caller can
// refuse to save a node that failed to restore. These tests pin that contract.
//==============================================================================
namespace
{
    using namespace lighthost::state;

    /** Records the state it is given, and can be told to throw on restore, to
        model a real plugin misbehaving in third-party code.
    */
    class StateStub final : public juce::AudioProcessor
    {
    public:
        explicit StateStub (bool throwOnRestore = false)
            : AudioProcessor (BusesProperties()
                                  .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
                                  .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
              shouldThrow (throwOnRestore)
        {
        }

        const juce::String getName() const override        { return "StateStub"; }
        bool acceptsMidi() const override                  { return false; }
        bool producesMidi() const override                 { return false; }
        double getTailLengthSeconds() const override       { return 0.0; }
        int getNumPrograms() override                       { return 1; }
        int getCurrentProgram() override                    { return 0; }
        void setCurrentProgram (int) override               {}
        const juce::String getProgramName (int) override    { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        bool hasEditor() const override                     { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        void prepareToPlay (double, int) override            {}
        void releaseResources() override                     {}
        using juce::AudioProcessor::processBlock;
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

        void getStateInformation (juce::MemoryBlock& out) override
        {
            out.append (payload.toRawUTF8(), payload.getNumBytesAsUTF8());
        }

        void setStateInformation (const void* data, int size) override
        {
            if (shouldThrow)
                throw std::runtime_error ("plugin refused to restore");

            restored = juce::String::fromUTF8 (static_cast<const char*> (data), size);
        }

        juce::String payload  { "the-good-state" };
        juce::String restored;
        bool shouldThrow = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StateStub)
    };

    /** base64 of a processor's current getStateInformation output. */
    juce::String base64Of (juce::AudioProcessor& p)
    {
        juce::MemoryBlock block;
        p.getStateInformation (block);
        return block.toBase64Encoding();
    }
}

//==============================================================================
class PluginStateRestoreTests final : public juce::UnitTest
{
public:
    PluginStateRestoreTests()
        : juce::UnitTest ("Plugin state restore", "PluginState") {}

    void runTest() override
    {
        beginTest ("a good saved blob restores and reports success");
        {
            StateStub source;
            source.payload = "chain-of-plugins-preset";
            const auto blob = base64Of (source);

            StateStub target;
            const auto result = restoreInto (target, blob);

            expect (result == RestoreResult::restored);
            expectEquals (target.restored, juce::String ("chain-of-plugins-preset"));
        }

        beginTest ("an empty saved blob reports nothingSaved, not failure");
        {
            // A freshly added plugin has no saved state. That must not be treated
            // as a failure, or every fresh plugin would be flagged and never saved.
            StateStub target;
            expect (restoreInto (target, juce::String()) == RestoreResult::nothingSaved);
        }

        beginTest ("a throwing plugin reports failure and does not propagate");
        {
            // This is the bug. The saved blob is valid, but the plugin throws
            // while applying it. restoreInto must swallow the throw and report
            // failure, so the caller knows not to overwrite the good blob.
            StateStub source;
            const auto goodBlob = base64Of (source);

            StateStub thrower (/*throwOnRestore*/ true);
            RestoreResult result = RestoreResult::restored;

            // Must not escape.
            try
            {
                result = restoreInto (thrower, goodBlob);
            }
            catch (...)
            {
                expect (false, "restoreInto let the plugin's exception escape");
            }

            expect (result == RestoreResult::failed);
        }

        beginTest ("garbage that decodes to nothing reports failure");
        {
            StateStub target;
            // A base64 string of only invalid characters decodes to zero bytes.
            expect (restoreInto (target, juce::String ("!!!!")) == RestoreResult::failed);
        }

        beginTest ("the failure contract protects the round trip");
        {
            // End to end: a plugin's good state is saved, a later session fails
            // to restore it, and the rule 'do not save a node that failed to
            // restore' preserves the original blob. Modelled directly.
            StateStub original;
            original.payload = "years-of-tweaking";
            const auto savedBlob = base64Of (original);

            // New session: the plugin now throws on restore.
            StateStub reopened (/*throwOnRestore*/ true);
            const bool restoreOk = restoreInto (reopened, savedBlob) == RestoreResult::restored;

            expect (! restoreOk, "precondition: the restore should have failed");

            // The guard: only a successful restore may be saved back.
            juce::String blobOnDisk = savedBlob;
            if (restoreOk)
                blobOnDisk = base64Of (reopened);   // would clobber with defaults

            expectEquals (blobOnDisk, savedBlob, "the user's saved state was overwritten");
        }
    }
};

static PluginStateRestoreTests pluginStateRestoreTests;
