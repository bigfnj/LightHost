#include "../Source/PluginChainStore.hpp"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

//==============================================================================
// Contract tests for the chain settings store.
//
// juce::PropertySet is used directly as the settings: juce::PropertiesFile is a
// PropertySet, so an in-memory one behaves identically without touching a disk
// or needing a temp directory. No fake, no mock, no new dependency.
//==============================================================================
namespace
{
    using namespace lighthost::chain;

    juce::PluginDescription describe (const juce::String& name,
                                     const juce::String& version = "1.0.0",
                                     const juce::String& path = "C:/VST3/Thing.vst3",
                                     const juce::String& format = "VST3")
    {
        juce::PluginDescription description;
        description.name             = name;
        description.version          = version;
        description.fileOrIdentifier = path;
        description.pluginFormatName = format;
        return description;
    }

    /** The 4.0.3 key format, written out by hand so the migration test pins the
        real old format rather than whatever the code now says it was.
    */
    juce::String legacyKey (const juce::String& field, const juce::PluginDescription& d)
    {
        return "plugin-" + field + "-" + d.name + d.version + d.pluginFormatName;
    }

    int countKeysMentioning (juce::PropertySet& settings, const juce::String& identity)
    {
        int found = 0;

        for (const auto& key : settings.getAllProperties().getAllKeys())
            if (key.contains (identity))
                ++found;

        return found;
    }
}

//==============================================================================
class PluginChainIdentityTests final : public juce::UnitTest
{
public:
    PluginChainIdentityTests()
        : juce::UnitTest ("Chain settings identity", "PluginChain") {}

    void runTest() override
    {
        beginTest ("a plugin update does not change its identity");
        {
            // The bug: the old key included the version, so updating a plugin
            // orphaned its order, lane, bypass and saved preset in place.
            const auto before = describe ("Pro-Q", "3.24");
            const auto after  = describe ("Pro-Q", "3.25");

            expectEquals (Store::identityOf (after), Store::identityOf (before));
        }

        beginTest ("a rename does not change its identity either");
        {
            const auto before = describe ("Pro-Q 3", "3.24");
            const auto after  = describe ("Pro-Q 4", "3.24");

            expectEquals (Store::identityOf (after), Store::identityOf (before));
        }

        beginTest ("names and versions that concatenate alike stay distinct");
        {
            // "EQ" + "8" and "EQ8" + "" produced the same old key, so two
            // different plugins shared one set of settings.
            const auto first  = describe ("EQ", "8", "C:/VST3/EQ.vst3");
            const auto second = describe ("EQ8", "", "C:/VST3/EQ8.vst3");

            expect (Store::identityOf (first) != Store::identityOf (second));
            expectEquals (legacyKey ("order", first), legacyKey ("order", second),
                          "precondition: the old keys really did collide");
        }

        beginTest ("the same plugin in two folders gets two identities");
        {
            const auto system = describe ("Thing", "1.0", "C:/Program Files/VST3/Thing.vst3");
            const auto local  = describe ("Thing", "1.0", "D:/Plugins/Thing.vst3");

            expect (Store::identityOf (system) != Store::identityOf (local));
        }

        beginTest ("the same file in two formats gets two identities");
        {
            const auto asVst2 = describe ("Thing", "1.0", "C:/Plugins/Thing.dll", "VST");
            const auto asVst3 = describe ("Thing", "1.0", "C:/Plugins/Thing.dll", "VST3");

            expect (Store::identityOf (asVst2) != Store::identityOf (asVst3));
        }

        beginTest ("two plugins inside one shell file get two identities");
        {
            // A VST2 shell packs several plugins into one .dll, so they share a
            // path and are told apart only by their unique id. An identity built
            // from the path alone would give them one set of settings.
            auto first  = describe ("Shell Comp", "1.0", "C:/Plugins/Shell.dll", "VST");
            auto second = describe ("Shell EQ",   "1.0", "C:/Plugins/Shell.dll", "VST");
            first.uniqueId  = 0x11111111;
            second.uniqueId = 0x22222222;

            expect (Store::identityOf (first) != Store::identityOf (second));
        }

        beginTest ("the deprecated uid still distinguishes an old scan");
        {
            // Plugins scanned by an older JUCE have uniqueId 0 and only
            // deprecatedUid set. They must still be told apart.
            auto first  = describe ("Shell Comp", "1.0", "C:/Plugins/Shell.dll", "VST");
            auto second = describe ("Shell EQ",   "1.0", "C:/Plugins/Shell.dll", "VST");
            first.deprecatedUid  = 0x33333333;
            second.deprecatedUid = 0x44444444;

            expect (Store::identityOf (first) != Store::identityOf (second));
        }
    }
};

static PluginChainIdentityTests pluginChainIdentityTests;

//==============================================================================
class PluginChainStoreTests final : public juce::UnitTest
{
public:
    PluginChainStoreTests()
        : juce::UnitTest ("Chain settings store", "PluginChain") {}

    void runTest() override
    {
        beginTest ("an unknown plugin reads back as defaults");
        {
            juce::PropertySet settings;
            const Store store (settings);

            const auto slot = store.read (describe ("Nothing"));

            expectEquals (slot.order, 0);
            expectEquals (slot.lane, 0);
            expect (! slot.bypassed);
            expectEquals (slot.nodeId, 0);
            expect (slot.state.isEmpty());
        }

        beginTest ("every field round-trips through a commit");
        {
            juce::PropertySet settings;
            Store store (settings);

            Slot slot;
            slot.description = describe ("Pro-Q");
            slot.order       = 17;
            slot.lane        = 2;
            slot.bypassed    = true;
            slot.nodeId      = 42;
            slot.state       = "c2F2ZWQ=";

            store.stage (slot);
            store.commit();

            const Store reader (settings);
            const auto restored = reader.read (slot.description);

            expectEquals (restored.order, 17);
            expectEquals (restored.lane, 2);
            expect (restored.bypassed);
            expectEquals (restored.nodeId, 42);
            expectEquals (restored.state, juce::String ("c2F2ZWQ="));
        }

        beginTest ("a staged change is visible to the store but not to the settings");
        {
            juce::PropertySet settings;
            Store store (settings);
            const auto plugin = describe ("Pro-Q");

            store.stageLane (plugin, 3);

            expectEquals (store.readLane (plugin), 3, "the store should see its own staged write");
            expectEquals (settings.getAllProperties().size(), 0,
                          "nothing should reach the settings before commit");
            expect (store.hasPendingChanges());

            store.commit();

            expect (! store.hasPendingChanges());
            expectEquals (settings.getIntValue (Store::keyFor (plugin, fields::lane), -1), 3);
        }

        beginTest ("rollback leaves the previous values in place");
        {
            // This is the transaction the chain edit needs: stage the settings,
            // mutate the plugin list, and if that throws, undo the settings
            // rather than leaving the two halves disagreeing.
            juce::PropertySet settings;
            Store store (settings);
            const auto plugin = describe ("Pro-Q");

            store.stageLane (plugin, 1);
            store.commit();

            store.stageLane (plugin, 3);
            store.stageBypassed (plugin, true);
            store.rollback();

            expect (! store.hasPendingChanges());
            expectEquals (store.readLane (plugin), 1, "rollback should restore the committed lane");
            expect (! store.readBypassed (plugin));
        }

        beginTest ("erasing a plugin leaves nothing of it behind");
        {
            // The lane key was the one handleDeletePlugin forgot. This asserts
            // over whatever fields exist rather than a hand-written list, so a
            // field added without being registered fails here.
            juce::PropertySet settings;
            Store store (settings);

            Slot slot;
            slot.description = describe ("Pro-Q");
            slot.order       = 3;
            slot.lane        = 2;
            slot.bypassed    = true;
            slot.nodeId      = 9;
            slot.state       = "blob";

            store.stage (slot);
            store.commit();

            const auto identity = Store::identityOf (slot.description);
            expect (countKeysMentioning (settings, identity) > 0, "precondition: it was stored");

            store.stageErase (slot.description);
            store.commit();

            expectEquals (countKeysMentioning (settings, identity), 0,
                          "a field survived the erase");
        }

        beginTest ("clearing a saved state removes the key rather than blanking it");
        {
            juce::PropertySet settings;
            Store store (settings);
            const auto plugin = describe ("Pro-Q");

            store.stageState (plugin, "blob");
            store.commit();
            expect (settings.containsKey (Store::keyFor (plugin, fields::state)));

            store.stageState (plugin, {});
            store.commit();

            expect (! settings.containsKey (Store::keyFor (plugin, fields::state)),
                    "an empty state should leave no key behind");
            expect (store.readState (plugin).isEmpty());
        }

        beginTest ("erasing one plugin does not touch another");
        {
            juce::PropertySet settings;
            Store store (settings);

            const auto doomed   = describe ("Doomed", "1.0", "C:/VST3/Doomed.vst3");
            const auto survivor = describe ("Survivor", "1.0", "C:/VST3/Survivor.vst3");

            store.stageLane (doomed, 1);
            store.stageLane (survivor, 2);
            store.commit();

            store.stageErase (doomed);
            store.commit();

            expectEquals (store.readLane (survivor), 2);
            expectEquals (store.readLane (doomed), 0);
        }

        beginTest ("node ids are handed out once each, and survive a commit");
        {
            juce::PropertySet settings;
            Store store (settings);

            const auto first  = store.allocateNodeId();
            const auto second = store.allocateNodeId();

            expect (second > first, "an id was handed out twice");

            store.commit();

            Store later (settings);
            expect (later.allocateNodeId() > second, "the counter did not persist");
        }

        beginTest ("an abandoned edit does not burn a node id");
        {
            juce::PropertySet settings;
            Store store (settings);

            const auto abandoned = store.allocateNodeId();
            store.rollback();

            expectEquals (store.allocateNodeId(), abandoned);
        }
    }
};

static PluginChainStoreTests pluginChainStoreTests;

//==============================================================================
class PluginChainMigrationTests final : public juce::UnitTest
{
public:
    PluginChainMigrationTests()
        : juce::UnitTest ("Chain settings migration", "PluginChain") {}

    void runTest() override
    {
        beginTest ("4.0.3 settings are carried over and the old keys are dropped");
        {
            juce::PropertySet settings;
            const auto plugin = describe ("Pro-Q", "3.24");

            settings.setValue (legacyKey ("order",  plugin), 100);
            settings.setValue (legacyKey ("lane",   plugin), 2);
            settings.setValue (legacyKey ("bypass", plugin), true);
            settings.setValue (legacyKey ("nodeid", plugin), 7);
            settings.setValue (legacyKey ("state",  plugin), "c2F2ZWQ=");

            Store store (settings);
            expectEquals (store.migrateIfNeeded ({ plugin }), 1);

            const auto slot = store.read (plugin);
            expectEquals (slot.order, 100);
            expectEquals (slot.lane, 2);
            expect (slot.bypassed);
            expectEquals (slot.nodeId, 7);
            expectEquals (slot.state, juce::String ("c2F2ZWQ="));

            expect (! settings.containsKey (legacyKey ("order", plugin)),
                    "the old key should be gone, not duplicated");
        }

        beginTest ("a migrated plugin keeps its settings across a version bump");
        {
            // The end of the story: 4.0.3 stored under a versioned key, the
            // plugin updates, and the settings still belong to it.
            juce::PropertySet settings;
            const auto before = describe ("Pro-Q", "3.24");

            settings.setValue (legacyKey ("lane", before), 3);

            Store store (settings);
            store.migrateIfNeeded ({ before });

            const auto updated = describe ("Pro-Q", "3.25");
            expectEquals (store.readLane (updated), 3,
                          "the update orphaned its settings all over again");
        }

        beginTest ("migration runs once");
        {
            juce::PropertySet settings;
            const auto plugin = describe ("Pro-Q");

            settings.setValue (legacyKey ("lane", plugin), 2);

            Store store (settings);
            expectEquals (store.migrateIfNeeded ({ plugin }), 1);
            expectEquals (store.migrateIfNeeded ({ plugin }), 0, "it migrated twice");
            expectEquals (store.formatVersion(), Store::kFormatVersion);

            // A legacy key written after migration is not picked up, which is
            // correct: nothing writes that format any more.
            settings.setValue (legacyKey ("lane", plugin), 3);
            expectEquals (store.migrateIfNeeded ({ plugin }), 0);
            expectEquals (store.readLane (plugin), 2);
        }

        beginTest ("a value already in the new namespace is not overwritten");
        {
            juce::PropertySet settings;
            const auto plugin = describe ("Pro-Q");

            settings.setValue (Store::keyFor (plugin, fields::lane), 1);
            settings.setValue (legacyKey ("lane", plugin), 3);

            Store store (settings);
            store.migrateIfNeeded ({ plugin });

            expectEquals (store.readLane (plugin), 1);
            expect (! settings.containsKey (legacyKey ("lane", plugin)));
        }

        beginTest ("keys belonging to no plugin in the chain are swept up");
        {
            juce::PropertySet settings;
            const auto inChain = describe ("Kept", "1.0", "C:/VST3/Kept.vst3");
            const auto gone    = describe ("Deleted", "1.0", "C:/VST3/Deleted.vst3");

            settings.setValue (legacyKey ("lane", inChain), 1);
            settings.setValue (legacyKey ("lane", gone), 2);   // leaked by a tray-delete

            Store store (settings);
            store.migrateIfNeeded ({ inChain });

            expectEquals (store.readLane (inChain), 1);
            expect (! settings.containsKey (legacyKey ("lane", gone)),
                    "the orphaned key should have been swept");
        }

        beginTest ("an empty chain sweeps nothing");
        {
            // An empty chain and a chain whose XML failed to load look the same
            // from here, and in the second case these keys are the only record
            // of the user's settings.
            juce::PropertySet settings;
            const auto plugin = describe ("Pro-Q");

            settings.setValue (legacyKey ("lane", plugin), 2);

            Store store (settings);
            expectEquals (store.migrateIfNeeded ({}), 0);

            expect (settings.containsKey (legacyKey ("lane", plugin)),
                    "settings were destroyed for a chain that may only have failed to load");
        }

        beginTest ("the plugin lists themselves are never swept");
        {
            juce::PropertySet settings;
            const auto plugin = describe ("Pro-Q");

            settings.setValue ("pluginList", "<KNOWNPLUGINS/>");
            settings.setValue ("pluginListActive", "<KNOWNPLUGINS/>");
            settings.setValue (legacyKey ("lane", plugin), 1);

            Store store (settings);
            store.migrateIfNeeded ({ plugin });

            expect (settings.containsKey ("pluginList"));
            expect (settings.containsKey ("pluginListActive"));
        }
    }
};

static PluginChainMigrationTests pluginChainMigrationTests;
