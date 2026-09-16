# Releasing Light Host

The release pipeline is `.github/workflows/release.yml`. It fires on a pushed tag
matching `v[0-9]*`, builds and tests Windows, Linux and macOS, and creates a
GitHub Release carrying four assets: the three platform archives and a
`SHA256SUMS` file covering them. Each platform runs `ctest` before it packages
anything, so a failing test stops the release rather than shipping into it.

It has run end to end three times: 5.0.0 on 2026-08-27, 5.0.2 on 2026-09-08 and
5.1.0 on 2026-09-16. It has also failed once, and how it failed is the first
section below.

Final tags are pushed directly. There is no release-candidate step, because a
candidate would not change what is verified: every check that matters here is
manual, and all of it can be done on a local Release build before any tag
exists. Candidates remain available for the one case where they do earn their
keep — see "Prerelease tags".

---

## Wait for CI before tagging

**Push `main`, wait for CI to go green on all three platforms, and only then
tag.** This is the single most important line in this file.

v5.0.1 was tagged before CI had finished on the commit. The macOS job then
failed — on a flaky test, not a real defect — the `create-release` job never ran
because it `needs` all three builds, and no release was ever published. The tag
still exists with nothing behind it, which is now permanent: moving or deleting a
published tag breaks anyone who has already fetched it. The flaky test was fixed
in 5.0.2, and the lesson survives the fix, because the next flake will be a
different test.

`.github/workflows/ci.yml` builds and tests the same three platforms on every
push to `main`. It is not the release workflow, but it is the same compilers and
the same test suite, so a green CI run on the exact commit being tagged is the
evidence that the release build will get that far.

**Read the run's conclusion; do not trust `gh run watch --exit-status`.** It
returned exit code 0 for two separate runs that had failed, on 2026-09-16. Ask
for the field instead:

```bash
gh run view <run-id> --json conclusion,jobs --jq '"run: \(.conclusion)", (.jobs[] | "  \(.name): \(.conclusion)")'
```

A gate that reports success on a failed run is worse than no gate, and this is
the one gate standing between you and a repeat of v5.0.1.

Local green is not evidence for the other two platforms. There is no GCC or
Clang on the development machine, so an MSVC-only compile error reaches CI
having passed everything locally — that happened twice during 5.2.0, once on an
ambiguous `operator<<` overload MSVC resolves and GCC and Clang refuse.

---

## Cutting a release

1. **Bump `VERSION` in `CMakeLists.txt`.** It is the only place the version
   lives; everything else derives from it through
   `JUCE_APPLICATION_VERSION_STRING`.

2. **Delete `build/release` before building the bumped version.**

   > JUCE generates the Windows version resource (`LightHost_resources.rc`)
   > through `juceaide` at configure time, and does not regenerate it when only
   > the project `VERSION` changes. An incremental build after a bump relinks
   > without complaint and stamps the **old** version into the binary, so the
   > release you publish reports the previous version to anyone who checks.
   > Verify after building with:
   >
   > ```powershell
   > (Get-Item "Light Host.exe").VersionInfo.FileVersion
   > ```
   >
   > CI and the release pipeline are unaffected, because no job there caches its
   > build directory. This is a local-build problem only, and it is silent.

3. **Rename the `## [Unreleased]` heading in `CHANGELOG.md`** to
   `## [X.Y.Z] — YYYY-MM-DD`, and open a fresh empty `[Unreleased]` above it.

   The release notes are extracted from that section by the pipeline. If a
   `## [X.Y.Z]` heading matching the tag does not exist, the job **fails** rather
   than publishing an empty release body. Forgetting to rename the heading is the
   usual cause.

4. **Run the manual checks** in the next section, on the binary built in step 2.

5. **Commit, push `main`, and wait for CI to pass on that commit.** See above.

6. **Tag and push.**

   ```bash
   git tag v5.2.0
   git push origin v5.2.0
   ```

7. **Confirm the release actually published.** See "After the tag".

---

## The manual checks

None of these are automated, and two of them need a person looking at a display.

- **Run the built binary on a machine that has never had Visual Studio
  installed.** The C runtime is linked statically so no Visual C++
  redistributable should be needed, and this is the only way to prove it rather
  than assume it.
- **Load a real plugin.** The test suite uses stub processors throughout; no
  automated check instantiates a plugin from a DLL.
- **Exercise the chain**: bypass a plugin, move one between lanes, move a lane
  trim, and confirm each takes effect in the audio.
- **Quit from the tray menu and relaunch.** The chain, plugin settings, lanes,
  bypasses, trims and device choices should all come back. Quitting from the tray
  menu is what writes plugin state.
- **Quit the machine, not just the application.** Restart Windows with Light Host
  running and a device open. This is the path the v3.2.0 shutdown crash was on,
  and it is the one path no automated test can reach — with no audio device on a
  CI runner there is no callback thread, so the race cannot occur there.
- **Render the Preferences window at 150% display scaling** and confirm nothing
  clips, overlaps or falls outside the window. The layout is hand-written in
  pixels; every control has to be looked at, not just the panel outline.
- **Check the tray icon against a light taskbar theme as well as a dark one.**
  Windows ships both, the icon is a single bitmap for both, and an icon that is
  legible on dark and invisible on light is a defect that only a person at the
  display will see.
- **Run the render regression.** Nothing in a normal release should change a
  sample, and this is the only check that proves it:

  ```bash
  tools/render-regression.sh check <input.wav>
  ```

  Capture a baseline first if there is not one on this machine. Both the input
  and the baseline hash are deliberately local — the input is a voice recording
  and this repository is public, and the hash depends on which plugins are
  installed — so a fresh clone has to capture its own before the check means
  anything. If a sample did change and that was intended, re-capture in the same
  commit and say why.

---

## After the tag

**A pushed tag is not a release.** Open the Releases page and confirm:

- The release exists, is published rather than draft, and carries the Latest
  badge.
- All **four** assets are attached: `LightHost-vX.Y.Z-windows-x64.zip`,
  `LightHost-vX.Y.Z-macos-universal.zip`, `LightHost-vX.Y.Z-linux-x64.tar.gz`
  and `SHA256SUMS`.
- The release body is the changelog section for this version and is not empty.

Then download at least the Windows archive and check it against `SHA256SUMS`.

If the run failed, fix the cause on `main`, let CI pass, and tag the next patch
version. **Do not move a tag that has been pushed**: anyone who fetched it keeps
the old commit, and the artifacts stop matching the tag they claim to come from.

---

## Prerelease tags

The workflow still supports them, and they are worth knowing about even though
they are not the normal path. A tag carrying a prerelease suffix (`v5.2.0-rc1`)
is classified as a draft marked prerelease: nothing becomes visible until someone
publishes it by hand, and it never takes the Latest badge. The changelog lookup
strips the suffix, so `v5.2.0-rc1` reads the `## [5.2.0]` section.

Use one when the thing being tested is the pipeline itself — a change to
`release.yml`, a new artifact, a new platform — because that is what a local
build cannot check. Delete the draft and both tags afterwards:

```bash
gh release delete v5.2.0-rc1 --yes
git push origin :refs/tags/v5.2.0-rc1
git tag -d v5.2.0-rc1
```

---

## What each artifact is, and who it runs for

| Archive | Contains | Runs on |
|---|---|---|
| `LightHost-vX.Y.Z-windows-x64.zip` | `Light Host.exe`, licence files, README | Windows 10 or later, x64. The runtime is static, so no Visual C++ redistributable |
| `LightHost-vX.Y.Z-macos-universal.zip` | `Light Host.app`, licence files, README | macOS 11 or later, Apple Silicon and Intel both, from one universal binary |
| `LightHost-vX.Y.Z-linux-x64.tar.gz` | `Light Host`, licence files, README | See the glibc note below |

The application is conveyed under AGPLv3, which is why the licence files travel
inside each archive rather than being left behind in the repository.

The Linux binary is built on `ubuntu-24.04`, so it needs that image's glibc (2.39)
or newer. It will not start on Ubuntu 22.04, Debian 12, or anything else older, and
the failure is an unhelpful loader error rather than a clear message. Anyone on an
older distribution has to build from source, which is a short job because the
dependencies are listed in the README. Building on an older image, or in a
container with an older glibc, is the fix if Linux downloads turn out to matter.

Linux also gets no `.desktop` entry, no icon registration and no tray-support
check. The tray icon needs an appindicator-capable panel, which several desktops no
longer provide by default.

macOS is unsigned and unnotarised, so Gatekeeper refuses it on first launch.
Right-click and choose Open, or run
`xattr -dr com.apple.quarantine "Light Host.app"`. If macOS is ever to be a
supported download rather than a build-it-yourself platform, signing is the piece
of work that makes it one.

---

## What the pipeline does not do

- **No code signing on any platform.** Windows shows a SmartScreen warning on
  first run of a new binary, and macOS refuses it outright until the quarantine
  attribute is cleared. Both are expected for an unsigned build. Both would be
  fixed by a certificate, which is a recurring paid subscription tied to a
  verified legal identity, and which has been declined — see the README.
- **No installer.** A single executable in an archive, deliberately.
- **No update check.** Light Host never phones home.
- **No version consistency check.** Nothing compares the tag against `VERSION` in
  `CMakeLists.txt` or against the version stamped into the Windows binary. Steps
  1 and 2 above are the only thing standing between a bumped tree and a release
  whose executable reports the wrong version.
