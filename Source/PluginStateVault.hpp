#pragma once

#include <juce_core/juce_core.h>

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
            juce::FileOutputStream out (temp.getFile());

            if (out.failedToOpen())
                return false;

            if (! out.write (magic, static_cast<size_t> (magicLength)))
                return false;

            {
                // Scoped INSIDE `out`, not beside it. GZIPCompressorOutputStream
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
                juce::GZIPCompressorOutputStream gzip (out);

                if (! gzip.write (state.getData(), state.getSize()))
                    return false;
            }

            // flush() before getStatus(): FileOutputStream buffers, so bytes the
            // deflate tail handed it may still be unwritten here, and its own
            // destructor would flush them after the last moment we can look.
            // flush() is flushBuffer() plus FlushFileBuffers, and both record a
            // failure in `status`. Saves are already rare -- savePluginStates
            // fingerprints and skips unchanged state -- so the forced flush costs
            // nothing measurable and makes the durability claim above true.
            out.flush();

            if (out.getStatus().failed())
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
};

} // namespace lighthost::state
