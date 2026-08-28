# Releasing Light Host

The release pipeline is `.github/workflows/release.yml`. It fires on a pushed tag
matching `v[0-9]*`, builds and tests Windows, Linux and macOS, and creates a
GitHub Release with all three artifacts and a `SHA256SUMS` file.

Two rules make it safe to use:

- A tag with a prerelease suffix (`v5.0.0-rc1`) produces a **draft** marked
  **prerelease**. Nothing becomes visible until someone publishes it, and it never
  takes the Latest badge.
- A clean tag (`v5.0.0`) publishes immediately. Do not push one until a candidate
  built from the same commit has been downloaded and run.

The release notes come from `CHANGELOG.md`. The job looks for a `## [X.Y.Z]`
heading matching the tag with any prerelease suffix stripped, so `v5.0.0-rc1` reads
the `## [5.0.0]` section. If that section does not exist the job **fails** rather
than publishing an empty body, which is the usual outcome of tagging before
renaming the `[Unreleased]` heading.

## Before the first candidate

The pipeline has never run to completion. Expect the first candidate to expose
something, the same way the first CI run did.

1. Confirm `main` is green on all three platforms.
2. Complete the manual checks that no automation covers. They are listed under
   "Not yet verified" in [BACKLOG.md](BACKLOG.md), and the important one is that a
   real plugin has never been instantiated by this code.
3. Bump `VERSION` in `CMakeLists.txt`. It is the only place the version lives;
   everything else derives from it through `JUCE_APPLICATION_VERSION_STRING`.

   > **Delete the build directory after bumping.** JUCE generates the Windows
   > version resource (`LightHost_resources.rc`) through `juceaide` at configure
   > time and does not regenerate it when only the project `VERSION` changes. An
   > incremental build after a bump relinks without complaint and stamps the
   > **old** version into the binary. Confirm before tagging with
   > `(Get-Item "Light Host.exe").VersionInfo.FileVersion`. CI and the release
   > pipeline are unaffected, because no job there caches its build directory.
4. Rename the `## [Unreleased] — towards 5.0.0` heading in `CHANGELOG.md` to
   `## [5.0.0] — YYYY-MM-DD`, and open a fresh empty `[Unreleased]` above it.
5. Commit, push, and wait for CI to pass on that commit.

## Cutting a candidate

```bash
git tag v5.0.0-rc1
git push origin v5.0.0-rc1
```

Then, when the run finishes:

1. Open the draft release and download all three artifacts.
2. Verify the checksums against `SHA256SUMS`.
3. **Windows:** unzip and run `Light Host.exe` on a machine that has never had
   Visual Studio installed. The runtime is statically linked so no Visual C++
   redistributable should be needed, and this is the only way to prove it.
4. **macOS:** the app is **not signed or notarised**. Gatekeeper will refuse it on
   first launch. Right-click and choose Open, or run
   `xattr -dr com.apple.quarantine "Light Host.app"`. If macOS is going to be a
   supported download rather than a build-it-yourself platform, signing is the next
   piece of work.
5. **Linux:** extract and run. The binary needs the same runtime libraries the
   build needed (ALSA, X11, freetype, fontconfig).
6. On each platform, run through the manual checklist: tray icon appears, left
   click opens Preferences, a real plugin loads, bypass and lane changes take
   effect, lane trims move audio, quit and relaunch restores the chain.

## Promoting to a release

Only when a candidate has been run on at least Windows, and ideally all three:

```bash
git tag v5.0.0
git push origin v5.0.0
```

Then delete the candidate's draft release and its tag, since the final artifacts
supersede it:

```bash
gh release delete v5.0.0-rc1 --yes
git push origin :refs/tags/v5.0.0-rc1
git tag -d v5.0.0-rc1
```

## If a candidate is wrong

Fix on `main`, let CI pass, then cut `-rc2`. Do not move a tag that has been
pushed: anyone who fetched it keeps the old commit, and the release artifacts stop
matching the tag they claim to come from.

## What each artifact is, and who it runs for

| Archive | Contains | Runs on |
|---|---|---|
| `LightHost-vX.Y.Z-windows-x64.zip` | `Light Host.exe`, licence files | Windows 10 or later, x64. The runtime is static, so no Visual C++ redistributable |
| `LightHost-vX.Y.Z-macos-universal.zip` | `Light Host.app`, licence files | macOS 11 or later, Apple Silicon and Intel both, from one universal binary |
| `LightHost-vX.Y.Z-linux-x64.tar.gz` | `Light Host`, licence files | See the glibc note below |

The Linux binary is built on `ubuntu-24.04`, so it needs that image's glibc (2.39)
or newer. It will not start on Ubuntu 22.04, Debian 12, or anything else older, and
the failure is an unhelpful loader error rather than a clear message. Anyone on an
older distribution has to build from source, which is a short job because the
dependencies are listed in the README. Building on an older image, or in a
container with an older glibc, is the fix if Linux downloads turn out to matter.

Linux also gets no `.desktop` entry, no icon registration and no tray-support
check. The tray icon needs an appindicator-capable panel, which several desktops no
longer provide by default.

## What the pipeline does not do

- **No code signing on any platform.** Windows will show a SmartScreen warning on
  first run of a new binary, and macOS will refuse it outright until the quarantine
  attribute is cleared. Both are expected for an unsigned open-source build; both
  would be fixed by certificates, which cost money and are worth having only if the
  download counts justify them.
- **No installer.** A single executable in a zip, deliberately.
- **No update check.** Light Host never phones home.
