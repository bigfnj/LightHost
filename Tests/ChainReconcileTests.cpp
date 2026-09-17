#include "../Source/PluginChainStore.hpp"

#include <juce_core/juce_core.h>

#include <algorithm>

//==============================================================================
// Folding a refreshed chain into one the user has edited but not applied.
//
// Registers the EXISTING "PluginChain" category rather than a new one, because
// chain::ChainEntry and chain::isNoOpEdit are already covered there and the
// subject here is the third function in that family. Tests/TestMain.cpp
// therefore needs no entry for this file -- its orphan-category check would fail
// the run if a new name had been introduced without one.
//
// Every case states, in a comment, what makes it fail. "It would fail if the
// code were wrong" is not that: the point is to name the specific wrong code,
// because a test whose failure mode nobody can state is a test nobody can trust
// to be measuring the thing its name claims.
//==============================================================================
namespace
{
    using namespace lighthost::chain;

    ChainEntry entry (const char* identity, bool bypassed = false, int lane = 0)
    {
        return { juce::String (identity), bypassed, lane };
    }

    /** Exactly what PreferencesContentComponent::setChain does with a plan.

        The plan is indices into the two inputs, so the readable assertion is
        about the chain that comes out of it rather than about the indices. An
        out-of-range index returns nothing rather than reading past the end:
        that is one of the things under test, and indexing it here would take
        the process down instead of failing a named case.
    */
    std::vector<ChainEntry> materialise (const Reconciliation& plan,
                                         const std::vector<ChainEntry>& staged,
                                         const std::vector<ChainEntry>& incoming)
    {
        std::vector<ChainEntry> merged;
        merged.reserve (plan.rows.size());

        for (const auto& row : plan.rows)
        {
            const auto& from = (row.source == MergedRow::Source::staged) ? staged : incoming;

            if (row.index >= from.size())
                return {};

            merged.push_back (from[row.index]);
        }

        return merged;
    }

    /** True when every merged row came from one input.

        Replaces a `stagedWasDirty` flag that no shipped code ever read. The
        fast path's observable signature is that it copies `incoming` whole, so
        asserting THAT tests the behaviour rather than a field kept alive for
        the test that checked it.
    */
    bool allRowsFrom (const Reconciliation& plan, MergedRow::Source source)
    {
        return std::all_of (plan.rows.begin(), plan.rows.end(),
                            [source] (const MergedRow& row) { return row.source == source; });
    }

    juce::String identitiesOf (const std::vector<ChainEntry>& entries)
    {
        juce::StringArray names;

        for (const auto& e : entries)
            names.add (e.identity);

        return names.joinIntoString (",");
    }

    /** A readable rendering of the whole merged chain, so a failure says which
        row is wrong rather than only that some count did not match.
    */
    juce::String describeChain (const std::vector<ChainEntry>& entries)
    {
        juce::StringArray parts;

        for (const auto& e : entries)
            parts.add (e.identity + (e.bypassed ? "/byp" : "/on")
                       + "/L" + juce::String (e.lane));

        return parts.joinIntoString (" ");
    }
}

//==============================================================================
class ChainReconcileTests final : public juce::UnitTest
{
public:
    ChainReconcileTests()
        : juce::UnitTest ("Chain reconcile", "PluginChain") {}

    void runTest() override
    {
        beginTest ("with nothing staged the committed chain is taken verbatim");
        {
            // The regression guard for the fix itself. Before any of this
            // existed, every refresh assigned the committed chain straight over
            // the staged list, and that is still what has to happen when there
            // is no pending edit.
            //
            // FAILS IF: the isNoOpEdit(staged, baseline) early return in
            // reconcileStagedChain is removed, so an unedited list goes through
            // the merge. Measured, not reasoned: three of the four assertions
            // below go red -- the source check, because the surviving B is then
            // taken from `staged` rather than from `incoming`, and both counters,
            // which come back at 1 instead of 0.
            //
            // The chain STRING is unaffected, and this comment used to claim it
            // was ("C would land after B instead of replacing it"). Both routes
            // produce B,C, so that assertion passes under the mutation and
            // describes nothing. The fast path is worth keeping for the counters
            // and the provenance, not for the order.
            const std::vector<ChainEntry> baseline { entry ("A"), entry ("B") };
            const auto staged = baseline;
            const std::vector<ChainEntry> incoming { entry ("B"), entry ("C") };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expect (allRowsFrom (plan, MergedRow::Source::incoming),
                    "the fast path did not fire, so an unedited refresh went through the merge");
            expectEquals (identitiesOf (materialise (plan, staged, incoming)),
                          juce::String ("B,C"));
            expectEquals (plan.droppedStagedRows, 0);
            expectEquals (plan.addedIncomingRows, 0);
        }

        beginTest ("a plugin staged but not applied survives a refresh");
        {
            // The headline bug: stage three additions, flip a plugin to linear
            // phase in its own editor, and all three vanished. That editor
            // change reaches here through IconMenu::handleAsyncUpdate.
            //
            // FAILS IF: the "in neither the incoming chain nor the baseline"
            // branch is removed, so a pending addition is counted as a row
            // deleted elsewhere and dropped. Merged would then be "A" alone.
            const std::vector<ChainEntry> baseline { entry ("A") };
            const std::vector<ChainEntry> staged   { entry ("A"), entry ("B") };
            const std::vector<ChainEntry> incoming { entry ("A") };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (identitiesOf (materialise (plan, staged, incoming)),
                          juce::String ("A,B"));
            expectEquals (plan.droppedStagedRows, 0);
            expectEquals (plan.addedIncomingRows, 0);
        }

        beginTest ("a plugin deleted elsewhere does not come back");
        {
            // A tray delete while the window is open. Membership belongs to the
            // committed chain, so the staged copy goes even though the user was
            // in the middle of an edit.
            //
            // FAILS IF: the drop branch keeps the row instead of counting it,
            // which would put a plugin the user deleted from the tray back into
            // the list and then back into the graph at the next Apply.
            const std::vector<ChainEntry> baseline { entry ("A"), entry ("B") };
            const std::vector<ChainEntry> staged   { entry ("A"), entry ("B"), entry ("C") };
            const std::vector<ChainEntry> incoming { entry ("A") };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (identitiesOf (materialise (plan, staged, incoming)),
                          juce::String ("A,C"));
            expectEquals (plan.droppedStagedRows, 1);
            expectEquals (plan.addedIncomingRows, 0);
        }

        beginTest ("a plugin added elsewhere appears, at the end");
        {
            // FAILS IF: the trailing loop over unconsumed incoming rows is
            // removed, so a plugin added from anywhere but this panel is
            // invisible here until the window is reopened -- and the next Apply
            // would then delete it, because the panel sends the whole chain.
            const std::vector<ChainEntry> baseline { entry ("A") };
            const std::vector<ChainEntry> staged   { entry ("A"), entry ("C") };
            const std::vector<ChainEntry> incoming { entry ("A"), entry ("B") };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (identitiesOf (materialise (plan, staged, incoming)),
                          juce::String ("A,C,B"),
                          "an arrival must land at the end: the staged order is the "
                          "user's most recent statement about order");
            expectEquals (plan.droppedStagedRows, 0);
            expectEquals (plan.addedIncomingRows, 1);
        }

        beginTest ("a staged bypass and lane win over the committed ones");
        {
            // FAILS IF: a matched row is taken from `incoming` rather than from
            // `staged` -- the identity would still be right and the chain would
            // still be the right length, so only the settings say which side
            // won.
            const std::vector<ChainEntry> baseline { entry ("A", false, 0) };
            const std::vector<ChainEntry> staged   { entry ("A", true,  2) };
            const std::vector<ChainEntry> incoming { entry ("A", false, 1) };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (describeChain (materialise (plan, staged, incoming)),
                          juce::String ("A/byp/L2"));
        }

        beginTest ("the staged order wins");
        {
            // FAILS IF: matched rows are emitted in incoming order rather than
            // by walking `staged`. A reorder the user has not applied yet is an
            // edit like any other.
            const std::vector<ChainEntry> baseline { entry ("A"), entry ("B") };
            const std::vector<ChainEntry> staged   { entry ("B"), entry ("A") };
            const std::vector<ChainEntry> incoming { entry ("A"), entry ("B") };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (identitiesOf (materialise (plan, staged, incoming)),
                          juce::String ("B,A"));
            expectEquals (plan.droppedStagedRows, 0);
            expectEquals (plan.addedIncomingRows, 0);
        }

        beginTest ("a tray bypass on a staged row is not shown until Apply");
        {
            // Pinned deliberately rather than treated as a defect. It is the
            // price of "staged wins for rows it still knows about": the panel
            // cannot both keep the user's bypass and show the tray's.
            //
            // FAILS IF: someone decides a matched row should take the incoming
            // bypass and lane "because only membership is staged". That change
            // is reasonable-sounding and would silently throw away a checkbox
            // click made seconds earlier, so it has to break a named test.
            const std::vector<ChainEntry> baseline { entry ("A", false, 0) };
            const std::vector<ChainEntry> staged   { entry ("A", false, 0), entry ("B") };
            const std::vector<ChainEntry> incoming { entry ("A", true,  0) };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (describeChain (materialise (plan, staged, incoming)),
                          juce::String ("A/on/L0 B/on/L0"));
        }

        beginTest ("duplicate identities are matched positionally");
        {
            // Two rows can share an identity only if one arrived from outside
            // the add menu, which greys an identity already staged -- but the
            // matching has to be ordinal anyway, for the same reason the row
            // menus resolve by identity rather than by index.
            //
            // FAILS IF: claimIncoming stops marking a row consumed. Both staged
            // rows then match incoming[0], neither incoming row is consumed, and
            // the tail loop appends both: the chain comes back as
            // "X/on/L5 X/on/L7 X/on/L0 X/on/L0" with addedIncomingRows at 2.
            // Measured under the mutation. This used to say 1, which is the
            // count for one unclaimed row, not two.
            const std::vector<ChainEntry> baseline { entry ("X", false, 0), entry ("X", false, 0) };
            const std::vector<ChainEntry> staged   { entry ("X", false, 5), entry ("X", false, 7) };
            const std::vector<ChainEntry> incoming { entry ("X", false, 0), entry ("X", false, 0) };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (describeChain (materialise (plan, staged, incoming)),
                          juce::String ("X/on/L5 X/on/L7"));
            expectEquals (plan.addedIncomingRows, 0);
            expectEquals (plan.droppedStagedRows, 0);
        }

        beginTest ("one of two duplicates removed elsewhere drops exactly one");
        {
            // FAILS IF: claimIncoming is first-hit rather than first UNCONSUMED
            // hit. The second staged X would match the single incoming X all
            // over again, nothing would be dropped, and the chain would keep a
            // plugin that is no longer in it.
            const std::vector<ChainEntry> baseline { entry ("X", false, 0), entry ("X", false, 0) };
            const std::vector<ChainEntry> staged   { entry ("X", false, 5), entry ("X", false, 0) };
            const std::vector<ChainEntry> incoming { entry ("X", false, 0) };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (describeChain (materialise (plan, staged, incoming)),
                          juce::String ("X/on/L5"));
            expectEquals (plan.droppedStagedRows, 1);
            expectEquals (plan.addedIncomingRows, 0);
        }

        beginTest ("a second copy the user staged is not read as a deletion");
        {
            // FAILS IF: the baseline is claimed only on the path where the
            // incoming match failed. The first X would then leave the baseline
            // count untouched, the second X would claim it, and the copy the
            // user had just added would be counted as one deleted elsewhere and
            // thrown away.
            const std::vector<ChainEntry> baseline { entry ("X", false, 0) };
            const std::vector<ChainEntry> staged   { entry ("X", false, 0), entry ("X", false, 9) };
            const std::vector<ChainEntry> incoming { entry ("X", false, 0) };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (describeChain (materialise (plan, staged, incoming)),
                          juce::String ("X/on/L0 X/on/L9"));
            expectEquals (plan.droppedStagedRows, 0);
        }

        beginTest ("the empty cases");
        {
            // FAILS IF: a reserve or a loop bound assumes a non-empty vector.
            // Three of these four reach production: a first run with no chain,
            // a user who has just staged the first plugin, and a user who has
            // just deleted the last one.
            expect (reconcileStagedChain ({}, {}, {}).rows.empty());

            {
                const std::vector<ChainEntry> incoming { entry ("A") };
                const auto plan = reconcileStagedChain ({}, {}, incoming);

                expect (allRowsFrom (plan, MergedRow::Source::incoming),
                    "the fast path did not fire, so an unedited refresh went through the merge");
                expectEquals (identitiesOf (materialise (plan, {}, incoming)),
                              juce::String ("A"));
                expectEquals (plan.addedIncomingRows, 0);
            }

            {
                const std::vector<ChainEntry> staged { entry ("A") };
                const auto plan = reconcileStagedChain (staged, {}, {});

                    expectEquals (identitiesOf (materialise (plan, staged, {})),
                              juce::String ("A"),
                              "the first plugin staged into an empty chain was dropped");
                expectEquals (plan.droppedStagedRows, 0);
                expectEquals (plan.addedIncomingRows, 0);
            }

            {
                const std::vector<ChainEntry> baseline { entry ("A") };
                const auto plan = reconcileStagedChain ({}, baseline, {});

                    expect (plan.rows.empty());
                expectEquals (plan.droppedStagedRows, 0,
                              "a row the user deleted and that is also gone upstream "
                              "is not a row the refresh took away");
            }
        }

        beginTest ("every merged index is in range");
        {
            // The materialise helper above returns nothing on a bad index, so
            // the cases that read a chain would fail with a confusing empty
            // result. This says the real thing directly.
            //
            // FAILS IF: an index is emitted against the wrong vector -- a
            // staged index into `incoming` reads past the end whenever the
            // staged list is the longer of the two, which is exactly the case a
            // pending addition produces.
            struct Case
            {
                const char* what;
                std::vector<ChainEntry> staged, baseline, incoming;
            };

            const std::vector<Case> cases
            {
                { "staged longer",  { entry ("A"), entry ("B"), entry ("C") },
                                    { entry ("A") },
                                    { entry ("A") } },
                { "incoming longer", { entry ("A") },
                                     { entry ("A"), entry ("B") },
                                     { entry ("A"), entry ("B"), entry ("C") } },
                { "nothing in common", { entry ("A") }, { entry ("B") }, { entry ("C") } },
                { "clean path",     { entry ("A") }, { entry ("A") },
                                    { entry ("B"), entry ("C") } }
            };

            for (const auto& testCase : cases)
            {
                const auto plan = reconcileStagedChain (testCase.staged, testCase.baseline,
                                                        testCase.incoming);

                for (const auto& row : plan.rows)
                {
                    const auto limit = row.source == MergedRow::Source::staged
                                           ? testCase.staged.size()
                                           : testCase.incoming.size();

                    expect (row.index < limit,
                            juce::String (testCase.what) + ": index "
                                + juce::String ((int) row.index) + " is outside a vector of "
                                + juce::String ((int) limit));
                }
            }
        }

        beginTest ("a plugin the user removed does not come back");
        {
            // The mirror of "a pending addition survives". Rule (2) keeps a
            // staged ADDITION the committed chain has not heard of; the tail
            // loop has to honour a staged DELETION the same way, or the panel
            // resurrects the plugin -- silently, and at the BOTTOM of the list
            // rather than where it used to be, so it does not even look like
            // the row that came back.
            //
            // The baseline is what makes this answerable: B is absent from
            // staged and present in baseline, so the user removed it. An
            // incoming row absent from baseline is an arrival and still gets
            // appended, which the next case pins.
            //
            // FAILS IF: the tail loop appends every unconsumed incoming row.
            //
            // THIS REVERSED A DELIBERATE DECISION, so the argument is here
            // rather than in a commit message. The first implementation kept
            // staged additions and discarded staged deletions, on the grounds
            // that undoing a deletion loses nothing because the plugin is
            // still in the running chain. Two things are wrong with that. The
            // row does not come back where it was, it comes back at the END,
            // so the user's ordering is quietly changed as well; and pressing
            // Apply then commits a chain nobody asked for. BACKLOG.md's own
            // entry specifies "keep staged additions AND removals", so the
            // narrower rule was the deviation, not this.
            const std::vector<ChainEntry> baseline { entry ("A"), entry ("B"), entry ("C") };
            const std::vector<ChainEntry> staged   { entry ("A"), entry ("C") };
            const std::vector<ChainEntry> incoming { entry ("A"), entry ("B"), entry ("C") };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (identitiesOf (materialise (plan, staged, incoming)),
                          juce::String ("A,C"),
                          "a row the user deleted in the panel was put back by a refresh");

            expectEquals (plan.addedIncomingRows, 0,
                          "honouring a deletion was counted as an arrival, so the "
                          "panel would claim the chain changed elsewhere when it did not");
            expectEquals (plan.droppedStagedRows, 0);
        }

        beginTest ("a deletion and an arrival are told apart");
        {
            // Both are unconsumed incoming rows at the tail. Only the baseline
            // separates them: B was known to the staged list and removed from
            // it, D was never in it.
            //
            // FAILS IF: the tail skips on "is it in baseline" without claiming,
            // or claims without checking -- either way one of these two rows
            // gets the other's treatment.
            const std::vector<ChainEntry> baseline { entry ("A"), entry ("B") };
            const std::vector<ChainEntry> staged   { entry ("A") };
            const std::vector<ChainEntry> incoming { entry ("A"), entry ("B"), entry ("D") };

            const auto plan = reconcileStagedChain (staged, baseline, incoming);

            expectEquals (identitiesOf (materialise (plan, staged, incoming)),
                          juce::String ("A,D"),
                          "the deleted row survived, or the genuinely new one was lost");
            expectEquals (plan.addedIncomingRows, 1, "only D arrived");
        }

        beginTest ("the message gate fires exactly when the list moved by itself");
        {
            // PreferencesContentComponent::setChain shows its "chain changed
            // elsewhere" message on droppedStagedRows + addedIncomingRows > 0.
            // Not on "was anything staged at all": that stays true for the whole
            // time an edit is pending, so it would put a message on screen every
            // time any of the nine refresh sites fired.
            //
            // FAILS IF: either counter is incremented on a path that keeps the
            // row. The consequence is not cosmetic -- dirty is true for the
            // whole time an edit is pending, so a gate on it would put a
            // message on screen every time any of the nine refresh sites fired,
            // including a plugin re-declaring its latency mid-drag.
            struct Case
            {
                const char* what;
                std::vector<ChainEntry> staged, baseline, incoming;
                bool shouldSpeak;
            };

            const std::vector<Case> cases
            {
                { "nothing staged, nothing changed",
                  { entry ("A") }, { entry ("A") }, { entry ("A") }, false },
                { "nothing staged, chain replaced upstream",
                  { entry ("A") }, { entry ("A") }, { entry ("B") }, false },
                { "an addition kept intact",
                  { entry ("A"), entry ("B") }, { entry ("A") }, { entry ("A") }, false },
                { "a reorder kept intact",
                  { entry ("B"), entry ("A") }, { entry ("A"), entry ("B") },
                  { entry ("A"), entry ("B") }, false },
                // Dirty by the pending C, so the merge runs at all: with
                // nothing staged the clean path takes the incoming chain
                // verbatim and says nothing, which is the case above.
                { "a staged row deleted upstream while an edit is pending",
                  { entry ("A"), entry ("B"), entry ("C") }, { entry ("A"), entry ("B") },
                  { entry ("A") }, true },
                { "a row arriving from upstream",
                  { entry ("A"), entry ("B") }, { entry ("A") },
                  { entry ("A"), entry ("C") }, true }
            };

            for (const auto& testCase : cases)
            {
                const auto plan = reconcileStagedChain (testCase.staged, testCase.baseline,
                                                        testCase.incoming);

                const bool speaks = plan.droppedStagedRows + plan.addedIncomingRows > 0;

                expect (speaks == testCase.shouldSpeak,
                        juce::String (testCase.what) + ": the panel would "
                            + (speaks ? "speak" : "stay silent") + " and should not");
            }
        }
    }
};

static ChainReconcileTests chainReconcileTests;
