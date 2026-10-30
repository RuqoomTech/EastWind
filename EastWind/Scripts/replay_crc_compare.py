# Runs two game exes on each replay with -replayCRCLog and reports the first CRC that differs.
# Use it to check that two builds (e.g. win32 and win64) simulate a replay the same way.
# Usage: replay_crc_compare.py <game dir> <exeA> <exeB> <replay.rep>...
#   The exes are run from the game dir; replay paths are relative to the user's Replays folder.
import os
import subprocess
import sys

game, a, b, reps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]


def run(exe, rep):
    p = subprocess.run([os.path.join(game, exe), '-headless', '-replayCRCLog', '-replay', rep],
                       cwd=game, capture_output=True, text=True, timeout=1800)
    return p.returncode, [l for l in p.stdout.splitlines() if l.startswith('CRC Frame')]


bad = 0
for rep in reps:
    (ca, la), (cb, lb) = run(a, rep), run(b, rep)
    diff = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y), None)
    ok = diff is None and len(la) == len(lb) and len(la) > 0 and ca == cb == 0
    bad += not ok
    if ok:
        msg = 'same'
    elif diff is not None:
        msg = 'first diff: %s | %s' % (la[diff], lb[diff])
    else:
        msg = 'lengths %d/%d exit %d/%d' % (len(la), len(lb), ca, cb)
    print('%-40s %5d CRCs  %s' % (rep, len(la), msg), flush=True)
sys.exit(1 if bad else 0)
