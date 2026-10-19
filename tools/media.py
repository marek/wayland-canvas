#!/usr/bin/env python3
"""Remake the README's pictures in docs/media/ and its walkthrough video.

The tour's public-domain assets (tools/media/assets.tsv) are fetched once
into scratch and checked by hash; the web pages it shows (tools/media/
pages.tsv) are captured once as pictures of the rendered page and replayed
from scratch on every later run, so the pictures keep the day of capture.
build/media-tour then runs the compositor on the headless backend with
Chrome, Krita, foot, Neovim, imv and mpv in scratch profiles, and what it
captured is encoded: the stills as WebP into docs/media/, the walkthrough
as an MP4 with its captions burnt in into tmp/media/walkthrough.mp4, for
uploading as a GitHub attachment the README links.

--stills skips the walkthrough; --keep keeps the run's scratch folder (the
apps' profiles, the raw captures) for a look; --record records every page
again. Scratch lives in tmp/media/ ($CANVAS_TMP overrides tmp/).
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MEDIA = ROOT / "docs/media"
STILLS = ("hero", "tiling", "writing", "table")
PROGRAMS = ("ffmpeg", "curl", "google-chrome-stable", "krita", "imv", "mpv",
            "foot", "nvim", "qs", "dbus-daemon", "openssl")
# the most a video attachment on GitHub may weigh on a free plan is 10 MB
MP4_BUDGET = 8 * 1024 * 1024

sys.path.insert(0, str(ROOT / "tools/media"))
import pages  # noqa: E402


def run(cmd, **kwargs):
    subprocess.run(cmd, check=True, **kwargs)


def fail(message):
    print(f"tools/media.py: {message}", file=sys.stderr)
    sys.exit(1)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def human(path):
    size = float(path.stat().st_size)
    for unit in ("B", "kB", "MB"):
        if size < 1000 or unit == "MB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1000


def fetch_assets(assets):
    """Every row of assets.tsv, fetched once and checked by its hash."""
    for row in (ROOT / "tools/media/assets.tsv").read_text().splitlines():
        if not row or row.startswith("#"):
            continue
        name, digest, url = row.split("\t")
        target = assets / name
        if target.exists() and sha256(target) == digest:
            continue
        print(f"media.py: fetching {name}")
        part = assets / f"{name}.part"
        run(["curl", "-fsS", "--retry", "5", "--retry-delay", "5",
             "-A", "wayland-canvas tools/media.py", "-o", str(part), url])
        if sha256(part) != digest:
            fail(f"{name} does not match its hash in tools/media/assets.tsv")
        part.replace(target)
        time.sleep(1)  # Wikimedia limits the rate of requests


def make_album(assets):
    """The music player's album: the recording, the painting as its cover."""
    if (assets / "gymnopedie.mp3").exists():
        return
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
         "-i", str(assets / "gymnopedie.ogg"),
         "-i", str(assets / "fisherman.jpg"),
         "-map", "0:a", "-map", "1:v", "-c:a", "libmp3lame", "-q:a", "4",
         "-vf", "crop='min(iw,ih)':'min(iw,ih)',scale=800:800",
         "-c:v", "mjpeg", "-disposition:v", "attached_pic",
         "-id3v2_version", "3", "-metadata", "title=Gymnopédie No. 1",
         "-metadata", "artist=Erik Satie",
         "-metadata", "album=Trois Gymnopédies",
         str(assets / "gymnopedie.mp3")])


def record_pages(folder, profile, rerecord):
    """Every page of pages.tsv captured once into scratch; index.tsv maps
    each name to the URL its capture ended at (after whatever redirected
    the visit), which is what the tour's Chrome asks the replay server for.
    Returns the captures' base paths."""
    folder.mkdir(parents=True, exist_ok=True)
    profile.mkdir(parents=True, exist_ok=True)
    index, bases = [], []
    for row in (ROOT / "tools/media/pages.tsv").read_text().splitlines():
        if not row or row.startswith("#"):
            continue
        # maxsplit keeps the selectors, with their spaces and commas, whole
        name, width, url, remove = row.split("\t", 3)
        base = str(folder / name)
        if rerecord or not Path(base + ".json").exists():
            print(f"media.py: capturing {name}")
            pages.record(url, int(width), base, str(profile), remove=remove)
        index.append(f"{name}\t{pages.Snapshot(base).url}\n")
        bases.append(base)
    (folder / "index.tsv").write_text("".join(index))
    return bases


def bar_standins():
    """The bar's stand-in services, as the bar scenario has them; without
    them the bar shows only the widgets that need no service."""
    proc = subprocess.run(["tests/scenarios/ensure-bar-standins.sh"],
                          cwd=ROOT, capture_output=True)
    if proc.returncode != 0:
        return {}
    lines = proc.stdout.decode().rstrip("\n").splitlines()
    if not lines or not lines[-1]:
        return {}
    return {"CANVAS_BAR_STANDIN_PY": lines[-1],
            "CANVAS_BAR_STANDINS": str(ROOT / "tests/scenarios/bar-standins.py")}


def run_tour(run_dir, log_path, env):
    print(f"media.py: running the tour (log: {log_path})")
    with open(log_path, "w") as log:
        proc = subprocess.run([str(ROOT / "build/media-tour")], cwd=ROOT,
                              env=env, stdout=log, stderr=log)
    if proc.returncode == 0:
        return
    for line in log_path.read_text().splitlines()[-20:]:
        print(line, file=sys.stderr)
    print(f"tools/media.py: the tour failed; its folder is kept in {run_dir}",
          file=sys.stderr)
    sys.exit(1)


def ass_stamp(ms):
    return (f"{ms // 3600000}:{ms // 60000 % 60:02d}:"
            f"{ms // 1000 % 60:02d}.{ms // 10 % 100:02d}")


def write_captions(run_out):
    """The captions the tour timed, as an ASS script: a translucent box at
    the foot of the frame, the text centred 120 px up, 250 ms fades."""
    head = [
        "[Script Info]", "ScriptType: v4.00+", "PlayResX: 1600",
        "PlayResY: 1000", "ScaledBorderAndShadow: yes", "",
        "[V4+ Styles]",
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
        "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding",
        "Style: Caption,Noto Sans,34,&H00FFFFFF,&H00FFFFFF,&H30141418,"
        "&H00000000,0,0,0,0,100,100,0,0,3,16,0,2,80,80,120,1", "",
        "[Events]",
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
        "Effect, Text",
    ]
    dialogues = []
    for row in (run_out / "captions.tsv").read_text().splitlines():
        if not row:
            continue
        start, end, text = row.split("\t", 2)
        dialogues.append(f"Dialogue: 0,{ass_stamp(int(start))},"
                         f"{ass_stamp(int(end))},Caption,,0,0,0,,"
                         f"{{\\fad(250,250)}}{text}")
    (run_out / "captions.ass").write_text("\n".join(head + dialogues) + "\n",
                                         encoding="utf-8")


def encode(run_out, out_dir, video, stills_only):
    """The tour's captures in run_out encoded: the four stills as 1920-px
    WebP into out_dir, and unless stills-only the walkthrough with its
    captions burnt in as the MP4 `video`."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in STILLS:
        target = out_dir / f"{name}.webp"
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-i", str(run_out / f"{name}.png"),
             "-vf", "scale=1920:-1:flags=lanczos", "-c:v", "libwebp",
             "-quality", "88", str(target)])
        print(f"media.py: {target} ({human(target)})")
    if stills_only:
        return
    write_captions(run_out)
    # the crf is raised until the video fits the attachment budget
    for crf in (24, 26, 28):
        run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-i", "walkthrough-raw.mp4", "-vf", "ass=captions.ass",
             "-c:v", "libx264", "-preset", "slow", "-crf", str(crf),
             "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-an",
             str(video)], cwd=run_out)
        if video.stat().st_size <= MP4_BUDGET:
            break
    print(f"media.py: {video} ({human(video)}): upload it as a GitHub attachment"
          " and put its link under Walkthrough in README.md")


def main():
    parser = argparse.ArgumentParser(
        description="Remake the README's pictures in docs/media/ and its "
                    "walkthrough in tmp/media/walkthrough.mp4 ($CANVAS_TMP "
                    "overrides tmp/).")
    parser.add_argument("--stills", action="store_true",
                        help="remake only the four stills, not the walkthrough")
    parser.add_argument("--keep", action="store_true",
                        help="keep the run's scratch folder (the apps' "
                             "profiles, the raw captures)")
    parser.add_argument("--record", action="store_true",
                        help="record every web page again, not only the "
                             "missing ones")
    args = parser.parse_args()

    os.chdir(ROOT)
    missing = [name for name in PROGRAMS if shutil.which(name) is None]
    for name in missing:
        print(f"tools/media.py: {name} is not installed; the tour needs it",
              file=sys.stderr)
    if missing:
        sys.exit(1)

    scratch = Path(os.environ.get("CANVAS_TMP") or ROOT / "tmp") / "media"
    scratch.mkdir(parents=True, exist_ok=True)
    (scratch / "replay").mkdir(exist_ok=True)
    assets, pages_dir = scratch / "assets", scratch / "pages"
    run_dir, log_path = scratch / "run", scratch / "tour.log"

    assets.mkdir(exist_ok=True)
    fetch_assets(assets)
    make_album(assets)
    snapshots = record_pages(pages_dir, scratch / "chrome-record", args.record)

    run(["tools/build.sh"])
    env = {**os.environ, **bar_standins(),
           "MEDIA_WORK": str(run_dir),
           "MEDIA_ASSETS": str(assets),
           "MEDIA_SOURCE": str(ROOT),
           "MEDIA_ONLY": "stills" if args.stills else "",
           "MEDIA_PAGES": str(pages_dir / "index.tsv"),
           "MEDIA_ICON_THEME": ("Yaru-blue"
                                if Path("/usr/share/icons/Yaru-blue").is_dir()
                                else "Adwaita")}

    server = pages.ReplayServer(snapshots, str(scratch / "replay"))
    try:
        env["MEDIA_CHROME_FLAGS"] = server.chrome_flags()
        run_tour(run_dir, log_path, env)
    finally:
        server.close()

    encode(run_dir / "out", MEDIA, scratch / "walkthrough.mp4", args.stills)
    if not args.keep:
        shutil.rmtree(run_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
