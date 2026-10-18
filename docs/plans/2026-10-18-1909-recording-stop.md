# Stopping a recording finishes its file

Date: 2026-10-18 19:09 EDT

## Context

Stopping a recording, by Alt+Print, the bar's dot or `capture.stop`, ends
with a playable MP4 in the videos dir and the bar's dot gone, whatever was
started while it ran. The recorder stops on the end of its input pipe, and
the pipe's write end reached every process the compositor started during
the recording (a terminal from its shortcut, an app from the launcher):
with those still holding it, ffmpeg never saw the end, never wrote the
file's index, and the compositor waited on it for good. No descriptor of
the compositor's reaches a process it starts.

The contracts are [../intents/apps/capture.md](../intents/apps/capture.md)
for the recording and
[../intents/workspace/build-and-run.md](../intents/workspace/build-and-run.md)
for what a started process gets.

## Tasks

- [ ] The intents state it: a stop always ends in a finished file, and a started process holds only its stdin, stdout and stderr of the compositor's
- [ ] The recorder's pipe is close-on-exec, and every command the compositor starts begins with no descriptor above stderr
- [ ] The capture scenario proves it: a command started during a recording holds no compositor descriptor, and the recording stopped while it runs is a file ffprobe reads; `tools/check.sh` green
- [ ] Whether a full-screen recording on the 2880x1800 panel stalls the compositor in its blocking pipe writes (on screen, the user)
