"""stage_release.py: the release folder (what the release zip holds), from the play build and the repo, with its
contents list and a check that it holds no game files, translated shaders or Microsoft files other than the two
DXC redistributables (docs/RELEASE_FIRST_RUN.md).

  python -I tools\\release\\stage_release.py OUT_DIR [--build out\\build\\win-amd64-release-play] [--no-zip]

OUT_DIR is created (it must not exist, or must be a folder this script made: RELEASE_CONTENTS.txt in it). The game
files go in later by the player: OUT_DIR\\game\\ (a disc image or the extracted disc, and Title Update 3).
Then, unless --no-zip: PDZRestored-<version>.zip beside OUT_DIR with the files this script placed (in a folder
PDZRestored-<version>); the version is src/pdz_version.h's PDZ_RESTORED_VERSION, which the exe must hold.
"""
import argparse, hashlib, os, re, shutil, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

FROM_BUILD = ["PDZRestored.exe", "rexruntime.dll", "rexgpu-xenos.dll", "dxcompiler.dll", "dxil.dll", "gamecontrollerdb.txt"]
FROM_GIT = {  # committed versions (a developer's local edits never go into a release)
    # The player's settings ship as defaults (0.9.2): the exe copies settings\defaults\<file> to settings\<file> only
    # when that is missing (utils.h EnsureSettingsFiles), so unzipping a new version over an old folder keeps the
    # player's own settings and key bindings. settings\hardware.toml / mapping.toml themselves are never in a release.
    "settings/defaults/hardware.toml": "settings/release/hardware.toml",  # the release template, not the developer's own
    "settings/defaults/mapping.toml": "settings/mapping.toml",
    "settings/gamecontrollerdb.txt": "settings/gamecontrollerdb.txt",
    "README.md": "tools/release/README.md",  # the players' README (the repo's README.md is for developers)
    "THIRD_PARTY_NOTICES.md": "THIRD_PARTY_NOTICES.md",
    "LICENSE": "LICENSE",
}  # and every committed file under licenses/ (THIRD_PARTY_NOTICES.md lists them), added in main()
LICENCE_DIR = "licenses"
WRITTEN = {
    "PDZRestored (Safe mode).bat": "@echo off\r\nrem Safe mode: the emulated renderer, locked at 60 fps (no native renderer).\r\n"
                                   "start \"\" /D \"%~dp0\" \"%~dp0PDZRestored.exe\" --pdz_safe_mode=true\r\n",
    "PDZ Restored Settings.bat": "@echo off\r\nrem The settings window, then the game (the same as holding Shift while starting).\r\n"
                                 "start \"\" /D \"%~dp0\" \"%~dp0PDZRestored.exe\" --pdz_settings_open=true\r\n",
    "game/README.txt": "Put your own Perfect Dark Zero files here before the first start:\r\n"
                       "- the disc: a disc image (.iso) of any retail Perfect Dark Zero disc, or its extracted files;\r\n"
                       "- Title Update 3 (any disc's), copied from your own Xbox 360: its file name usually starts\r\n"
                       "  with TU_16L61UJ_. Its extracted files (default.xexp and Pdz\\...) work too.\r\n"
                       "PDZ Restored checks them, prepares them once (a few minutes at most) and makes the renderer's\r\n"
                       "shaders from them. You can delete the disc image afterwards; the prepared files are in data\\.\r\n",
}
# Never in a release: game files, translated shaders and their caches, Microsoft runtime DLLs.
BANNED_EXT = {".xex", ".xexp", ".pkg", ".iso", ".xiso", ".dxil", ".hlsl", ".xsh", ".str", ".bin", ".lib", ".pdb", ".map",
              ".mp3", ".wav", ".flac", ".ogg", ".m4a", ".wma", ".aac", ".xwm"}  # music / audio files: never shipped
# The release's only launchers besides the exe: Safe mode and the settings window (the Sabotage / Plant the Flag /
# Protection .bat files in C:\Games are test tools).
LAUNCHERS = {"pdz restored settings.bat", "pdzrestored (safe mode).bat"}
BANNED_NAMES = ("msvcp", "vcruntime", "concrt", "ucrtbase", "api-ms-win", "d3dcompiler", "xinput")
MICROSOFT_OK = {"dxcompiler.dll", "dxil.dll"}
# The player's own files (made from settings\defaults\ at the first start): a release that held them would replace a
# player's settings and key bindings when unzipped over an older version.
PLAYER_FILES = {os.path.join("settings", "hardware.toml"), os.path.join("settings", "mapping.toml")}
# Built into PDZRestored.exe (cmake/pdz_xenosrecomp.cmake), not separate files: the committed versions must be in it, and
# hold only comment lines and hex / decimal fields (hashes, formats, pipeline state: no code of any kind).
EMBEDDED = ["data/native_bind_list.txt", "data/native_pipeline_list.txt"]
EMBEDDED_CHARS = set(b"0123456789abcdef :;|-")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--build", default=os.path.join(REPO, "out", "build", "win-amd64-release-play"))
    ap.add_argument("--no-zip", action="store_true", help="the folder only")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if (os.path.exists(out) and not os.path.exists(os.path.join(out, "RELEASE_CONTENTS.txt"))
            and set(os.listdir(out)) - {"game"}):
        sys.exit("%s exists and wasn't made by this script: not touched" % out)
    os.makedirs(out, exist_ok=True)
    header = subprocess.run(["git", "-C", REPO, "show", "HEAD:src/pdz_version.h"], capture_output=True, check=True,
                            text=True).stdout
    m = re.search(r'#define\s+PDZ_RESTORED_VERSION\s+"([^"]+)"', header)
    if not m:
        sys.exit("no PDZ_RESTORED_VERSION in the committed src/pdz_version.h")
    version = m.group(1)
    for name in FROM_BUILD:
        src = os.path.join(a.build, name)
        if not os.path.exists(src):
            sys.exit("missing in the build: " + src)
        shutil.copy2(src, os.path.join(out, name))
    licences = subprocess.run(["git", "-C", REPO, "ls-tree", "-r", "--name-only", "HEAD", LICENCE_DIR + "/"],
                              capture_output=True, check=True, text=True).stdout.split()
    if not licences:
        sys.exit("no committed files under %s/" % LICENCE_DIR)
    for dst, rel in list(FROM_GIT.items()) + [(rel, rel) for rel in licences]:
        data = subprocess.run(["git", "-C", REPO, "show", "HEAD:" + rel], capture_output=True, check=True).stdout
        os.makedirs(os.path.dirname(os.path.join(out, dst)) or out, exist_ok=True)
        open(os.path.join(out, dst), "wb").write(data)
    for dst, text in WRITTEN.items():
        os.makedirs(os.path.dirname(os.path.join(out, dst)) or out, exist_ok=True)
        open(os.path.join(out, dst), "w", encoding="ascii", newline="").write(text)

    # The contents list, and the check. Only files this script placed are listed (game\ and data\ are the player's).
    listed, problems = [], []
    exe = open(os.path.join(out, "PDZRestored.exe"), "rb").read()
    if exe.find(version.encode("ascii")) < 0:
        problems.append("PDZRestored.exe doesn't hold the version %s (rebuild the exe)" % version)
    embedded = []
    for rel in EMBEDDED:
        data = subprocess.run(["git", "-C", REPO, "show", "HEAD:" + rel], capture_output=True).stdout
        if not data:
            problems.append("not committed: " + rel)
            continue
        lines = data.split(b"\n")
        bad = [i + 1 for i, line in enumerate(lines)
               if line and not line.startswith(b"#") and not set(line) <= EMBEDDED_CHARS]
        if bad:
            problems.append("%s: lines %s hold more than hashes and numbers" % (rel, bad[:5]))
        if exe.find(data) < 0:
            problems.append("PDZRestored.exe doesn't hold the committed %s (rebuild the exe)" % rel)
        rows = sum(1 for line in lines if line and not line.startswith(b"#"))
        embedded.append((rel, rows, len(data), hashlib.sha256(data).hexdigest()))
    for root, dirs, files in os.walk(out):
        rel_root = os.path.relpath(root, out)
        if rel_root.split(os.sep)[0] in ("data", "logs", "userdata", "native_shaders", "native_cache"):
            dirs[:] = []
            continue
        for fn in sorted(files):
            rel = os.path.normpath(os.path.join(rel_root, fn))
            if rel == "RELEASE_CONTENTS.txt" or (rel.startswith("game" + os.sep) and fn != "README.txt"):
                continue
            low = fn.lower()
            if os.path.splitext(low)[1] in BANNED_EXT:
                problems.append("game file / shader / debug file: " + rel)
            if low.startswith(BANNED_NAMES):
                problems.append("Microsoft runtime file: " + rel)
            if low.endswith((".bat", ".cmd", ".lnk")) and low not in LAUNCHERS:
                problems.append("a launcher that isn't a release one: " + rel)
            if rel.lower() in PLAYER_FILES:
                problems.append("the player's own settings file (it ships as settings\\defaults\\): " + rel)
            if "mymusic" in rel.lower():
                problems.append("the developer's own music folder: " + rel)
            if re.search(r"users[\\/]|downloads|xbox360", rel.lower()):
                problems.append("a path from the developer's PC: " + rel)
            listed.append((rel, os.path.getsize(os.path.join(root, fn)), sha256(os.path.join(root, fn))))
    with open(os.path.join(out, "RELEASE_CONTENTS.txt"), "w", encoding="ascii", newline="\r\n") as f:
        f.write("PDZ Restored %s release contents (stage_release.py). Microsoft files: only %s (DXC "
                "redistributables, see THIRD_PARTY_NOTICES.md). No game files, no translated shaders, no Visual C++ "
                "runtime DLLs, no audio files. Settings ship as settings\\defaults\\ (the player's own files are made "
                "from them at the first start and never replaced).\n" % (version, " and ".join(sorted(MICROSOFT_OK))))
        f.write("%-44s %12s  %s\n" % ("file", "bytes", "sha256"))
        for rel, size, h in listed:
            f.write("%-44s %12d  %s\n" % (rel, size, h))
        f.write("Built into PDZRestored.exe (hashes, formats and pipeline state only; no game code):\n")
        for rel, rows, size, h in embedded:
            f.write("%-44s %12d  %s  (%d rows)\n" % (rel, size, h, rows))
    for rel, size, h in listed:
        print("%-44s %12d  %s" % (rel, size, h[:16]))
    for rel, rows, size, h in embedded:
        print("%-44s %12d  %s  in the exe, %d rows" % (rel, size, h[:16], rows))
    if problems:
        print("PROBLEMS:\n  " + "\n  ".join(problems))
        sys.exit(1)
    print("PDZ Restored %s: %d files, %.1f MB; check passed" % (version, len(listed),
                                                               sum(s for _, s, _ in listed) / 1e6))
    if a.no_zip:
        return
    # The zip: exactly the checked files and the contents list, under one folder.
    folder = "PDZRestored-" + version
    zip_path = os.path.join(os.path.dirname(out), folder + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in [r for r, _, _ in listed] + ["RELEASE_CONTENTS.txt"]:
            z.write(os.path.join(out, rel), folder + "/" + rel.replace(os.sep, "/"))
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        bad = [n for n in names if os.path.splitext(n.lower())[1] in BANNED_EXT or
               os.path.basename(n.lower()).startswith(BANNED_NAMES)]
        if bad or z.testzip() is not None:
            sys.exit("zip check failed: %s" % (bad or "a damaged member"))
    print("zip: %s (%d files, %.1f MB)" % (zip_path, len(names), os.path.getsize(zip_path) / 1e6))


if __name__ == "__main__":
    main()
