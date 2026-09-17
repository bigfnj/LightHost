#include "../Source/OfflineRender.hpp"

#include <juce_core/juce_core.h>

//==============================================================================
// The command line that decides what --render renders.
//
// WHY THIS FILE EXISTS
//
// Source/OfflineRender.hpp is 630 lines and had no test of any kind, which
// would matter less if it were an ordinary feature. It is not: it is this
// project's own audio-regression gate. tools/render-regression.sh proves a
// change did not alter the sound by pushing a file through the chain and
// comparing the output, and every one of those runs is configured entirely by
// the five functions below. A mis-parse does not fail -- it renders a DIFFERENT
// chain, with different parameters, and reports success. The gate then passes,
// having measured something nobody asked for.
//
// renderFile itself needs real plugins and a real file on disk, so it stays out
// of scope here. The parsers do not, and they are where the silent wrong answer
// comes from.
//
// isPaced is the sharpest of the five, and OfflineRender.hpp:601-612 says why in
// its own words: an unpaced render starved a worker-thread plugin, the plugin
// fell back to passing the dry signal through, the render reported success, and
// the conclusion drawn from it -- that the plugin did nothing -- was confidently
// wrong about a plugin that removes 25 dB of keyboard noise. That cost an
// afternoon. It turns on one boolean, and nothing was watching it.
//==============================================================================
namespace
{
    /** A double-quote, built rather than escaped, so this source file carries no
        escape sequence of its own to get wrong.
    */
    const juce::String quote = juce::String::charToString ('"');

    /** A command line with one value that has to be quoted. Built by hand
        because a braced juce::StringArray of mixed literals and juce::Strings
        picks a different constructor depending on the compiler.
    */
    juce::StringArray flagWithValue (const char* flag, const juce::String& value)
    {
        juce::StringArray params;
        params.add (flag);
        params.add (value);
        return params;
    }
}

//==============================================================================
class OfflineRenderArgumentTests final : public juce::UnitTest
{
public:
    OfflineRenderArgumentTests()
        : juce::UnitTest ("Offline render command line", "OfflineRender") {}

    void runTest() override
    {
        using namespace lighthost::render;

        //======================================================================
        beginTest ("--render is recognised in both spellings and nothing else is");
        {
            expect (isRequested ({ "--render", "in.wav", "out.wav" }));
            expect (isRequested ({ "-render", "in.wav", "out.wav" }));
            expect (isRequested ({ "-multi-instance=rig", "--render", "in.wav", "out.wav" }),
                    "the flag does not have to come first");

            expect (! isRequested ({}));
            expect (! isRequested ({ "-self-test" }));
            expect (! isRequested ({ "render" }), "a bare word is not the flag");

            // Pinned because it is the mistake a person makes, and because the
            // consequence is the application starting normally -- tray icon,
            // audio device, the lot -- when a render was asked for. contains()
            // is an exact match, so there is no partial spelling that works.
            expect (! isRequested ({ "--render=in.wav" }),
                    "an =-joined value is not recognised, and the app would launch instead");
        }

        //======================================================================
        beginTest ("the two paths after --render are the input and the output");
        {
            juce::String in, out;
            parseArguments ({ "--render", "a.wav", "b.wav" }, in, out);

            expectEquals (in,  juce::String ("a.wav"));
            expectEquals (out, juce::String ("b.wav"));
        }

        beginTest ("the paths are taken from the first --render, and the scan stops");
        {
            // It returns on the first match, so a second --render is not a
            // second render: its paths are ignored entirely.
            juce::String in, out;
            parseArguments ({ "--render", "a.wav", "b.wav",
                              "-render", "c.wav", "d.wav" }, in, out);

            expectEquals (in,  juce::String ("a.wav"));
            expectEquals (out, juce::String ("b.wav"));
        }

        beginTest ("a path the command line does not carry is left alone, not cleared");
        {
            // Sentinels, because the interesting case is what happens to a
            // value the caller already had. The function used to leave it
            // alone while its doc comment promised an empty string; both
            // shipped callers pass empties so it could not bite, and the day
            // it did -- a caller reusing one variable across two renders --
            // the symptom would have been rendering the previous file again
            // and reporting success.
            juce::String in ("previous-in"), out ("previous-out");
            parseArguments ({ "--render", "a.wav" }, in, out);

            expectEquals (in,  juce::String ("a.wav"));
            expectEquals (out, juce::String(),
                          "an absent path must come back empty, not as the caller's old value");
        }

        beginTest ("--render with nothing after it sets neither path");
        {
            juce::String in, out;
            parseArguments ({ "-self-test", "--render" }, in, out);

            expect (in.isEmpty());
            expect (out.isEmpty());
        }

        beginTest ("quotes around a path are removed");
        {
            // A path with a space in it arrives quoted from every shell, and
            // juce::File would look for a directory literally named with the
            // opening quote.
            juce::StringArray params;
            params.add ("--render");
            params.add (quote + "C:/My Audio/take 1.wav" + quote);
            params.add (quote + "out.wav" + quote);

            juce::String in, out;
            parseArguments (params, in, out);

            expectEquals (in,  juce::String ("C:/My Audio/take 1.wav"));
            expectEquals (out, juce::String ("out.wav"));
        }

        //======================================================================
        beginTest ("a --chain value is split on commas, in the order given");
        {
            // Order is the chain. Getting it back in a different order renders
            // a compressor before a gate instead of after it, which sounds
            // different and reports the same success.
            const auto chain = parseChain ({ "--chain", "Denoise,Gate,Limiter" });

            expectEquals (chain.size(), 3);
            expectEquals (chain[0], juce::String ("Denoise"));
            expectEquals (chain[1], juce::String ("Gate"));
            expectEquals (chain[2], juce::String ("Limiter"));
        }

        beginTest ("--chain is repeatable, and the repeats append in order");
        {
            const auto chain = parseChain ({ "--chain", "A,B", "-chain", "C" });

            expectEquals (chain.size(), 3);
            expectEquals (chain.joinIntoString ("|"), juce::String ("A|B|C"));
        }

        beginTest ("whitespace around a name is dropped, and empty entries with it");
        {
            // "A, B ,, C," is the list written the way it reads. A stray comma
            // must not become a plugin nobody named: renderFile looks every
            // entry up and aborts the whole render with "no scanned plugin
            // matches" when one is not found, so an empty entry fails a render
            // that was correctly specified.
            const auto chain = parseChain ({ "--chain", "A, B ,, C," });

            expectEquals (chain.size(), 3);
            expectEquals (chain.joinIntoString ("|"), juce::String ("A|B|C"));
        }

        beginTest ("a quoted --chain value loses its quotes and keeps its commas");
        {
            const auto chain = parseChain (
                flagWithValue ("--chain", quote + "Noise Gate, De-esser" + quote));

            expectEquals (chain.size(), 2);
            expectEquals (chain[0], juce::String ("Noise Gate"));
            expectEquals (chain[1], juce::String ("De-esser"));
        }

        beginTest ("--chain with no value after it contributes nothing");
        {
            expectEquals (parseChain ({ "--render", "a.wav", "b.wav", "--chain" }).size(), 0);
            expectEquals (parseChain ({ "--chain" }).size(), 0);
        }

        beginTest ("no --chain at all means the saved chain, not an empty one");
        {
            // renderFile branches on chainOverride.isEmpty(), and the two
            // branches read different settings keys. An accidental one-entry
            // result here would render a chain the user never configured.
            expect (parseChain ({ "--render", "a.wav", "b.wav" }).isEmpty());
            expect (parseChain ({}).isEmpty());
        }

        //======================================================================
        beginTest ("pacing is on unless --fast is given");
        {
            expect (isPaced ({}));
            expect (isPaced ({ "--render", "a.wav", "b.wav" }));
            expect (isPaced ({ "--render", "a.wav", "b.wav", "--chain", "Denoise" }));

            expect (! isPaced ({ "--fast" }));
            expect (! isPaced ({ "-fast" }));
            expect (! isPaced ({ "--render", "a.wav", "b.wav", "--fast" }));
        }

        beginTest ("nothing else on the line can turn pacing off by accident");
        {
            // The failure mode is silent and expensive, so the match has to be
            // exact in both directions: a value that merely looks like the flag
            // must not disable pacing.
            expect (isPaced ({ "--chain", "fast" }), "a plugin named fast is not the flag");
            expect (isPaced ({ "--fastest" }),       "a longer flag is not --fast");
            expect (isPaced ({ "--fast=1" }),        "an =-joined value is not the flag");
            expect (isPaced ({ "fast" }),            "a bare word is not the flag");
            expect (isPaced ({ "--param", "Fast=1" }));
        }

        //======================================================================
        beginTest ("every --param is collected, in the order given");
        {
            // Order matters here too: two overrides of one parameter resolve to
            // the last, and renderFile applies them in this order.
            const auto overrides = parseOverrides ({ "--param", "Threshold=-18",
                                                     "-param",  "Mix@0.25",
                                                     "--param", "Threshold=-24" });

            expectEquals (overrides.size(), 3);
            expectEquals (overrides[0], juce::String ("Threshold=-18"));
            expectEquals (overrides[1], juce::String ("Mix@0.25"));
            expectEquals (overrides[2], juce::String ("Threshold=-24"));
        }

        beginTest ("a --param value is unquoted and otherwise handed over whole");
        {
            // renderFile splits it itself, at the first "=" or else at "@", and
            // a value can legitimately contain either -- "Note=A@440" is the
            // case its comment names. Splitting here as well would take the
            // decision away from the one place that knows the rule.
            const auto overrides = parseOverrides (
                flagWithValue ("--param", quote + "Note=A@440" + quote));

            expectEquals (overrides.size(), 1);
            expectEquals (overrides[0], juce::String ("Note=A@440"));
        }

        beginTest ("--param with no value after it contributes nothing");
        {
            expectEquals (parseOverrides ({ "--param" }).size(), 0);
            expectEquals (parseOverrides ({ "--render", "a.wav", "b.wav", "-param" }).size(), 0);
        }

        beginTest ("an empty --param survives, unlike an empty --chain entry");
        {
            // The two disagree on purpose. An empty chain entry is looked up as
            // a plugin name and aborts the render, so parseChain drops it. An
            // empty --param reaches renderFile, which logs "ignoring malformed
            // --param" and carries on with everything else. Pinned because the
            // asymmetry reads as an oversight and is not: tidying parseOverrides
            // to match would turn a logged warning into silence.
            expectEquals (parseOverrides ({ "--param", "" }).size(), 1);
            expectEquals (parseChain ({ "--chain", "" }).size(), 0);
        }

        //======================================================================
        beginTest ("a full command line parses into all five answers at once");
        {
            // The five are read from one juce::StringArray, so the real risk is
            // one of them consuming or being confused by another's value. A
            // path called "--fast" is not going to happen; a chain entry
            // called "Fast Attack" is.
            const juce::StringArray params { "--render", "in.wav", "out.wav",
                                             "--chain", "Fast Attack,Limiter",
                                             "--param", "Threshold=-18",
                                             "--fast" };

            juce::String in, out;
            parseArguments (params, in, out);

            expect (isRequested (params));
            expectEquals (in,  juce::String ("in.wav"));
            expectEquals (out, juce::String ("out.wav"));
            expectEquals (parseChain (params).joinIntoString ("|"),
                          juce::String ("Fast Attack|Limiter"));
            expectEquals (parseOverrides (params).joinIntoString ("|"),
                          juce::String ("Threshold=-18"));
            expect (! isPaced (params));
        }
    }
};

static OfflineRenderArgumentTests offlineRenderArgumentTests;
