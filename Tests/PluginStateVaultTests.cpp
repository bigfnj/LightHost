#include "../Source/PluginStateVault.hpp"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// Tests for the per-plugin state files.
//
// This is where a user's presets live, so the cases that matter are the ones
// where something goes wrong: an identity that was never written, a file
// written by a future version, a file that has been truncated. None of them may
// be mistaken for success by a caller deciding whether to drop its only other
// copy.
//
// All of them now read as unusable, and a truncated file is reported as
// `corrupt` rather than `absent` so the caller preserves it. Until 5.4.0 the
// mid-stream case returned a partial blob.
//
// The first two read as "nothing stored". Truncation used to read that way only when
// the cut lands before deflate emitted anything; a cut mid-stream still yields a
// partial blob. That is now detected: the payload is a zlib stream, RFC 1950
// ends one with a big-endian Adler-32 of the uncompressed data, and comparing
// it against what actually decompressed catches truncation without the format
// change BACKLOG.md predicted would be needed.
//==============================================================================
namespace
{

using Vault = lighthost::state::Vault;

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

        beginTest ("a file truncated mid-stream is refused, not handed over partial");
        {
            // This test previously pinned the opposite, deliberately: a file cut
            // after deflate had emitted output decompressed to whatever it had
            // emitted, and restorePluginState handed that half-preset to
            // setStateInformation. Its own comment said "when that lands, this
            // test SHOULD fail; change it to expect 0 then." It has landed.
            //
            // It did not need the format change the comment predicted. The
            // payload is a zlib stream and RFC 1950 ends one with a big-endian
            // Adler-32 of the uncompressed data, so the file already carries a
            // checksum of its own contents. Truncation destroys the real
            // trailer, so the last four bytes are deflate data, and they
            // disagree with the checksum of what decompressed.
            //
            // Incompressible bytes, not blockOf's repeated pattern: a pattern
            // deflates to almost nothing, leaving no stream to truncate inside.
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

            auto outcome = Vault::ReadResult::ok;
            const auto readBack = vault.read ("truncated-mid", &outcome);

            expectEquals ((int) readBack.getSize(), 0,
                          "a truncated file must yield nothing, not half a preset");
            expect (outcome == Vault::ReadResult::corrupt,
                    "truncation must report corrupt, not absent -- absent would "
                    "license the caller to overwrite the only copy");
        }

        beginTest ("corrupt is distinguishable from absent, which is the whole point");
        {
            // The caller keys on this. `absent` means nothing was ever stored,
            // so saving over it is harmless; `corrupt` means the only copy of a
            // preset is unusable and must be left alone. Collapsing them is how
            // 5.2.0 lost a preset through the legacy base64 path.
            auto outcome = Vault::ReadResult::ok;

            (void) vault.read ("never-written-at-all", &outcome);
            expect (outcome == Vault::ReadResult::absent);

            expect (vault.write ("intact", blockOf ("q", 512)));
            (void) vault.read ("intact", &outcome);
            expect (outcome == Vault::ReadResult::ok);
        }

        beginTest ("a single flipped byte in the payload is caught");
        {
            // Not just truncation. A bit rot or a partial overwrite in the
            // middle of the stream changes what decompresses, so the stored
            // Adler-32 no longer matches it.
            const auto original = randomBlock (32 * 1024, 20260918);

            expect (vault.write ("bit-rot", original));

            const auto file = vault.fileFor ("bit-rot");

            juce::MemoryBlock raw;
            expect (file.loadFileAsData (raw));

            auto* bytes = static_cast<juce::uint8*> (raw.getData());
            const auto middle = raw.getSize() / 2;
            bytes[middle] = static_cast<juce::uint8> (bytes[middle] ^ 0xff);

            expect (file.replaceWithData (raw.getData(), raw.getSize()));

            auto outcome = Vault::ReadResult::ok;
            const auto readBack = vault.read ("bit-rot", &outcome);

            // Either the deflate stream rejects the altered byte outright or the
            // checksum does. Both are correct; what matters is that no altered
            // preset reaches a plugin.
            expect (readBack != original, "a corrupted payload was returned as valid");

            if (readBack.getSize() != 0)
                expect (false, "a corrupted payload returned bytes at all");
            else
                expect (outcome == Vault::ReadResult::corrupt);
        }

        beginTest ("the Adler-32 helper agrees with the values RFC 1950 specifies");
        {
            // Tested directly because the truncation check rests on it, and a
            // checksum that is subtly wrong would pass every test above by
            // disagreeing with the file consistently.
            //
            // Known values: the empty string is 1, and "Wikipedia" is 0x11E60398.
            expectEquals ((int) (juce::int64) Vault::adler32 ("", 0), 1);

            const char* wiki = "Wikipedia";
            expectEquals ((juce::int64) Vault::adler32 (wiki, 9), (juce::int64) 0x11E60398);
        }

        beginTest ("the Adler-32 helper blocks correctly past NMAX");
        {
            // The modulo runs once per 5552 bytes rather than per byte, so the
            // blocking arithmetic only shows up on inputs longer than that. A
            // real plugin state measured four megabytes.
            const auto big = blockOf ("abcdefgh", 64 * 1024);

            // Recomputed the slow, obvious way.
            constexpr juce::uint32 base = 65521;
            juce::uint32 a = 1, b = 0;
            const auto* p = static_cast<const juce::uint8*> (big.getData());

            for (size_t i = 0; i < big.getSize(); ++i)
            {
                a = (a + p[i]) % base;
                b = (b + a) % base;
            }

            expectEquals ((juce::int64) Vault::adler32 (big.getData(), big.getSize()),
                          (juce::int64) ((b << 16) | a),
                          "the blocked implementation disagrees with the direct one");
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

//==============================================================================
// The write failures a real disk produces and a test never could.
//
// Every failure case above corrupts a file that has already been written,
// because that
// is all a test could reach: write() built its own FileOutputStream, so the
// durability check at the end of it -- the one guarding against a truncated
// temporary being renamed over the user's only good preset -- could only fire
// on a full disk or a device pulled mid-write.
//
// Vault::withSink supplies the stream instead, and nothing else about the
// write changes: the directory is really created, the bytes really go to a
// juce::TemporaryFile, and the rename is really
// overwriteTargetFileWithTemporary. So what these cases assert is not "the
// sink was consulted" but the thing the user cares about -- that a write which
// reported failure left the previous preset exactly where it was.
//==============================================================================

/** Repetitive bytes standing in for the float data real plugin state is mostly
    made of. Kept separate from the class above's blockOf: these cases are
    about whether bytes survive a failed write, not about how well they
    compress.
*/
juce::MemoryBlock presetBytes (const char* seed, size_t count)
{
    const juce::String pattern (seed);
    juce::MemoryBlock block;

    while (block.getSize() < count)
        block.append (pattern.toRawUTF8(), (size_t) pattern.getNumBytesAsUTF8());

    block.setSize (count, false);
    return block;
}

enum class SinkMode
{
    failToOpen,     // the factory hands back nothing: a path that will not open
    failOnFlush,    // every byte accepted, then the flush reports the fault
    succeed         // the positive control: a real file, really written
};

/** A sink that writes for real and then lies about the flush on request.

    Writing for real is the part that makes the failure cases able to fail.
    A sink that swallowed the bytes would leave an empty temporary, and the
    "the stored preset is untouched" assertions would pass even with the
    durability check deleted -- there would be nothing worth renaming. Because
    the temporary is complete, deleting that check renames it over the stored
    preset, which is exactly the full-disk accident, and the assertion goes
    red.
*/
class FailingSink final : public lighthost::state::SinkStream
{
public:
    FailingSink (const juce::File& target, bool flushShouldLand)
        : out (target), flushLands (flushShouldLand) {}

    [[nodiscard]] bool opened() const { return ! out.failedToOpen(); }

    [[nodiscard]] juce::OutputStream& stream() override { return out; }

    [[nodiscard]] bool flushAndCheck() override
    {
        out.flush();

        return flushLands && ! out.getStatus().failed();
    }

private:
    juce::FileOutputStream out;
    bool flushLands;
};

[[nodiscard]] lighthost::state::SinkFactory sinkThat (SinkMode mode)
{
    return [mode] (const juce::File& file) -> std::unique_ptr<lighthost::state::SinkStream>
    {
        if (mode == SinkMode::failToOpen)
            return nullptr;

        auto sink = std::make_unique<FailingSink> (file, mode == SinkMode::succeed);

        if (! sink->opened())
            return nullptr;

        return sink;
    };
}

class VaultWriteFailureTests final : public juce::UnitTest
{
public:
    VaultWriteFailureTests()
        : juce::UnitTest ("Plugin state vault write failures", "PluginStateVault") {}

    void runTest() override
    {
        auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("LightHostVaultSinkTests-"
                                       + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

        const auto stored      = presetBytes ("the preset the user actually saved", 4096);
        const auto replacement = presetBytes ("the write that is about to fail", 8192);

        beginTest ("a stream that will not open is a failed write, and the stored preset survives it");
        {
            const auto directory = root.getChildFile ("cannot-open");
            const lighthost::state::Vault real (directory);

            expect (real.write ("preset", stored), "the fixture write should have landed");

            const auto broken = lighthost::state::Vault::withSink (directory,
                                                                   sinkThat (SinkMode::failToOpen));

            // FAILS IF: write() stops checking the sink for nullptr, or starts
            // reporting a stream it never opened as a saved state. The caller
            // migrating out of the legacy format drops its own copy on a true
            // return, so this is the difference between a failed save and a
            // lost preset.
            expect (! broken.write ("preset", replacement),
                    "a write that never opened a stream reported success");

            // FAILS IF: the no-stream path ever reaches
            // overwriteTargetFileWithTemporary.
            expect (real.read ("preset") == stored,
                    "a preset was replaced by a write that never opened a stream");
        }

        beginTest ("a write whose flush fails does not replace the stored preset");
        {
            const auto directory = root.getChildFile ("flush-fails");
            const lighthost::state::Vault real (directory);

            expect (real.write ("preset", stored), "the fixture write should have landed");

            const auto failing = lighthost::state::Vault::withSink (directory,
                                                                    sinkThat (SinkMode::failOnFlush));

            // FAILS IF: write() stops asking flushAndCheck, or ignores what it
            // says. This is the branch a full disk takes: the deflate tail is
            // written from a destructor with every return value discarded, so
            // the flush is the only place the truncation is still visible.
            expect (! failing.write ("preset", replacement),
                    "a write whose flush failed reported success");

            expect (real.read ("preset") == stored,
                    "a preset was replaced by a write that reported failure -- a full "
                    "disk would now have destroyed the only copy");
        }

        beginTest ("a sink that accepts everything lands the write");
        {
            // The positive control. Without it the two cases above prove only
            // that an injected sink makes writes fail, which they would also
            // prove if withSink were broken outright.
            const auto directory = root.getChildFile ("accepts");
            const auto working = lighthost::state::Vault::withSink (directory,
                                                                    sinkThat (SinkMode::succeed));

            expect (working.write ("preset", stored),
                    "a sink that accepted every byte was still reported as a failed write");

            // FAILS IF: the injected sink is consulted but its bytes go
            // nowhere -- read() is the production path and knows nothing about
            // sinks.
            const lighthost::state::Vault real (directory);
            expect (real.read ("preset") == stored,
                    "the bytes an accepting sink took never reached the file");
        }

        beginTest ("the ordinary constructor still writes a real readable file");
        {
            // FAILS IF: the default factory stops being fileSink. Every case
            // above supplies its own sink, so all of them would stay green with
            // production pointed at a test double; this is the only line that
            // notices.
            const lighthost::state::Vault production (root.getChildFile ("default-sink"));

            expect (production.write ("preset", stored)
                        && production.fileFor ("preset").getSize() > 0
                        && production.read ("preset") == stored,
                    "a default-constructed vault did not write a real readable file");
        }

        root.deleteRecursively();
    }
};

VaultWriteFailureTests vaultWriteFailureTests;

//==============================================================================
// Clearing every saved state, and the sentence shown afterwards.
//
// Both halves used to live inside IconMenu, reachable only by clicking Delete
// in a native message box -- so the one thing worth checking, that the count
// in the message is the number that actually failed, was checkable only by
// arranging for a plugin's state file to be locked by hand.
//
// The count is not cosmetic. "Deleted saved plugin states" was once printed
// unconditionally, including on a run where every single delete failed, and a
// user who reads that has no reason to look at their presets again.
//
// The ordering assertion below is the load-bearing one. A loop that returns on
// the first failure reports the same `failed` count as a loop that finishes,
// so only the record of what was attempted can tell them apart -- and the
// difference between them is every preset after the one that would not go.
//==============================================================================
class PluginStateDeletionTests final : public juce::UnitTest
{
public:
    PluginStateDeletionTests()
        : juce::UnitTest ("Plugin state deletion accounting", "PluginStateVault") {}

    void runTest() override
    {
        using lighthost::state::deletionReport;
        using lighthost::state::eraseEach;

        beginTest ("every identity is attempted even after one fails");
        {
            const std::vector<juce::String> five { "one", "two", "three", "four", "five" };
            juce::StringArray attempts;

            const auto outcome = eraseEach (five,
                                            [&attempts] (const juce::String& identity)
                                            {
                                                attempts.add (identity);
                                                return identity != "two";
                                            });

            // FAILS IF: eraseEach returns early on a failure. The counts alone
            // cannot catch that -- stopping at "two" reports failed == 1, and
            // so does finishing -- so the recorded order is the evidence, and
            // what it stands for is the three presets after it that would have
            // been left on disk.
            expectEquals (attempts.joinIntoString (","),
                          juce::String ("one,two,three,four,five"),
                          "an identity after the failing one was never attempted: "
                              + attempts.joinIntoString (","));

            expectEquals (outcome.attempted, 5, "the attempted count disagrees with the calls made");
            expectEquals (outcome.failed, 1);
            expectEquals (outcome.failedIdentities.joinIntoString (","), juce::String ("two"),
                          "the wrong identity was recorded as the one that would not go");
        }

        beginTest ("a run where every delete fails reports every one");
        {
            const std::vector<juce::String> three { "a", "b", "c" };

            const auto outcome = eraseEach (three, [] (const juce::String&) { return false; });

            // FAILS IF: the count saturates, or records only the first
            // failure. A user told one plugin would not clear, when none of
            // them did, goes looking in the wrong place.
            expectEquals (outcome.attempted, 3);
            expectEquals (outcome.failed, 3, "a total failure was under-reported");
            expectEquals (outcome.failedIdentities.size(), 3);
        }

        beginTest ("an empty list reports nothing attempted");
        {
            int calls = 0;

            const auto outcome = eraseEach ({}, [&calls] (const juce::String&)
                                                {
                                                    ++calls;
                                                    return true;
                                                });

            // FAILS IF: a chain with no plugins somehow erases something, or
            // reports a failure and tells the user their presets are stuck.
            expectEquals (calls, 0, "an empty chain still tried to erase something");
            expectEquals (outcome.attempted, 0);
            expectEquals (outcome.failed, 0);
        }

        beginTest ("the count reaches the message");
        {
            const std::vector<juce::String> three { "kept", "locked", "also-locked" };

            const auto outcome = eraseEach (three,
                                            [] (const juce::String& identity)
                                            {
                                                return ! identity.contains ("locked");
                                            });

            const auto message = deletionReport (outcome.failed);

            // FAILS IF: the count stops reaching the sentence -- a hard-coded
            // number, or a report built from `attempted` rather than `failed`.
            expect (message.contains ("2"),
                    "the message does not name how many failed: " + message);
            expect (message.contains ("plugins'"),
                    "two failures were described in the singular: " + message);

            // FAILS IF: the wording drifts. It moved out of IconMenu to become
            // testable, not to be rewritten; changing what the user reads is a
            // separate decision from making it checkable.
            expectEquals (message,
                          juce::String ("Could not delete 2 plugins' saved states. "
                                        "Something else is holding the files open."));
            expectEquals (deletionReport (1),
                          juce::String ("Could not delete 1 plugin's saved state. "
                                        "Something else is holding the files open."));
        }

        beginTest ("a clean run does not claim a failure");
        {
            const std::vector<juce::String> three { "a", "b", "c" };

            const auto outcome = eraseEach (three, [] (const juce::String&) { return true; });

            expectEquals (outcome.failed, 0);

            // FAILS IF: the report ever describes a successful run as a
            // failure. The scare is the whole cost here -- there is nothing
            // for the user to do about a deletion that worked.
            expectEquals (deletionReport (outcome.failed),
                          juce::String ("Deleted saved plugin states"),
                          "a run in which everything cleared reported trouble");

            // A negative is unreachable, and clamping it is the better of the
            // two wrong answers: "Could not delete -1 plugins' saved states"
            // invents a data loss out of an arithmetic slip.
            expectEquals (deletionReport (-1), juce::String ("Deleted saved plugin states"));
        }
    }
};

PluginStateDeletionTests pluginStateDeletionTests;

} // namespace
