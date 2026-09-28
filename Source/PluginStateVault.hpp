#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

//==============================================================================
// Where a plugin's saved state lives: one compressed file per slot.
//
// Up to 5.0.0 every plugin's state was base64-encoded into the single
// PropertiesFile that also holds the chain settings. Measured on a real chain
// (2026-08-27; five plugins, two of them sonible): the settings file was
// 7,489,165 bytes, of which 7,477,351 were state blobs. That is 99.84% of the
// document. juce::PropertiesFile rewrites the whole document on every save, so
// toggling one bypass wrote seven and a half megabytes on the message thread,
// and each of those writes was another window in which an interrupted shutdown
// could lose the lot.
//
// So state moves out of the settings document. One file per plugin identity,
// holding gzip-compressed raw bytes with no base64 in sight. On the same
// measurement that took the largest blob from 4,126,524 bytes to roughly 14% of
// it: a third of the saving is simply not paying base64's 33% inflation, the
// rest is compression. The settings file drops to about twelve kilobytes and
// stays readable XML, which is worth keeping — reading it directly is how every
// persistence question in this project has actually been answered.
//
// The other half of the point is write amplification: only the plugin whose
// state changed gets written, so a bypass toggle no longer touches the others.
//==============================================================================
namespace lighthost::state
{

class Vault;

/** The vault directory that belongs beside a settings file.

    One rule, one copy. It was written out four times -- IconMenu, SelfTest,
    the Chain Test button and the CLI render -- and it decides where every saved
    plugin preset lives, so a single entry point disagreeing would silently
    orphan all of them with no error anywhere.
*/
[[nodiscard]] inline juce::File directoryFor (const juce::File& settingsFile)
{
    return settingsFile.getSiblingFile (settingsFile.getFileNameWithoutExtension() + ".state");
}

//==============================================================================
// The one thing Vault::write could not be made to do: fail.
//
// write() ends in a durability check -- flush(), then getStatus().failed() --
// and only a real I/O fault can fire it: a full disk, a device pulled
// mid-write, a share that went away. write() built its own FileOutputStream,
// so no test could produce one of those, and a check nobody can fire is a
// check nobody should trust. It is not a minor one either: it guards the
// branch where a truncated temporary is renamed over the user's only good
// preset, which is the most expensive failure in this file.
//
// So the stream, and nothing else, becomes injectable. Everything else about
// the write stays real -- the directory is still created, the bytes still go
// through a juce::TemporaryFile, the rename is still
// overwriteTargetFileWithTemporary. A test wants the real temp-and-rename; it
// only needs the stream to fail.
//
// WHY NOT TAKE AN OutputStream& PARAMETER INSTEAD. That moves the
// TemporaryFile and the overwriteTargetFileWithTemporary out to the four call
// sites, so the atomicity of every save would rest on each caller remembering
// to reproduce it. That trades a testability gap for a data-loss surface, in
// the one file that holds the user's presets. Wrong direction.
//
// WHY NOT COMPOSE TO A MemoryBlock AND WRITE THAT. The first reason is fatal
// on its own: a MemoryBlock's flush cannot fail, so the branch under test
// would stop being the branch that runs in production, and the test would be
// green about code nobody ships. The second is cost -- it doubles peak memory
// on a state this header has already measured at 4,126,524 bytes.
//==============================================================================

/** The one piece of I/O Vault::write does not do for itself. */
struct SinkStream
{
    virtual ~SinkStream() = default;

    [[nodiscard]] virtual juce::OutputStream& stream() = 0;

    /** Flushes what a destructor would otherwise flush unchecked, and says
        whether all of it landed.
    */
    [[nodiscard]] virtual bool flushAndCheck() = 0;
};

/** Opens the sink for one file, or returns nullptr when it cannot. */
using SinkFactory = std::function<std::unique_ptr<SinkStream> (const juce::File&)>;

/** The production sink. Returns nullptr when the file could not be opened.

    Inline because this header has no .cpp -- every other definition here is
    inline for the same reason, and four translation units include it.
*/
[[nodiscard]] inline std::unique_ptr<SinkStream> fileSink (const juce::File& file)
{
    // Local to the factory on purpose: the only way to obtain the real sink is
    // to call this, so no other code can accidentally grow a second opinion
    // about what a successful flush means.
    class FileSink final : public SinkStream
    {
    public:
        explicit FileSink (const juce::File& target) : out (target) {}

        [[nodiscard]] bool failedToOpen() const { return out.failedToOpen(); }

        [[nodiscard]] juce::OutputStream& stream() override { return out; }

        [[nodiscard]] bool flushAndCheck() override
        {
            // flush() before getStatus(): FileOutputStream buffers, so bytes the
            // deflate tail handed it may still be unwritten here, and its own
            // destructor would flush them after the last moment we can look.
            // flush() is flushBuffer() plus FlushFileBuffers, and both record a
            // failure in `status`. Saves are already rare -- savePluginStates
            // fingerprints and skips unchanged state -- so the forced flush costs
            // nothing measurable and makes the durability claim above true.
            out.flush();

            return ! out.getStatus().failed();
        }

    private:
        juce::FileOutputStream out;
    };

    auto sink = std::make_unique<FileSink> (file);

    if (sink->failedToOpen())
        return nullptr;

    return sink;
}

class Vault
{
public:
    /** The directory is created on first write, not here, so constructing a
        Vault for a chain that has never saved anything touches no disk.
    */
    explicit Vault (juce::File directoryToUse) : directory (std::move (directoryToUse)) {}

    /** The vault beside a settings file, which is how every caller wants it. */
    [[nodiscard]] static Vault beside (const juce::File& settingsFile)
    {
        return Vault (directoryFor (settingsFile));
    }

    /** TEST ONLY: a vault whose writes go through a stream you supply.

        Named rather than an extra constructor parameter so that the control is
        a grep: `grep -rn withSink Source/` finds this declaration and nothing
        else, and that is the whole guarantee that production still writes to a
        real file. An optional second constructor argument would have no such
        tell -- a default argument is invisible at the call site, so a sink
        passed in shipping code would read as ordinary construction.

        The one-argument constructor above is untouched and still defaults
        makeSink to fileSink, so every existing caller keeps the real thing
        without saying so.
    */
    [[nodiscard]] static Vault withSink (juce::File directoryToUse, SinkFactory sink)
    {
        Vault vault (std::move (directoryToUse));
        vault.makeSink = std::move (sink);

        return vault;
    }

    /** Magic bytes, then a zlib stream. The magic is here so that a future
        format change is detectable rather than being fed to the decompressor and
        reported as corruption.
    */
    static constexpr const char* magic       = "LHS1";
    static constexpr int         magicLength = 4;

    /** How a read turned out.

        `corrupt` exists because it must not be confused with `absent`. A blob
        that is present and unusable is still the only copy there is, so the
        caller has to record the node as un-restored and leave the file alone --
        reporting it as "nothing saved" is precisely how 5.2.0 lost a preset
        through the legacy base64 path.
    */
    enum class ReadResult { ok, absent, corrupt };

    [[nodiscard]] juce::File getDirectory() const { return directory; }

    /** A cheap content fingerprint, so a state whose bytes have not changed is
        not gzipped and written again. FNV-1a: not a cryptographic hash and not
        trying to be. It only has to be much cheaper than compressing three
        megabytes, which it comfortably is.
    */
    [[nodiscard]] static juce::uint64 fingerprint (const juce::MemoryBlock& state)
    {
        juce::uint64 hash = 14695981039346656037ULL;
        const auto*  bytes = static_cast<const juce::uint8*> (state.getData());

        for (size_t i = 0; i < state.getSize(); ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }

        return hash;
    }

    /** RFC 1950 Adler-32, which is the checksum a zlib stream ends with.

        Blocked by NMAX so the modulo runs once per 5552 bytes rather than once
        per byte: a real plugin state measured four megabytes, and this runs on
        the message thread during startup for every plugin in the chain.
    */
    [[nodiscard]] static juce::uint32 adler32 (const void* data, size_t numBytes) noexcept
    {
        constexpr juce::uint32 base = 65521;
        constexpr size_t       nmax = 5552;

        const auto* p = static_cast<const juce::uint8*> (data);
        juce::uint32 a = 1, b = 0;

        while (numBytes > 0)
        {
            const auto block = juce::jmin (nmax, numBytes);

            for (size_t i = 0; i < block; ++i)
            {
                a += p[i];
                b += a;
            }

            a %= base;
            b %= base;
            p += block;
            numBytes -= block;
        }

        return (b << 16) | a;
    }

    /** The four trailing bytes, big-endian, as zlib writes its Adler-32. */
    [[nodiscard]] static juce::uint32 trailerOf (const juce::File& file, juce::int64 fileSize)
    {
        juce::FileInputStream tail (file);

        if (tail.failedToOpen() || ! tail.setPosition (fileSize - 4))
            return 0;

        juce::uint8 bytes[4] = {};

        if (tail.read (bytes, 4) != 4)
            return 0;

        return (static_cast<juce::uint32> (bytes[0]) << 24)
             | (static_cast<juce::uint32> (bytes[1]) << 16)
             | (static_cast<juce::uint32> (bytes[2]) << 8)
             |  static_cast<juce::uint32> (bytes[3]);
    }

    [[nodiscard]] juce::File fileFor (const juce::String& identity) const
    {
        return directory.getChildFile (identity + ".lhs");
    }

    //==========================================================================
    /** Writes the state for one identity.

        Returns false when the write did not land. A caller that is migrating out
        of the old format must not discard its copy until this has returned true,
        or a failed write becomes lost user state.

        An empty block is refused, not written and not treated as an erase.
        Clearing is erase()'s job and has to stay a separate verb. This used to
        erase, and the only production caller that can arrive here with an empty
        block is savePluginStates, which gets one when a plugin's
        getStateInformation fails transiently -- a VST2 getChunk that returned
        nothing, or a plugin in a bad state. Reading that as "the user cleared
        the preset" deleted the good .lhs AND returned true, so the save path
        recorded a success, updated lastWrittenState and dropped the pre-5.0.0
        blob as well. Both copies gone, nothing logged. restoreInto already
        treats an empty block as "nothing stored" and refuses to overwrite; the
        two halves now agree on what empty means.
    */
    [[nodiscard]] bool write (const juce::String& identity,
                              const juce::MemoryBlock& state) const
    {
        if (identity.isEmpty() || state.getSize() == 0)
            return false;

        if (! directory.createDirectory())
            return false;

        // Through a temporary and then moved into place: an interrupted write
        // leaves the previous state intact rather than a truncated file, which
        // matters because a truncated state file is indistinguishable from a
        // plugin whose preset legitimately changed.
        juce::TemporaryFile temp (fileFor (identity));

        {
            auto sink = makeSink (temp.getFile());

            if (sink == nullptr)
                return false;

            if (! sink->stream().write (magic, static_cast<size_t> (magicLength)))
                return false;

            {
                // Scoped INSIDE the sink, not beside it. GZIPCompressorOutputStream
                // writes its deflate tail from its destructor, via
                // GZIPCompressorHelper::finish, which is
                // `while (! finished) doNextBlock (...)` with every return value
                // discarded -- so the last bytes of every file used to be written
                // with nobody checking, and then overwriteTargetFileWithTemporary
                // renamed a truncated file over the good one and returned true. A
                // same-volume rename needs no free space, so a full disk hit
                // exactly that path.
                //
                // The gzip.write check below cannot stand in for this. zlib
                // buffers with Z_NO_FLUSH, so for any state small enough that
                // deflate emits nothing before Z_FINISH, gzip.write returns true
                // having put zero bytes on disk and the whole stream is written
                // in the unchecked destructor flush.
                juce::GZIPCompressorOutputStream gzip (sink->stream());

                if (! gzip.write (state.getData(), state.getSize()))
                    return false;
            }

            // The last moment at which a failed write is still visible: after
            // this block the sink is destroyed and the rename below reports
            // only whether the rename worked. See SinkStream above for why the
            // flush is forced, and fileSink for what it is checking.
            if (! sink->flushAndCheck())
                return false;
        }

        return temp.overwriteTargetFileWithTemporary();
    }

    /** Reads the state stored for an identity, and says whether it is usable.

        INTEGRITY IS CHECKED, and it does not need a format change to do it.

        The payload is a zlib stream -- GZIPCompressorOutputStream defaults to
        windowBits = 0 and GZIPDecompressorInputStream to zlibFormat -- and RFC
        1950 ends such a stream with a four-byte big-endian Adler-32 of the
        uncompressed data. So the file already carries a checksum of its own
        contents, and BACKLOG.md was wrong to conclude that detecting a truncated
        file required adding a length or checksum to the header.

        It works on a truncated file for a reason worth stating, because it is
        not obvious: truncation destroys the real trailer, so the last four bytes
        are whatever deflate data happened to land there. Those bytes are then
        compared against the Adler-32 of what actually decompressed, and they
        disagree unless the garbage collides with the true checksum -- about one
        chance in four billion. This is a probabilistic check, not a proof, and
        that is a far better position than the previous one, which was that
        `gzip.readIntoMemoryBlock` discarded its return value and a half-decoded
        preset was handed to `setStateInformation` as though it were whole.

        What the caller must do with `corrupt`: treat it as "there is state here
        and it cannot be used". Do not overwrite it.
    */
    [[nodiscard]] juce::MemoryBlock read (const juce::String& identity,
                                          ReadResult* outcome = nullptr) const
    {
        const auto report = [outcome] (ReadResult r) { if (outcome != nullptr) *outcome = r; };

        juce::MemoryBlock result;
        report (ReadResult::absent);

        if (identity.isEmpty())
            return result;

        const auto file = fileFor (identity);

        if (! file.existsAsFile())
            return result;

        // magic + zlib header + at least one deflate byte + the Adler-32 tail.
        // Anything shorter cannot be a complete stream, so it is corrupt rather
        // than absent: something wrote it and did not finish.
        const auto fileSize = file.getSize();

        if (fileSize < magicLength + 2 + 1 + 4)
        {
            report (ReadResult::corrupt);
            return result;
        }

        juce::FileInputStream in (file);

        if (in.failedToOpen())
            return result;

        char header[magicLength] = {};

        if (in.read (header, magicLength) != magicLength
            || juce::String (header, static_cast<size_t> (magicLength)) != magic)
        {
            report (ReadResult::corrupt);
            return result;
        }

        {
            juce::GZIPDecompressorInputStream gzip (in);
            gzip.readIntoMemoryBlock (result);
        }

        if (result.getSize() == 0)
        {
            report (ReadResult::corrupt);
            result.reset();
            return result;
        }

        if (adler32 (result.getData(), result.getSize()) != trailerOf (file, fileSize))
        {
            report (ReadResult::corrupt);
            result.reset();           // never hand a partial preset to a plugin
            return result;
        }

        report (ReadResult::ok);
        return result;
    }

    [[nodiscard]] bool has (const juce::String& identity) const
    {
        return identity.isNotEmpty() && fileFor (identity).existsAsFile();
    }

    /** Removing state that is not there succeeds: the postcondition is "no state
        stored for this identity", and that already holds.
    */
    [[nodiscard]] bool erase (const juce::String& identity) const
    {
        if (identity.isEmpty())
            return false;

        const auto file = fileFor (identity);
        return (! file.existsAsFile()) || file.deleteFile();
    }

private:
    juce::File directory;

    /** Defaulted rather than taken by the constructor, so the production shape
        of a Vault stays one argument and gets the real file sink by omission.
    */
    SinkFactory makeSink { &fileSink };
};

//==============================================================================
// Clearing every saved state, and saying honestly how it went.
//
// Two halves of one operation that no test could reach, because both lived
// inside IconMenu: the loop that erases each plugin's state, and the sentence
// the user reads afterwards. The sentence has already been wrong once -- an
// unconditional "Deleted saved plugin states" that was printed even on a run
// where every single delete failed -- so it is worth pinning in a test rather
// than in somebody's memory.
//
// The erase itself cannot move down here. IconMenu's erase is
// forgetPluginState, which clears the vault AND the in-memory fingerprint
// cache that decides whether the next save is skipped as unchanged; those two
// have to move together, or a later save compares against a fingerprint for
// bytes that are no longer on disk and skips writing them. So eraseEach takes
// the erase as a callable and stays ignorant of what else it drags along.
//==============================================================================

/** What to tell the user after clearing saved plugin states.

    The wording is pinned, not chosen here. These are the strings IconMenu has
    always shown, moved verbatim so that a test can assert them; changing them
    is a user-visible change and belongs in a commit that says so.
*/
[[nodiscard]] inline juce::String deletionReport (int failed)
{
    // A negative count is unreachable -- eraseEach only ever increments -- and
    // clamping it to the success message is the better of two wrong answers if
    // it ever arrives. Reporting it would put "Could not delete -1 plugins'
    // saved states" in front of a user, inventing a data loss out of an
    // arithmetic slip; the failure a negative would be standing in for is one
    // nothing here can describe anyway.
    if (failed <= 0)
        return "Deleted saved plugin states";

    return "Could not delete " + juce::String (failed)
         + (failed == 1 ? " plugin's saved state."
                        : " plugins' saved states.")
         + " Something else is holding the files open.";
}

/** What a run of eraseEach did.

    `attempted` is not there for the message -- the wording deliberately does
    not say "deleted N of M" -- it is there so a test can tell "all five were
    tried and one failed" apart from "it stopped at the one that failed",
    which are the same `failed` count and very different behaviour.
*/
struct Deletion
{
    int attempted = 0;
    int failed    = 0;
    juce::StringArray failedIdentities;
};

/** Erases the stored state for each identity, and counts what would not go.

    NEVER STOPS EARLY. The user asked for every saved state to be cleared, so
    one file held open by a backup agent must not leave the rest of them on
    disk -- and the count shown afterwards is only honest if every identity
    was actually tried.

    The erase is a callable rather than a Vault because the caller's erase does
    more than the vault's: see the note above this function.
*/
[[nodiscard]] inline Deletion eraseEach (const std::vector<juce::String>& identities,
                                         const std::function<bool (const juce::String&)>& eraseOne)
{
    Deletion result;

    for (const auto& identity : identities)
    {
        ++result.attempted;

        if (eraseOne (identity))
            continue;

        ++result.failed;
        result.failedIdentities.add (identity);
    }

    return result;
}

} // namespace lighthost::state
