#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

//==============================================================================
// Restoring a plugin's saved state, with the failure handling factored out so it
// can be tested against a stub processor that throws.
//
// The bug this guards against: if setStateInformation throws (third-party plugin
// code, entirely outside our control), the node is left holding factory
// defaults. If the caller then saves that node's state back to disk, the user's
// saved preset is overwritten with defaults — a transient load failure turned
// into permanent, silent data loss. So the caller must be told whether the
// restore actually happened, and must NOT save a node whose restore failed.
//==============================================================================
namespace lighthost::state
{
    enum class RestoreResult
    {
        restored,      // state applied successfully
        nothingSaved,  // no saved state to apply; factory defaults are correct
        failed         // decode produced nothing, or setStateInformation threw
    };

    /** Outcome of recovering bytes from a pre-5.0.0 base64 blob.

        Separate from RestoreResult on purpose. Decoding is not restoring, and
        folding a fourth value into RestoreResult left the switch in
        IconMenu::restorePluginState quietly non-exhaustive -- it has no default,
        so a new value there is a case that compiles and does nothing.
    */
    enum class DecodeResult
    {
        decoded,       // destination holds the state
        nothingSaved,  // the string was empty; nothing was ever stored
        failed         // something was stored and will not decode
    };

    /** Applies raw state bytes to a processor. Never throws: a throwing plugin
        yields RestoreResult::failed, which the caller must treat as "do not
        overwrite the saved blob".

        This is the form state now arrives in. Since 5.0.0 it is read from a file
        per plugin rather than base64 out of the settings document, so decoding
        is no longer part of restoring.
    */
    [[nodiscard]] inline RestoreResult restoreInto (juce::AudioProcessor& processor,
                                                    const juce::MemoryBlock& state)
    {
        if (state.getSize() == 0)
            return RestoreResult::nothingSaved;

        try
        {
            processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
            return RestoreResult::restored;
        }
        catch (...)
        {
            return RestoreResult::failed;
        }
    }

    //==========================================================================
    /** Decodes a pre-5.0.0 base64 state blob into raw bytes.

        Three outcomes, and the caller has to tell them apart:

          nothingSaved  the string was empty. No preset was ever stored, so
                        factory defaults are the right answer and saving over
                        it later is harmless.

          failed        something WAS stored and will not decode. The bytes are
                        unusable but they are also the only copy there is, so
                        the caller must not overwrite them.

          decoded       `destination` holds the state.

        Sharing this mattered more than it looks. Both callers used to decode
        inline, and they disagreed: the migration checked the result and left an
        undecodable blob alone, while the load path discarded the return value of
        fromBase64Encoding entirely. That second one lost data. An undecodable
        legacy blob produced an empty MemoryBlock, restoreInto reported
        `nothingSaved` rather than `failed`, the node never landed in
        statesNotRestored, and the next savePluginStates wrote the plugin's
        factory defaults over the user's only copy of the preset -- which is the
        exact failure mode the note at the top of this file exists to prevent,
        reached through the one door that was not guarded.
    */
    [[nodiscard]] inline DecodeResult decodeLegacyState (const juce::String& base64State,
                                                         juce::MemoryBlock& destination)
    {
        destination.reset();

        if (base64State.isEmpty())
            return DecodeResult::nothingSaved;

        // Both halves matter: fromBase64Encoding returns false on a malformed
        // string, and a string of only invalid characters decodes to zero bytes
        // while still returning true.
        if (! destination.fromBase64Encoding (base64State) || destination.getSize() == 0)
        {
            destination.reset();
            return DecodeResult::failed;
        }

        return DecodeResult::decoded;
    }

    /** Applies base64-encoded state, as stored before 5.0.0.

        A thin façade over decodeLegacyState plus the MemoryBlock overload. It
        exists because the tests drive the decode rules through it, which means
        those tests now cover the same decoder the two production call sites use
        rather than a parallel copy of it.
    */
    [[nodiscard]] inline RestoreResult restoreInto (juce::AudioProcessor& processor,
                                                    const juce::String& base64State)
    {
        juce::MemoryBlock block;

        switch (decodeLegacyState (base64State, block))
        {
            case DecodeResult::decoded:      break;
            case DecodeResult::nothingSaved: return RestoreResult::nothingSaved;
            case DecodeResult::failed:       return RestoreResult::failed;
        }

        return restoreInto (processor, block);
    }
}
