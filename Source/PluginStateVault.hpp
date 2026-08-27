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

class Vault
{
public:
    /** The directory is created on first write, not here, so constructing a
        Vault for a chain that has never saved anything touches no disk.
    */
    explicit Vault (juce::File directoryToUse) : directory (std::move (directoryToUse)) {}

    /** Magic bytes, then a zlib stream. The magic is here so that a future
        format change is detectable rather than being fed to the decompressor and
        reported as corruption.
    */
    static constexpr const char* magic       = "LHS1";
    static constexpr int         magicLength = 4;

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

    [[nodiscard]] juce::File fileFor (const juce::String& identity) const
    {
        return directory.getChildFile (identity + ".lhs");
    }

    //==========================================================================
    /** Writes the state for one identity.

        Returns false when the write did not land. A caller that is migrating out
        of the old format must not discard its copy until this has returned true,
        or a failed write becomes lost user state.

        An empty block erases instead of writing, so clearing a preset leaves no
        file behind rather than an empty one.
    */
    [[nodiscard]] bool write (const juce::String& identity,
                              const juce::MemoryBlock& state) const
    {
        if (identity.isEmpty())
            return false;

        if (state.getSize() == 0)
            return erase (identity);

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

            // Scoped so the compressor is flushed and closed before the file
            // stream it writes through goes away. GZIPCompressorOutputStream
            // closes on flush(), so letting it destruct is the only correct
            // order here.
            juce::GZIPCompressorOutputStream gzip (out);

            if (! gzip.write (state.getData(), state.getSize()))
                return false;
        }

        return temp.overwriteTargetFileWithTemporary();
    }

    /** Reads the state for one identity.

        An empty block means "nothing usable stored". Absent, truncated and
        wrong-magic are deliberately not distinguished: every caller treats them
        the same way, as a plugin that starts at its factory defaults.
    */
    [[nodiscard]] juce::MemoryBlock read (const juce::String& identity) const
    {
        juce::MemoryBlock result;

        if (identity.isEmpty())
            return result;

        const auto file = fileFor (identity);

        if (! file.existsAsFile())
            return result;

        juce::FileInputStream in (file);

        if (in.failedToOpen())
            return result;

        char header[magicLength] = {};

        if (in.read (header, magicLength) != magicLength
            || juce::String (header, static_cast<size_t> (magicLength)) != magic)
            return result;

        juce::GZIPDecompressorInputStream gzip (in);
        gzip.readIntoMemoryBlock (result);
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
