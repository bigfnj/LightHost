#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <array>
#include <map>
#include <set>
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
        juce::String state;      ///< base64; empty when nothing has been saved yet
    };

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

        /** Stages every field of a slot. Callers that changed one field should
            stage that one instead: the state blob can be large, and rewriting it
            on a bypass toggle is pure cost.
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
            // "pluginList" and "pluginListActive" do not carry the dash, so the
            // prefix cannot match the plugin lists themselves.
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
