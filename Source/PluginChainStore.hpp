#pragma once

#include "GainProcessor.hpp"
#include "Lanes.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <utility>
#include <vector>

//==============================================================================
// The one place that knows how a plugin's chain settings are stored.
//
// WHY THIS EXISTS
//
// Light Host keeps five things per plugin: its position in the chain, its lane,
// whether it is bypassed, its graph node id, and its saved state. Up to 4.0.3
// each of those was read and written ad hoc through a key built on the spot:
//
//     "plugin-" + field + "-" + name + version + pluginFormatName
//
// Two faults followed from that key, and one from having no single owner.
//
// 1. The key is built from concatenated mutable metadata, so it aliases and it
//    moves. A plugin called "EQ" at version "8" and a plugin called "EQ8" with
//    no version produce the same key and share one set of settings. The same
//    plugin scanned from two folders also shares one set, because the path is
//    not part of the key. And when a plugin is updated, its version changes, so
//    every one of its settings is orphaned in place and it comes back with
//    defaults: order, lane, bypass and the saved preset, all silently gone.
//
// 2. Deleting a plugin meant remembering to remove each field by hand at each
//    call site, and one of them was always forgotten. handleDeletePlugin removed
//    order, bypass, state and nodeid but not lane, so a tray-delete left an
//    orphan lane key that the next plugin to occupy that key inherited.
//
// 3. There was no transaction. A settings write followed by a throwing
//    activePluginList mutation left the two halves disagreeing about what the
//    chain contains.
//
// So: identity is derived from what does not change (the plugin's file and its
// format), every field is declared once in fields::all so erasing cannot forget
// one, and writes are staged until commit() so a failure in between can roll the
// whole edit back.
//
// Identity is a hash of file and format rather than a monotonically allocated
// slot id. That still moves if the user relocates the plugin file, which an
// allocated id would survive; an allocated id needs the chain itself to become
// the store's own container rather than a juce::KnownPluginList, which is a
// larger change than this one and is not needed to fix the faults above.
//==============================================================================
namespace lighthost::chain
{
    /** The fields the store owns.

        stageErase() walks fields::all, so a field added here is erased with the
        rest. Tests/PluginChainStoreTests.cpp asserts that no key mentioning a
        plugin survives an erase, which fails if a field is added without being
        listed.
    */
    namespace fields
    {
        inline constexpr const char* order    = "order";
        inline constexpr const char* lane     = "lane";
        inline constexpr const char* bypass   = "bypass";
        inline constexpr const char* nodeId   = "nodeid";
        inline constexpr const char* state    = "state";

        inline constexpr std::array<const char*, 5> all { order, lane, bypass, nodeId, state };
    }

    /** Everything persisted about one plugin in the chain. */
    struct Slot
    {
        juce::PluginDescription description;
        int          order    = 0;
        int          lane     = 0;
        bool         bypassed = false;
        int          nodeId   = 0;
        /** Base64, empty when nothing has been saved yet.

            Read and written only by read() and stage() above, which are
            test-only, so this member is too. The shipped state path does not
            come through here at all: since 5.0.0 a plugin's state is a file in
            the vault, not a string in the settings document.
        */
        juce::String state;
    };

    /** The part of a slot a user edit can change: which plugin, and its two
        per-plugin settings. Position is the index in the vector.

        Node id and saved state are deliberately absent. Those are the host's own
        bookkeeping, not something the user asked for, and treating a new node id
        as an edit would make every Apply look like a change.
    */
    struct ChainEntry
    {
        juce::String identity;
        bool         bypassed = false;
        int          lane     = 0;

        bool operator== (const ChainEntry& other) const
        {
            return identity == other.identity
                && bypassed == other.bypassed
                && lane     == other.lane;
        }

        bool operator!= (const ChainEntry& other) const { return ! operator== (other); }
    };

    /** True when a requested edit would change nothing.

        Every field is compared, which is the whole point: the v4.0.2 bug was that
        a lane change flips neither the order nor the bypass, so an Apply that only
        moved a lane dropdown compared equal and was silently discarded. A field
        added to ChainEntry without being compared here fails the table-driven test
        in Tests/PluginChainStoreTests.cpp.
    */
    [[nodiscard]] inline bool isNoOpEdit (const std::vector<ChainEntry>& current,
                                          const std::vector<ChainEntry>& requested)
    {
        return current == requested;
    }

    //==========================================================================
    // Folding a refreshed chain into one the user has edited but not applied.
    //
    // THE BUG THIS EXISTS FOR
    //
    // The Preferences chain list is STAGED: nothing the user does to it reaches
    // the graph until Apply. But the panel is also refreshed from the committed
    // chain by IconMenu::refreshPreferencesIfOpen, which fires from nine sites
    // -- every tray bypass, move and delete, both ends of an apply, the two
    // rollback paths, and a plugin merely re-declaring its latency, which
    // happens whenever someone switches a plugin to linear phase in its own
    // editor. That refresh overwrote the staged list wholesale. Stage three
    // plugin additions, flip a plugin to linear phase, and all three vanished
    // with no message.
    //
    // THE RULE
    //
    // Staged wins for rows it still knows about; incoming wins for everything
    // else. Reporting the loss instead was considered and rejected: a message
    // does not give the user their work back.
    //
    // Three inputs, not two. BASELINE is what the staged list was built from,
    // and without it a row missing from the incoming chain is ambiguous -- it is
    // either a plugin the user has just added and not applied, or one that was
    // deleted somewhere else while the window was open, and those want opposite
    // answers.

    /** One row of the merged result: which input it came from, and its index
        there.

        Indices rather than descriptions, because a ChainEntry carries only an
        identity hash and cannot rebuild a juce::PluginDescription. The caller
        materialises the plan against the same two vectors it passed in. That is
        what keeps this function pure and testable without a UI.
    */
    struct MergedRow
    {
        enum class Source { staged, incoming };

        Source source;
        size_t index;
    };

    struct Reconciliation
    {
        std::vector<MergedRow> rows;

        /** Staged rows that had been removed at the source of truth, and
            incoming rows that were not in the staged list. Between them they
            are what the user did not do and would otherwise not notice, so
            `droppedStagedRows + addedIncomingRows > 0` is the message gate.
        */
        int droppedStagedRows = 0;
        int addedIncomingRows = 0;
    };

    /** Merges a committed chain into a staged one.

        `staged` is what is on screen, `baseline` is the committed chain the
        staged list was built from, and `incoming` is the committed chain now.
    */
    [[nodiscard]] inline Reconciliation reconcileStagedChain (
        const std::vector<ChainEntry>& staged,
        const std::vector<ChainEntry>& baseline,
        const std::vector<ChainEntry>& incoming)
    {
        Reconciliation result;

        // The common path, and deliberately the first thing tested: with no
        // pending edit the incoming chain is taken verbatim, which is exactly
        // what the code did before any of this existed. A refresh that used to
        // work cannot regress through a merge it never enters.
        if (isNoOpEdit (staged, baseline))
        {
            result.rows.reserve (incoming.size());

            for (size_t i = 0; i < incoming.size(); ++i)
                result.rows.push_back ({ MergedRow::Source::incoming, i });

            return result;
        }

        // ORDINAL matching, not first-hit, for the same reason the row menus
        // resolve by identity: the k-th staged row with a given identity has to
        // mean the k-th one in the other vector, or two copies of one plugin
        // collapse onto each other and one of them is silently lost.
        std::vector<bool> consumed (incoming.size(), false);

        const auto claimIncoming = [&consumed, &incoming] (const juce::String& identity)
        {
            for (size_t i = 0; i < incoming.size(); ++i)
            {
                if (consumed[i] || incoming[i].identity != identity)
                    continue;

                consumed[i] = true;
                return true;
            }

            return false;
        };

        std::map<juce::String, int> baselineRemaining;

        for (const auto& entry : baseline)
            ++baselineRemaining[entry.identity];

        const auto claimBaseline = [&baselineRemaining] (const juce::String& identity)
        {
            const auto found = baselineRemaining.find (identity);

            if (found == baselineRemaining.end() || found->second == 0)
                return false;

            --found->second;
            return true;
        };

        result.rows.reserve (staged.size() + incoming.size());

        for (size_t i = 0; i < staged.size(); ++i)
        {
            // Claimed for EVERY staged row, matched or not, so that the count
            // stays ordinal. Claiming only on the unmatched path makes a second
            // copy of an already-committed plugin -- one the user added -- take
            // the first copy's baseline slot and read as "deleted elsewhere",
            // so the addition is thrown away.
            const bool cameFromBaseline = claimBaseline (staged[i].identity);

            // Still in the committed chain: the staged row wins, bypass, lane,
            // position and all. A consequence worth stating rather than hiding:
            // while an unapplied edit exists, a tray bypass or lane change on a
            // plugin the staged list still holds is NOT shown until Apply. That
            // is the deliberate price of not discarding the user's work, and
            // Tests/ChainReconcileTests.cpp pins it.
            if (claimIncoming (staged[i].identity))
            {
                result.rows.push_back ({ MergedRow::Source::staged, i });
                continue;
            }

            // In neither the incoming chain nor the baseline, so the user put it
            // there and has not applied it yet. This is the headline bug.
            if (! cameFromBaseline)
            {
                result.rows.push_back ({ MergedRow::Source::staged, i });
                continue;
            }

            // It was committed and is not any more, so it was removed somewhere
            // else. The committed chain is the truth about membership.
            ++result.droppedStagedRows;
        }

        // At the END, deliberately. The staged order is the user's most recent
        // statement about order, so an arrival cannot be inserted into it
        // without guessing; appending is the one position that guesses nothing.
        //
        // But only rows the staged chain never knew about. An unconsumed
        // incoming row that IS still in the baseline is one the user deleted in
        // the panel and has not applied yet, and appending it would undo that
        // deletion -- silently, and at the bottom of the list rather than where
        // it used to be.
        //
        // This is what makes the rule symmetric. Rule (2) keeps a staged
        // ADDITION the committed chain has not heard of; without the claim
        // below, a staged DELETION was thrown away on the next refresh. "Staged
        // wins for rows it knows about" has to cover both, or the panel quietly
        // resurrects plugins.
        for (size_t i = 0; i < incoming.size(); ++i)
        {
            if (consumed[i])
                continue;

            // Present in the baseline and absent from staged: the user removed
            // it. Honour that, and do not count it as an arrival -- nothing
            // arrived, and the message must not claim otherwise.
            if (claimBaseline (incoming[i].identity))
                continue;

            result.rows.push_back ({ MergedRow::Source::incoming, i });
            ++result.addedIncomingRows;
        }

        return result;
    }

    class Store
    {
    public:
        /** The store writes into this property set and does not own it. It never
            calls saveIfNeeded: flushing to disk stays the caller's decision, so
            a caller can make several edits and pay for one write.
        */
        explicit Store (juce::PropertySet& settingsToUse) noexcept
            : settings (settingsToUse) {}

        //==========================================================================
        /** Stable identity for a plugin: file, format and unique id, hashed.

            Deliberately excludes name and version. Those change when a plugin is
            updated or renamed, and including them is what orphaned a plugin's
            settings on every update.

            The unique ids are what tell apart the several plugins that a shell
            plugin packs into one file, which a path alone cannot. This is the same
            set of fields JUCE itself uses to decide whether two descriptions are
            the same plugin (PluginDescription::isDuplicateOf), plus the format.
        */
        [[nodiscard]] static juce::String identityOf (const juce::PluginDescription& description)
        {
            const auto raw = description.fileOrIdentifier
                           + "|" + description.pluginFormatName
                           + "|" + juce::String (description.uniqueId)
                           + "|" + juce::String (description.deprecatedUid);

            return juce::String::toHexString (raw.hashCode64());
        }

        [[nodiscard]] static juce::String keyFor (const juce::PluginDescription& description,
                                                  juce::StringRef field)
        {
            return "chain-" + identityOf (description) + "-" + field;
        }

        /** The 4.0.3 key format, for migration only. */
        [[nodiscard]] static juce::String legacyKeyFor (const juce::PluginDescription& description,
                                                        juce::StringRef field)
        {
            return "plugin-" + juce::String (field).toLowerCase() + "-"
                 + description.name + description.version + description.pluginFormatName;
        }

        //==========================================================================
        /** Reads a plugin's slot, staged changes included, defaults for anything
            not stored yet.

            For tests. Nothing shipped calls this -- every production path reads
            the one field it needs through readOrder / readLane / readBypassed /
            readNodeId, because a bypass toggle has no business deserialising a
            four-megabyte state blob it is not going to look at.

            Kept rather than deleted because it is the only way to assert that
            the per-field readers agree with each other: a test that reads five
            fields five ways cannot catch a keyFor() that has drifted for one of
            them, and this can.
        */
        [[nodiscard]] Slot read (const juce::PluginDescription& description) const
        {
            Slot slot;
            slot.description = description;
            slot.order       = valueOf (keyFor (description, fields::order),  {}).getIntValue();
            slot.lane        = valueOf (keyFor (description, fields::lane),   {}).getIntValue();
            slot.bypassed    = valueOf (keyFor (description, fields::bypass), {}).getIntValue() != 0;
            slot.nodeId      = valueOf (keyFor (description, fields::nodeId), {}).getIntValue();
            slot.state       = valueOf (keyFor (description, fields::state),  {});
            return slot;
        }

        [[nodiscard]] int  readOrder    (const juce::PluginDescription& d) const { return valueOf (keyFor (d, fields::order), {}).getIntValue(); }
        [[nodiscard]] int  readLane     (const juce::PluginDescription& d) const { return valueOf (keyFor (d, fields::lane),  {}).getIntValue(); }
        [[nodiscard]] bool readBypassed (const juce::PluginDescription& d) const { return valueOf (keyFor (d, fields::bypass), {}).getIntValue() != 0; }
        [[nodiscard]] int  readNodeId   (const juce::PluginDescription& d) const { return valueOf (keyFor (d, fields::nodeId), {}).getIntValue(); }
        [[nodiscard]] juce::String readState (const juce::PluginDescription& d) const { return valueOf (keyFor (d, fields::state), {}); }

        //==========================================================================
        // Staged writes. Nothing reaches the property set until commit().

        void stageOrder    (const juce::PluginDescription& d, int value)  { stageValue (keyFor (d, fields::order),  juce::String (value)); }
        void stageLane     (const juce::PluginDescription& d, int value)  { stageValue (keyFor (d, fields::lane),   juce::String (value)); }
        void stageBypassed (const juce::PluginDescription& d, bool value) { stageValue (keyFor (d, fields::bypass), value ? "1" : "0"); }
        void stageNodeId   (const juce::PluginDescription& d, int value)  { stageValue (keyFor (d, fields::nodeId), juce::String (value)); }
        /** Stages a saved state. An empty state removes the key rather than
            storing a blank one, so clearing a preset leaves no trace and the
            settings file does not accumulate empty blobs.
        */
        void stageState (const juce::PluginDescription& d, const juce::String& value)
        {
            const auto key = keyFor (d, fields::state);

            if (value.isEmpty())
            {
                pendingWrites.erase (key);
                pendingRemovals.insert (key);
                return;
            }

            stageValue (key, value);
        }

        /** Stages every field of a slot.

            For tests, and the write-side counterpart to read() above. Nothing
            shipped calls it: callers that changed one field stage that one
            instead, because the state blob can be large and rewriting it on a
            bypass toggle is pure cost.

            Kept for the same reason as read() -- a round trip through both is
            what pins the whole key scheme in one assertion.
        */
        void stage (const Slot& slot)
        {
            stageOrder    (slot.description, slot.order);
            stageLane     (slot.description, slot.lane);
            stageBypassed (slot.description, slot.bypassed);
            stageNodeId   (slot.description, slot.nodeId);
            stageState    (slot.description, slot.state);
        }

        /** Stages removal of every field. This is the whole reason the field list
            exists in one place: a plugin cannot leave part of itself behind.
        */
        void stageErase (const juce::PluginDescription& description)
        {
            for (const auto* field : fields::all)
            {
                const auto key = keyFor (description, field);
                pendingWrites.erase (key);
                pendingRemovals.insert (key);
            }
        }

        //==========================================================================
        // Lane trims.
        //
        // These are not per-plugin and deliberately not part of Slot. A lane is not
        // an entity anywhere in this codebase: it is an integer carried by each
        // plugin, and lanes exist only for as long as GraphTopology groups plugins
        // by it. So a lane's trim cannot belong to any one plugin. Keeping it in
        // Slot would mean the trim vanished when its plugin was deleted and
        // travelled with a plugin moved to another lane, neither of which is what a
        // lane trim means.
        //
        // Stored in decibels, so the settings file stays readable and the UI needs
        // no conversion. Read clamped: the value is user-editable on disk.

        [[nodiscard]] static juce::String laneGainKey (int lane)
        {
            // "lanegain-N" cannot collide with a plugin identity, which is hex.
            return "chain-lanegain-" + juce::String (juce::jlimit (0, lighthost::kMaxLane, lane));
        }

        [[nodiscard]] float readLaneGainDb (int lane) const
        {
            const auto stored = valueOf (laneGainKey (lane), {});

            if (stored.isEmpty())
                return gain::kDefaultDb;

            return gain::clampDb (static_cast<float> (stored.getDoubleValue()));
        }

        void stageLaneGainDb (int lane, float decibels)
        {
            stageValue (laneGainKey (lane), juce::String (gain::clampDb (decibels), 2));
        }

        //==========================================================================
        /** Reserves the next graph node id. Staged like everything else, so an
            abandoned edit does not burn an id.
        */
        [[nodiscard]] int allocateNodeId()
        {
            const auto stored = valueOf (kNextNodeIdKey, "1").getIntValue();
            const auto nodeId = stored > 0 ? stored : 1;
            stageValue (kNextNodeIdKey, juce::String (nodeId + 1));
            return nodeId;
        }

        //==========================================================================
        [[nodiscard]] bool hasPendingChanges() const noexcept
        {
            return ! pendingWrites.empty() || ! pendingRemovals.empty();
        }

        /** Applies every staged change. Does not write to disk; the caller calls
            saveIfNeeded() on its PropertiesFile when it wants that.
        */
        void commit()
        {
            for (const auto& key : pendingRemovals)
                settings.removeValue (key);

            for (const auto& write : pendingWrites)
                settings.setValue (write.first, write.second);

            pendingRemovals.clear();
            pendingWrites.clear();
        }

        /** Throws away every staged change, leaving the property set untouched. */
        void rollback() noexcept
        {
            pendingRemovals.clear();
            pendingWrites.clear();
        }

        //==========================================================================
        static constexpr int kFormatVersion = 1;

        [[nodiscard]] int formatVersion() const
        {
            return settings.getIntValue (kFormatVersionKey, 0);
        }

        /** Moves 4.0.3's per-plugin keys into the stable namespace, once.

            Returns the number of plugins whose settings were carried over. Writes
            directly rather than staging: this runs at startup before anything can
            be rolled back, and a half-applied migration is worse than none.

            Orphaned legacy keys (a plugin no longer in the chain, or one whose
            aliased key was shared) are swept only when the chain is non-empty. An
            empty chain cannot be told apart from a chain whose XML failed to load,
            and in that case the keys are the only surviving record of the user's
            settings.
        */
        int migrateIfNeeded (const std::vector<juce::PluginDescription>& chain)
        {
            if (formatVersion() >= kFormatVersion)
                return 0;

            int migrated = 0;

            for (const auto& description : chain)
            {
                bool movedAnything = false;

                for (const auto* field : fields::all)
                {
                    const auto legacyKey = legacyKeyFor (description, field);

                    if (! settings.containsKey (legacyKey))
                        continue;

                    const auto key = keyFor (description, field);

                    // A value already in the new namespace wins: it is either
                    // newer, or it was written by a plugin whose legacy key
                    // aliased with this one.
                    if (! settings.containsKey (key))
                    {
                        settings.setValue (key, settings.getValue (legacyKey));
                        movedAnything = true;
                    }

                    settings.removeValue (legacyKey);
                }

                if (movedAnything)
                    ++migrated;
            }

            if (! chain.empty())
                purgeLegacyKeys();

            settings.setValue (kFormatVersionKey, kFormatVersion);
            return migrated;
        }

        /** Orders a chain by its stored order values, breaking ties by identity.

            The tie-break is load-bearing. Order values written by this version are
            unique indices, but settings migrated from 4.0.3 carry that version's
            `time(nullptr) + offset` values, and two plugins added within the same
            second share one. std::sort leaves equal keys in an unspecified order,
            so a migrated chain with a tie could come back in a different order on
            each launch until the next Apply rewrote the values. Identity is stable
            and unique per plugin, so ordering on it makes the result total.
        */
        [[nodiscard]] static std::vector<juce::PluginDescription> sortByOrder (
            std::vector<std::pair<int, juce::PluginDescription>> entries)
        {
            std::sort (entries.begin(), entries.end(),
                       [] (const auto& a, const auto& b)
                       {
                           if (a.first != b.first)
                               return a.first < b.first;

                           return identityOf (a.second) < identityOf (b.second);
                       });

            std::vector<juce::PluginDescription> ordered;
            ordered.reserve (entries.size());

            for (auto& entry : entries)
                ordered.push_back (std::move (entry.second));

            return ordered;
        }

        /** Builds the comparable form of a chain edit. Missing bypass or lane
            entries default, which is what the Preferences window sends for a
            plugin it has just added.
        */
        [[nodiscard]] static std::vector<ChainEntry> entriesFor (
            const std::vector<juce::PluginDescription>& chain,
            const std::vector<bool>& bypassed,
            const std::vector<int>& lanes)
        {
            std::vector<ChainEntry> entries;
            entries.reserve (chain.size());

            for (size_t i = 0; i < chain.size(); ++i)
                entries.push_back ({ identityOf (chain[i]),
                                     i < bypassed.size() ? bypassed[i] : false,
                                     i < lanes.size()    ? lanes[i]    : 0 });

            return entries;
        }

        /** The comparable form of what is currently stored for a chain. */
        [[nodiscard]] std::vector<ChainEntry> entriesFor (
            const std::vector<juce::PluginDescription>& chain) const
        {
            std::vector<ChainEntry> entries;
            entries.reserve (chain.size());

            for (const auto& description : chain)
                entries.push_back ({ identityOf (description),
                                     readBypassed (description),
                                     readLane (description) });

            return entries;
        }

    private:
        //==========================================================================
        [[nodiscard]] juce::String valueOf (const juce::String& key,
                                            const juce::String& fallback) const
        {
            if (pendingRemovals.find (key) != pendingRemovals.end())
                return fallback;

            const auto staged = pendingWrites.find (key);
            if (staged != pendingWrites.end())
                return staged->second;

            return settings.getValue (key, fallback);
        }

        void stageValue (const juce::String& key, const juce::String& value)
        {
            pendingRemovals.erase (key);
            pendingWrites[key] = value;
        }

        /** Removes every remaining key in the 4.0.3 namespace. Anything left
            after migration belongs to no plugin in the chain.
        */
        void purgeLegacyKeys()
        {
            // keys::pluginList and keys::pluginListActive do not carry the
            // dash, so the prefix cannot match the plugin lists themselves.
            // Named rather than spelled out here, because a comment that
            // restates a literal is one more copy to drift.
            const auto& all = settings.getAllProperties();
            std::vector<juce::String> doomed;

            for (const auto& key : all.getAllKeys())
                if (key.startsWith ("plugin-"))
                    doomed.push_back (key);

            for (const auto& key : doomed)
                settings.removeValue (key);
        }

        static constexpr const char* kNextNodeIdKey    = "nextPluginNodeId";
        static constexpr const char* kFormatVersionKey = "chainSettingsVersion";

        juce::PropertySet& settings;
        std::map<juce::String, juce::String> pendingWrites;
        std::set<juce::String> pendingRemovals;
    };
}
