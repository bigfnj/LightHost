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
            const auto result = restoreInto (reopened, savedBlob);
            const bool restoreOk = result == RestoreResult::restored;

            expect (! restoreOk, "precondition: the restore should have failed");

            // The guard: only a successful restore may be saved back.
            // The caller's rule, asserted directly rather than modelled: a
            // restore that did not succeed must be reported as `failed`, because
            // that is the value IconMenu::savePluginStates keys on to skip the
            // node and leave the stored blob alone. Reporting `nothingSaved`
            // here would let the save overwrite a real preset with defaults.
            //
            // This used to build blobOnDisk behind `if (restoreOk)`, which the
            // expect above had already established was false -- so the final
            // comparison was savedBlob against savedBlob and could not fail.
            expect (result == RestoreResult::failed,
                    "a refused restore must report failed, not nothingSaved, or "
                    "savePluginStates will overwrite the stored preset");

            juce::String blobOnDisk = savedBlob;
            if (restoreOk)
                blobOnDisk = base64Of (reopened);   // would clobber with defaults

            expectEquals (blobOnDisk, savedBlob, "the user's saved state was overwritten");
        }

        //======================================================================
        // decodeLegacyState, directly.
        //
        // It had no test of its own, and it is the whole of the 5.2.0 preset-loss
        // fix: the load path used to discard the result of fromBase64Encoding,
        // which turned an undecodable blob into an empty MemoryBlock. That was
        // then reported as "nothing was saved" rather than "this failed", so the
        // node never reached statesNotRestored and the next save wrote factory
        // defaults over the user's only copy.
        //
        // The three outcomes cannot be distinguished through the RestoreResult
        // façade, because it collapses nothingSaved and an empty block onto the
        // same value. So they are asserted here.
        beginTest ("an empty string means nothing was ever stored");
        {
            juce::MemoryBlock block;

            expect (decodeLegacyState (juce::String(), block) == DecodeResult::nothingSaved);
            expectEquals ((int) block.getSize(), 0);

            // The distinction that matters: nothing was stored, so saving over it
            // later is harmless. Compare with the failed case below.
        }

        beginTest ("state that will not decode is failed, not nothingSaved");
        {
            // A string of only invalid characters. This is the case that cost a
            // preset: it decodes to zero bytes, and calling that "nothing saved"
            // licenses the caller to overwrite it.
            juce::MemoryBlock block;

            expect (decodeLegacyState ("!!!!", block) == DecodeResult::failed,
                    "an undecodable blob reported as nothingSaved is how a real "
                    "preset gets replaced with factory defaults");
            expectEquals ((int) block.getSize(), 0,
                          "a failed decode must not leave partial bytes behind");
        }

        beginTest ("valid base64 round-trips to the original bytes");
        {
            juce::MemoryBlock original;
            original.append ("some plugin state", 17);

            juce::MemoryBlock decoded;
            expect (decodeLegacyState (original.toBase64Encoding(), decoded)
                        == DecodeResult::decoded);

            expectEquals ((int) decoded.getSize(), (int) original.getSize());
            expect (decoded == original);
        }

        beginTest ("the destination is cleared before every attempt");
        {
            // The caller reuses one MemoryBlock across chain entries, so a
            // failed decode must not leave the previous plugin's state in it --
            // which would restore one plugin's preset into another.
            juce::MemoryBlock block;
            block.append ("previous plugin state", 21);

            expect (decodeLegacyState (juce::String(), block) == DecodeResult::nothingSaved);
            expectEquals ((int) block.getSize(), 0, "stale bytes survived a decode");

            block.append ("previous plugin state", 21);
            expect (decodeLegacyState ("!!!!", block) == DecodeResult::failed);
            expectEquals ((int) block.getSize(), 0, "stale bytes survived a failed decode");
        }
    }
};

static PluginStateRestoreTests pluginStateRestoreTests;
