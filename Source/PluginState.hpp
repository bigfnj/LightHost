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

    /** Applies base64-encoded state, as stored before 5.0.0.

        Used only by the tests. The claim this comment used to make -- that the
        migration reads that format through here, and that un-migrated state is
        still loaded through here -- was not true of either caller: both
        IconMenu::migrateStateToVault and IconMenu::loadActivePlugins decode the
        base64 themselves and then call the MemoryBlock overload above.

        Kept rather than deleted because it is the only place the decode-failure
        rule is stated once and tested, and because those two call sites should
        eventually come through here instead of repeating the decode. Recorded in
        BACKLOG.md so that is a decision rather than a leftover.

        Note the asymmetry with the overload above: state that fails to decode is
        `failed`, not `nothingSaved`. Something was stored and could not be used,
        and the caller must not overwrite it.
    */
    [[nodiscard]] inline RestoreResult restoreInto (juce::AudioProcessor& processor,
                                                    const juce::String& base64State)
    {
        if (base64State.isEmpty())
            return RestoreResult::nothingSaved;

        juce::MemoryBlock block;
        block.fromBase64Encoding (base64State);

        if (block.getSize() == 0)
            return RestoreResult::failed;

        return restoreInto (processor, block);
    }
}
