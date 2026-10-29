# Contributing to Xerenge

Thanks for helping. This is a small project run in free time, so a report that
comes with everything needed gets looked at first. One that says only "it doesn't
work" has to wait for a round of questions before anyone can start, and is looked
at **many times later** than a complete one - or closed if the details never come.

**A report without a log is closed as invalid, without further explanation.**
The log is the one thing every report needs: without it nothing can be
checked, whatever the problem is. If the installer is what failed, attach its
`install.log` (the error names where it is) instead.

## Before you open an issue

- Update to the **latest build artifact** and check that the problem is still there. Open
  [Actions](https://github.com/shipa-2/Xerenge/actions/workflows/build.yml?query=branch%3Amain+is%3Asuccess),
  take the newest successful run on `main` and download the installer for your system from its
  Artifacts (you need to be signed in to GitHub; artifacts are kept for 7 days). It is newer than
  the [latest release](https://github.com/shipa-2/Xerenge/releases/latest): many problems are
  already fixed there.
- Search the [open and closed issues](https://github.com/shipa-2/Xerenge/issues?q=is%3Aissue):
  it may already be known, with a workaround.
- One problem per issue.

## What a bug report must have

1. **The build:** the artifact you installed - the run's number or commit from the Actions page -
   and the release it is based on (the `VERSION` at that commit, e.g. 0.4.7); or the release, if
   you could not use an artifact. And the disc: European or American.
2. **The system and the hardware:** OS and its version; CPU; GPU and its driver
   version. On Android: the phone model, the chip (e.g. Snapdragon 8 Gen 2), the
   Android version and whether a Turnip driver is used.
3. **The settings:** what you changed in the installer (renderer, resolution,
   hacks, server address). Attaching `xerenge.conf` from the install folder is
   the easiest way.
4. **The log - required.** The log of a run where the problem happened, attached
   to the issue as a file (not pasted into the text, not a link to a file host;
   zip it if GitHub refuses it as it is):
   - Windows: `%LOCALAPPDATA%\xerenge-burnout\burnout.log`
   - Linux: `~/.local/state/xerenge-burnout/burnout.log`
   - macOS: `~/Library/Logs/xerenge-burnout/burnout.log`
   - Android: set a log folder in the installer ("Log folder"); the logs are
     copied there.

   The log is overwritten on the next start, so copy it right after the run.
   Without the log the issue is closed as invalid.
5. **Steps to reproduce:** what you did, what you expected and what happened
   instead. Where in the game (event, track, menu), and whether it happens every
   time.
6. **A screenshot or a short video** for anything visual (missing effects, wrong
   colours, black screens).

For performance problems (low FPS, stutter) also say the resolution, what FPS
you get and where, and whether it gets worse the longer you play.

## Pull requests

- Keep a pull request to one change, and say in its description what it fixes and
  how you checked it (which system, which part of the game).
- Match the code around it: naming, comments and formatting.
- No game files, extracted assets or generated code in commits.
- By sending a pull request you agree that your contribution is released under
  the project's licence, the GNU GPL version 3 or later ([LICENSE](LICENSE)).
