#include "../Source/PluginStateVault.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// Tests for the per-plugin state files.
//
// This is where a user's presets live, so the cases that matter are the ones
// where something goes wrong: an identity that was never written, a file
// written by a future version, a file that has been truncated. None of them may
// be mistaken for success by a caller deciding whether to drop its only other
// copy.
//
// The first two read as "nothing stored". Truncation reads that way only when
// the cut lands before deflate emitted anything; a cut mid-stream still yields a
// partial blob, which the two truncation cases below pin honestly rather than
// papering over. That gap is in BACKLOG.md and needs a format change to close.
//==============================================================================
namespace
{

class PluginStateVaultTests final : public juce::UnitTest
{
public:
    PluginStateVaultTests()
        : juce::UnitTest ("Plugin state vault", "PluginStateVault") {}

    void runTest() override
    {
        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("LightHostVaultTests-"
                                       + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

        const lighthost::state::Vault vault (root);

        beginTest ("state survives a round trip unchanged");
        {
            const auto original = blockOf ("a plugin preset, byte for byte", 4096);

            expect (vault.write ("aaaa1111", original), "the write should have landed");

            const auto restored = vault.read ("aaaa1111");

            expectEquals ((int) restored.getSize(), (int) original.getSize());
            expect (restored == original, "the bytes came back different");
        }

        beginTest ("an identity that was never written reads as nothing stored");
        {
            expectEquals ((int) vault.read ("never-written").getSize(), 0);
            expect (! vault.has ("never-written"));
        }

        beginTest ("compressible state is smaller on disk than in memory");
        {
            // Plugin state is mostly repetitive float data, which is the case
            // this storage format exists to exploit. A payload that does not
            // shrink would mean the compressor is not running at all.
            const auto original = blockOf ("0", 200000);

            expect (vault.write ("compressible", original));

            const auto onDisk = vault.fileFor ("compressible").getSize();

            expect (onDisk > 0, "nothing was written");
            expect (onDisk < (juce::int64) original.getSize() / 2,
                    "expected the stored file to be well under half the raw size, got "
                        + juce::String (onDisk) + " from "
                        + juce::String ((int) original.getSize()));

            expect (vault.read ("compressible") == original, "compression was not lossless");
        }

        beginTest ("writing an empty state is refused and leaves the stored state alone");
        {
            // This used to erase and return TRUE. savePluginStates forwards
            // whatever getStateInformation produced, and a plugin can hand back
            // nothing transiently -- a VST2 getChunk that failed, a plugin in a
            // bad state. The vault deleted the good preset, reported success, so
            // the caller updated lastWrittenState and cleared the pre-5.0.0
            // fallback too. Both copies gone, nothing logged, no message shown.
            const auto stored = blockOf ("x", 64);

            expect (vault.write ("to-be-emptied", stored));
            expect (vault.has ("to-be-emptied"));

            expect (! vault.write ("to-be-emptied", {}),
                    "an empty write must be refused, not reported as a successful save");
            expect (vault.has ("to-be-emptied"), "the preset was deleted by an empty write");
            expect (vault.read ("to-be-emptied") == stored,
                    "the stored bytes did not survive an empty write");

            // Clearing is still possible, through the verb that means it.
            expect (vault.erase ("to-be-emptied"));
            expect (! vault.has ("to-be-emptied"));
        }

        beginTest ("a second write replaces the first");
        {
            expect (vault.write ("replaced", blockOf ("first", 128)));
            const auto second = blockOf ("second, and longer than the first", 512);
            expect (vault.write ("replaced", second));

            expect (vault.read ("replaced") == second, "the older state was still there");
        }

        beginTest ("erasing state that is not there succeeds");
        {
            // The postcondition is "no state stored for this identity", and for an
            // identity that has none it already holds. Reporting failure would
            // make every caller special-case it.
            expect (vault.erase ("absent"));
        }

        beginTest ("erase removes the file");
        {
            expect (vault.write ("doomed", blockOf ("y", 256)));
            expect (vault.erase ("doomed"));
            expect (! vault.has ("doomed"));
        }

        beginTest ("a file truncated before any deflate output reads as nothing stored");
        {
            // The truncation axis has two points, and it used to have only this
            // one -- and this one is degenerate, which is why the case passed
            // for a reason unrelated to the handling it claimed to test.
            //
            // The degeneracy is in the FIXTURE, not the offset. blockOf repeats
            // a pattern, so deflate finds it entirely and emits nothing until
            // Z_FINISH; cut at twelve bytes there is no output to recover and
            // zero is the only possible answer. Swap in incompressible bytes and
            // the same twelve-byte cut returns a partial blob, because deflate
            // falls back to STORED blocks for data it cannot shrink and those
            // carry literal bytes almost immediately. Measured, not reasoned:
            // that swap is mutation M5 and it turns this expectEquals red.
            //
            // So this case pins one real behaviour -- compressible state cut
            // early reads as nothing -- and nothing broader. The case below is
            // the one that covers state the way a plugin actually stores it.
            expect (vault.write ("truncated-early", blockOf ("z", 8192)));

            const auto file = vault.fileFor ("truncated-early");

            // Read as bytes, not as a String: compressed data contains nulls, and
            // a String would stop at the first one.
            juce::MemoryBlock raw;
            expect (file.loadFileAsData (raw), "could not read back the file just written");
            expect (raw.getSize() > 16, "expected a compressed stream longer than the magic");

            expect (file.replaceWithData (raw.getData(), 12));

            expectEquals ((int) vault.read ("truncated-early").getSize(), 0);
        }

        beginTest ("a file truncated mid-stream yields a PARTIAL state (known gap)");
        {
            // Pinning a defect on purpose, so it stops being invisible. A file
            // cut after deflate has emitted output decompresses to whatever it
            // had emitted, readIntoMemoryBlock appends that, and
            // restorePluginState hands the partial blob to setStateInformation:
            // a plugin configured from half a preset rather than reset to
            // factory defaults. The doc on Vault::read asserted the opposite
            // until today.
            //
            // Closing it needs a length or checksum in the header, which is a
            // format change -- the gzip trailer cannot stand in, because
            // isExhausted() folds error, clean end and EOF into one bool and the
            // helper's `finished` flag is private. Filed in BACKLOG.md. When
            // that lands, this test SHOULD fail; change it to expect 0 then.
            //
            // Incompressible bytes, not blockOf's repeated pattern: a pattern
            // deflates to almost nothing, leaving no stream to truncate inside,
            // which is how the degenerate point above came to be the only one.
            const auto original = randomBlock (64 * 1024, 20260917);

            expect (vault.write ("truncated-mid", original));

            const auto file = vault.fileFor ("truncated-mid");

            juce::MemoryBlock raw;
            expect (file.loadFileAsData (raw), "could not read back the file just written");
            expect (raw.getSize() > 4096,
                    "expected an incompressible stream of real length, got "
                        + juce::String ((int) raw.getSize()));

            // Half the stream: well past the header, well short of the end.
            expect (file.replaceWithData (raw.getData(), raw.getSize() / 2));

            const auto partial = vault.read ("truncated-mid");

            expect (partial.getSize() > 0,
                    "expected the partial-read gap this test exists to pin; if this now "
                    "fails, truncation is detected and the test should expect 0");
            expect (partial.getSize() < original.getSize(),
                    "a truncated file returned at least as much as was written");
            expect (partial != original, "a truncated file returned the original bytes");
        }

        beginTest ("a file without the magic reads as nothing stored");
        {
            const auto file = vault.fileFor ("foreign");
            file.getParentDirectory().createDirectory();
            file.replaceWithText ("LHS9 written by some later version");

            expectEquals ((int) vault.read ("foreign").getSize(), 0);
        }

        beginTest ("an empty identity is refused rather than writing a stray file");
        {
            expect (! vault.write ("", blockOf ("q", 32)), "an empty identity should not write");
            expect (! vault.erase (""));
            expectEquals ((int) vault.read ("").getSize(), 0);
        }

        beginTest ("the fingerprint tracks content, not identity");
        {
            const auto a = blockOf ("same", 1024);
            const auto b = blockOf ("same", 1024);
            auto       c = blockOf ("same", 1024);
            c.copyFrom ("!", 0, 1);

            using lighthost::state::Vault;

            expect (Vault::fingerprint (a) == Vault::fingerprint (b),
                    "identical bytes should fingerprint the same");
            expect (Vault::fingerprint (a) != Vault::fingerprint (c),
                    "a one-byte difference should change the fingerprint");
            expect (Vault::fingerprint ({}) != Vault::fingerprint (a),
                    "an empty state should not collide with a populated one");
        }

        root.deleteRecursively();
    }

private:
    /** A block of `count` bytes built by repeating `seed`. Repetitive on purpose:
        it stands in for the float data real plugin state is mostly made of.
    */
    /** Bytes deflate cannot shrink, so the stored stream is long enough to cut
        inside. Seeded rather than system-random: a truncation test that picks a
        different stream length on every run is a test that fails on somebody
        else's machine and nowhere else.
    */
    static juce::MemoryBlock randomBlock (size_t count, juce::int64 seed)
    {
        juce::Random random (seed);
        juce::MemoryBlock block (count);
        auto* bytes = static_cast<juce::uint8*> (block.getData());

        for (size_t i = 0; i < count; ++i)
            bytes[i] = static_cast<juce::uint8> (random.nextInt (256));

        return block;
    }

    static juce::MemoryBlock blockOf (const char* seed, size_t count)
    {
        const juce::String pattern (seed);
        juce::MemoryBlock block;

        while (block.getSize() < count)
            block.append (pattern.toRawUTF8(), (size_t) pattern.getNumBytesAsUTF8());

        block.setSize (count, false);
        return block;
    }
};

PluginStateVaultTests pluginStateVaultTests;

} // namespace
