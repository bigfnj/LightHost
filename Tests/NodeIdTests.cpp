#include "../Source/NodeIds.hpp"
#include "../Source/PluginChainStore.hpp"

#include <juce_core/juce_core.h>

#include <set>

//==============================================================================
// The reserved node-id scheme, and the container behaviour that keeps the
// settings keys unique.
//
// Two invariants that nothing was watching.
//
// THE ID BANDS MUST NOT OVERLAP. Plugin nodes are allocated from 1 upwards and
// persisted in the settings file; everything the host inserts itself lives in
// reserved bands above them. A collision here does not fail loudly -- it means
// one inserted node silently replaces another in the graph, or a plugin node is
// overwritten by a lane trim, and the symptom is audio going to the wrong place.
// The values are also written into user settings, so they cannot be changed
// freely once shipped: pinning them is the point.
//
// THE CHAIN CONTAINER MUST KEEP REFUSING DUPLICATES. The settings key for a
// plugin is a hash of its file, format and unique ids, so two instances of the
// same plugin in one chain would collide on every stored value: order, lane,
// bypass and saved preset. That collision is real and currently unreachable,
// because the chain lives in a juce::KnownPluginList and addType refuses a
// duplicate on the same field set. So the thing standing between this project
// and that bug is a JUCE container's behaviour, which was assumed rather than
// asserted. DECISIONS.md records the decision to leave it that way; this makes
// the assumption it rests on machine-checked, so replacing the container cannot
// quietly unmask the collision.
//==============================================================================
namespace
{
    namespace ids = lighthost::nodeids;

    using NodeID = juce::AudioProcessorGraph::NodeID;

    /** A description differing from another only in the fields identity ignores. */
    juce::PluginDescription describe (const juce::String& file,
                                     const juce::String& format,
                                     int uniqueId,
                                     const juce::String& name,
                                     const juce::String& version = "1.0.0")
    {
        juce::PluginDescription pd;
        pd.fileOrIdentifier  = file;
        pd.pluginFormatName  = format;
        pd.uniqueId          = uniqueId;
        pd.deprecatedUid     = uniqueId;
        pd.name              = name;
        pd.version           = version;
        pd.manufacturerName  = "Test";
        return pd;
    }
}

//==============================================================================
class NodeIdTests final : public juce::UnitTest
{
public:
    NodeIdTests()
        : juce::UnitTest ("Reserved node ids", "NodeIds") {}

    void runTest() override
    {
        beginTest ("the published ids are exactly these numbers");
        {
            // Pinned deliberately. Plugin node ids are stored in the settings
            // file, and the reserved ids have to stay clear of them across
            // versions, so these are part of the on-disk contract rather than
            // an implementation detail.
            expectEquals (ids::input.uid,  static_cast<juce::uint32> (1'000'000));
            expectEquals (ids::output.uid, static_cast<juce::uint32> (1'000'001));
            expectEquals (ids::laneGain (0).uid, static_cast<juce::uint32> (1'000'010));
            expectEquals (ids::probe (0).uid,    static_cast<juce::uint32> (1'000'100));
        }

        beginTest ("every reserved id is distinct");
        {
            std::set<juce::uint32> seen { ids::input.uid, ids::output.uid };

            for (int lane = 0; lane <= lighthost::kMaxLane; ++lane)
                expect (seen.insert (ids::laneGain (lane).uid).second,
                        "a lane trim id collided with another reserved id");

            for (int i = 0; i < ids::maxProbes; ++i)
                expect (seen.insert (ids::probe (i).uid).second,
                        "a probe id collided with another reserved id");

            const auto expected = 2u
                                + static_cast<unsigned> (lighthost::kMaxLane + 1)
                                + static_cast<unsigned> (ids::maxProbes);

            expectEquals (static_cast<unsigned> (seen.size()), expected);
        }

        beginTest ("the lane band cannot grow into the probe band");
        {
            // The failure this catches: someone raises kNumLanes past the gap
            // and the highest lane trim lands on probe 0, so opening the signal
            // view silently replaces a lane trim. There are 90 ids of headroom.
            expect (ids::laneGain (lighthost::kMaxLane).uid < ids::probe (0).uid,
                    "the lane trim band has run into the probe band");
        }

        beginTest ("the probe band cannot grow into anything above it");
        {
            // Nothing is reserved above the probes today, so this pins the top of
            // the range for whatever is added next.
            expect (ids::probe (ids::maxProbes - 1).uid < 1'000'200u,
                    "the probe band has outgrown its 100-id allocation");
        }

        beginTest ("reserved ids stay clear of plugin node ids");
        {
            // Plugin nodes are allocated from 1 upwards. A chain would have to
            // reach a million plugins to reach the reserved range.
            expect (ids::input.uid > 1'000u);
            expect (ids::laneGain (0).uid > ids::output.uid);
        }

        beginTest ("an out-of-range lane is clamped, not wrapped");
        {
            // The lane comes from the settings file and nothing else validates
            // it. Without the clamp, a negative lane produces a huge unsigned id
            // and a large one walks into the probe band -- both of which land on
            // some other node rather than failing.
            expectEquals (ids::laneGain (-1).uid,   ids::laneGain (0).uid);
            expectEquals (ids::laneGain (-9999).uid, ids::laneGain (0).uid);
            expectEquals (ids::laneGain (9999).uid, ids::laneGain (lighthost::kMaxLane).uid);

            for (int lane : { -5, -1, 0, 1, 9999 })
                expect (ids::laneGain (lane).uid >= ids::laneGain (0).uid
                            && ids::laneGain (lane).uid <= ids::laneGain (lighthost::kMaxLane).uid,
                        "a clamped lane escaped the reserved lane band");
        }

        beginTest ("an out-of-range probe index is clamped, not wrapped");
        {
            expectEquals (ids::probe (-1).uid, ids::probe (0).uid);
            expectEquals (ids::probe (ids::maxProbes).uid, ids::probe (ids::maxProbes - 1).uid);
            expectEquals (ids::probe (1'000'000).uid, ids::probe (ids::maxProbes - 1).uid);
        }

        beginTest ("consecutive ids are consecutive, so the bands stay readable");
        {
            for (int lane = 1; lane <= lighthost::kMaxLane; ++lane)
                expectEquals (ids::laneGain (lane).uid, ids::laneGain (lane - 1).uid + 1);

            for (int i = 1; i < ids::maxProbes; ++i)
                expectEquals (ids::probe (i).uid, ids::probe (i - 1).uid + 1);
        }

        //======================================================================
        beginTest ("the chain container refuses a second copy of one plugin");
        {
            // This is what makes the settings-key collision unreachable. If it
            // ever stops holding, two chain entries share every stored value.
            juce::KnownPluginList list;

            const auto first = describe ("C:/p/thing.dll", "VST", 0x1234, "Thing");

            expect (list.addType (first));
            expectEquals (list.getNumTypes(), 1);

            expect (! list.addType (first),
                    "the container accepted a duplicate; every stored setting "
                    "for this plugin now collides");
            expectEquals (list.getNumTypes(), 1);
        }

        beginTest ("a renamed or updated plugin is still the same plugin to it");
        {
            // The container and the settings key agree on this, which is why an
            // update does not orphan a preset.
            juce::KnownPluginList list;

            const auto before = describe ("C:/p/thing.dll", "VST", 0x1234, "Thing", "1.0.0");
            const auto after  = describe ("C:/p/thing.dll", "VST", 0x1234, "Thing Pro", "2.0.0");

            expect (list.addType (before));
            expect (! list.addType (after), "an update should replace, not add");
            expectEquals (list.getNumTypes(), 1);

            using Store = lighthost::chain::Store;
            expectEquals (Store::identityOf (before), Store::identityOf (after),
                          "the settings key must survive a rename and a version bump");
        }

        beginTest ("two sub-plugins of one file are different plugins");
        {
            // A shell plugin packs several plugins into one binary, told apart
            // only by their unique ids. Both the container and the key have to
            // see them as distinct or one would overwrite the other.
            juce::KnownPluginList list;

            const auto a = describe ("C:/p/shell.dll", "VST", 0x1111, "Shared Name");
            const auto b = describe ("C:/p/shell.dll", "VST", 0x2222, "Shared Name");

            expect (list.addType (a));
            expect (list.addType (b), "a shell plugin's second sub-plugin was refused");
            expectEquals (list.getNumTypes(), 2);

            using Store = lighthost::chain::Store;
            expect (Store::identityOf (a) != Store::identityOf (b),
                    "two sub-plugins sharing a display name collided on one key");
        }

        beginTest ("removing a plugin removes it");
        {
            juce::KnownPluginList list;

            const auto pd = describe ("C:/p/thing.dll", "VST", 0x1234, "Thing");
            expect (list.addType (pd));

            list.removeType (pd);
            expectEquals (list.getNumTypes(), 0);
        }
    }
};

static NodeIdTests nodeIdTests;
