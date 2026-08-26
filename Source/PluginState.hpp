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

    /** Applies base64-encoded state to a processor. Never throws: a throwing
        plugin yields RestoreResult::failed, which the caller must treat as
        "do not overwrite the saved blob".
    */
    [[nodiscard]] inline RestoreResult restoreInto (juce::AudioProcessor& processor,
                                                    const juce::String& base64State)
    {
        if (base64State.isEmpty())
            return RestoreResult::nothingSaved;

        try
        {
            juce::MemoryBlock block;
            block.fromBase64Encoding (base64State);

            if (block.getSize() == 0)
                return RestoreResult::failed;

            processor.setStateInformation (block.getData(), static_cast<int> (block.getSize()));
            return RestoreResult::restored;
        }
        catch (...)
        {
            return RestoreResult::failed;
        }
    }
}
