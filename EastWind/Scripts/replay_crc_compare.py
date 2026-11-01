# Runs two game exes on each replay with -replayCRCLog and reports the first CRC that differs.
# Run it before a push to check that two builds (e.g. before and after a change) simulate every replay the same way.
# Usage: replay_crc_compare.py <game dir> <exeA> <exeB> [replay.rep...]
#   The exes can be anywhere (e.g. build\win64\GeneralsMD\Release\generalszh.exe); they run from the game dir.
#   Replay paths are relative to the user's Replays folder. With no replays given, the repo's replays and
#   maps are copied there and all of them are checked. Our own EastWind/Replays were recorded with GameMath
#   builds, so they must also match the CRCs stored in the replay.
import ctypes
import glob
import os
import shutil
import subprocess
import sys

game, a, b, reps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
own = []
if not reps:
    repo = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    zh104 = os.path.join(repo, 'GeneralsReplays', 'GeneralsZH', '1.04')
    docs = ctypes.create_unicode_buffer(260)
    ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, docs)  # CSIDL_PERSONAL, as the game uses
    user = os.path.join(docs.value, 'Command and Conquer Generals Zero Hour Data')
    shutil.copytree(os.path.join(zh104, 'Maps'), os.path.join(user, 'Maps'), dirs_exist_ok=True)
    os.makedirs(os.path.join(user, 'Replays', 'eastwind-check'), exist_ok=True)
    ours = os.path.join(repo, 'EastWind', 'Replays')
    for src in (os.path.join(zh104, 'Replays'), ours):
        for f in sorted(glob.glob(os.path.join(src, '*.rep'))):
            shutil.copy2(f, os.path.join(user, 'Replays', 'eastwind-check'))
            reps.append('eastwind-check\\' + os.path.basename(f))
            if src == ours:
                own.append(reps[-1])


def run(exe, rep, log=True):
    args = [os.path.join(game, exe), '-useCwd', '-headless'] + ['-replayCRCLog'] * log + ['-replay', rep]
    p = subprocess.run(args, cwd=game, capture_output=True, text=True, timeout=1800)
    return p.returncode, [l for l in p.stdout.splitlines() if l.startswith('CRC Frame')]


bad = 0
for rep in reps:
    (ca, la), (cb, lb) = run(a, rep), run(b, rep)
    diff = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y), None)
    ok = diff is None and len(la) == len(lb) and len(la) > 0 and ca == cb == 0
    if ok and rep in own:
        ca, cb = run(a, rep, False)[0], run(b, rep, False)[0]
        ok = ca == cb == 0
        msg = 'same, matches recording' if ok else 'same, but differs from recording (exit %d/%d)' % (ca, cb)
    elif ok:
        msg = 'same'
    elif diff is not None:
        msg = 'first diff: %s | %s' % (la[diff], lb[diff])
    else:
        msg = 'lengths %d/%d exit %d/%d' % (len(la), len(lb), ca, cb)
    bad += not ok
    print('%-60s %5d CRCs  %s' % (rep, len(la), msg), flush=True)
print('%d of %d replays failed' % (bad, len(reps)))
sys.exit(1 if bad else 0)
