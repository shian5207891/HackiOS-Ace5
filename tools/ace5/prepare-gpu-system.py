#!/usr/bin/env python3
"""Stage the minimal Ace 5 RAM system with Triangle as a normal application.

Uses the same local iOS inputs as userland/build.sh's system-image section.
Does not download files, overwrite system.apfs, or access the phone. Files are
hard-linked into a new tree; source files are never edited. Pack with mkapfs.py
and ios165/system.manifest, then validate before uploading the image to RAM.
"""
import argparse
import os
from pathlib import Path
import re
import shutil

REPO = Path(__file__).resolve().parents[2]
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--ios', type=Path, required=True)
ap.add_argument('--stage0', type=Path, required=True)
ap.add_argument('--triangle', type=Path, required=True, help='Signed triangle_app executable')
ap.add_argument('--output', type=Path, required=True, help='New directory on the same filesystem as iOS inputs')
ap.add_argument('--fw', type=Path, help='audio DSP firmware directory (adsp.mdt, adsp.b00..): copied to /fw')
ap.add_argument('--jb', type=Path, help='the jailbreak prefix (jb/root/var/jb): copied to /jb, its '
                                       'Applications/*.app to /Applications')
args = ap.parse_args()
root = args.output.resolve()
if args.output.exists() or args.output.is_symlink():
    ap.error('output already exists')
for name in ['rootfs', 'cryptex', 'cryptex-app']:
    source = args.ios / name
    if not source.is_dir() or source.is_symlink():
        ap.error(f'input must be a real extracted directory: {source}')
    if root.is_relative_to(source.resolve()):
        ap.error('output must not be inside an input tree')
for source in [args.stage0, args.triangle, args.ios / 'system.manifest']:
    if not source.is_file():
        ap.error(f'missing input: {source}')
cache = args.ios / '20F66__iPhone10,3_6'
caches = sorted(p for p in cache.iterdir() if re.fullmatch(
    r'dyld_shared_cache_arm64(?:\.\d+(?:\.dyld(?:data|linkedit))?)?', p.name))
if not caches or not (cache / 'dyld_shared_cache_arm64').is_file():
    ap.error('missing extracted iOS 16.5 dyld cache')
# This helper reproduces the minimal RAM image used by the Ace 5 trials. Do not
# silently omit optional additions from a different workspace's system image.
for extra in [REPO / 'fw-audio', REPO / 'jb/root/var/jb', REPO / 'apps']:
    if extra.exists() and not (args.jb is not None and extra.resolve() == args.jb.resolve()):
        ap.error(f'optional system content exists; use userland/build.sh to include it: {extra}')

shutil.copytree(args.ios / 'rootfs', root, symlinks=True, copy_function=os.link)
for name in ['System/Library/LaunchDaemons', 'System/Library/NanoLaunchDaemons',
             'System/Library/LaunchAngels', 'System/Library/xpc/launchd.plist']:
    path = root / name
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
(root / 'System/Library/LaunchDaemons').symlink_to('/redwood/LaunchDaemons')
for name in ['redwood', 'sbin', 'System/Volumes/iOS']:
    (root / name).mkdir(parents=True, exist_ok=True)
staged = root / 'private/var/staged_system_apps'
if staged.is_dir():
    for app in staged.iterdir():
        target = root / 'Applications' / app.name
        if target.exists():
            ap.error(f'duplicate application: {target}')
        app.rename(target)
    shutil.rmtree(root / 'private/var')
(root / 'sbin/launchd').rename(root / 'sbin/launchd.ios')
shutil.copy2(args.stage0, root / 'sbin/launchd')
app = root / 'Applications/RedwoodTriangle.app'
app.mkdir()
shutil.copy2(args.triangle, app / 'RedwoodTriangle')
shutil.copy2(REPO / 'userland/src/triangle_app.plist', app / 'Info.plist')
for relative, target in [
    ('System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver', '/redwood/lib/RedwoodAudio.driver'),
    ('System/Library/Extensions/RedwoodMetal.bundle', '/redwood/lib/RedwoodMetal.bundle'),
]:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.symlink_to(target)
for name, destination in [('cryptex', 'OS'), ('cryptex-app', 'App')]:
    shutil.copytree(args.ios / name, root / 'System/Cryptexes' / destination,
                    symlinks=True, copy_function=os.link)
destination = root / 'System/Cryptexes/OS/System/Library/Caches/com.apple.dyld'
destination.mkdir(parents=True, exist_ok=True)
for source in caches:
    target = destination / source.name
    if target.exists() or target.is_symlink():
        target.unlink()
    os.link(source, target)
if args.fw is not None:
    # The audio DSP's image belongs on this volume, not in the boot payload: 41 MB of firmware made the
    # payload 67 MB, and the loader will not take that ("cannot load run-payload-adsp.bin"). The kernel
    # looks in the small image first and then at /fw here (redwood_adsp.c: image_file).
    dest = root / 'fw'
    dest.mkdir(parents=True, exist_ok=True)
    copied = 0
    for item in sorted(args.fw.iterdir()):
        if item.is_file() and item.name.startswith('adsp.'):
            shutil.copy2(item, dest / item.name)
            copied += 1
    print(f'Staged {root}: {copied} files of the audio DSP at /fw')

if args.jb is not None:
    # The rootless prefix (Procursus + Sileo + ElleKit), as userland/build.sh puts it in when it makes
    # the image itself: /jb on the image, which init copies to /private/var/jb at every boot, and its
    # applications -- Sileo, TrollStore -- into /Applications, which is where LaunchServices rebuilds
    # its database from at every start (no installd, no container, no uicache).
    prefix = root / 'jb'
    shutil.copytree(args.jb, prefix, symlinks=True)
    apps = 0
    source = prefix / 'Applications'
    if source.is_dir():
        for app in sorted(source.iterdir()):
            if app.suffix != '.app' or not app.is_dir():
                continue
            target = root / 'Applications' / app.name
            if target.exists():
                ap.error(f'duplicate application: {target}')
            shutil.copytree(app, target, symlinks=True)
            apps += 1
    print(f'Staged {root}: the jailbreak prefix at /jb ({sum(1 for _ in prefix.rglob("*"))} entries), '
          f'{apps} of its applications in /Applications')

print(f'Staged {root}: normal Triangle bundle and {len(caches)} dyld cache parts; no device changed')
