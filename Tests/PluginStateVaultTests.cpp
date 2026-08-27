#include "../Source/PluginStateVault.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// Tests for the per-plugin state files.
//
// This is where a user's presets live, so the cases that matter are the ones
// where something goes wrong: an identity that was never written, a file that
// has been truncated, a file written by a future version. All three must read as
// "nothing stored" rather than as garbage handed to a plugin, and none of them
// may be mistaken for success by a caller deciding whether to drop its only
// other copy.
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

        beginTest ("writing an empty state erases rather than storing nothing");
        {
            expect (vault.write ("to-be-emptied", blockOf ("x", 64)));
            expect (vault.has ("to-be-emptied"));

            expect (vault.write ("to-be-emptied", {}), "an empty write should succeed");
            expect (! vault.has ("to-be-emptied"), "the file should be gone, not blank");
            expectEquals ((int) vault.read ("to-be-emptied").getSize(), 0);
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

        beginTest ("a truncated file reads as nothing stored, not as garbage");
        {
            expect (vault.write ("truncated", blockOf ("z", 8192)));

            const auto file = vault.fileFor ("truncated");

            // Read as bytes, not as a String: compressed data contains nulls, and
            // a String would stop at the first one.
            juce::MemoryBlock raw;
            expect (file.loadFileAsData (raw), "could not read back the file just written");
            expect (raw.getSize() > 16, "expected a compressed stream longer than the magic");

            // Keep the magic, lose most of the compressed stream.
            expect (file.replaceWithData (raw.getData(), 12));

            expectEquals ((int) vault.read ("truncated").getSize(), 0,
                          "a truncated state must not be handed to a plugin");
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
