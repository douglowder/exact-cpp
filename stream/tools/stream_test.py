#!/usr/bin/env python3
"""Runs exact-stream against fake_watcher.py and saves the fake board's screen.

  stream_test.py EXACT_STREAM_BIN OUTDIR [APP]
Writes OUTDIR/APP-0.png (first frame), APP-1.png (after ArrowRight), stats.
"""
import os, subprocess, sys, time

here = os.path.dirname(os.path.abspath(__file__))
bin_, outdir = sys.argv[1], sys.argv[2]
app = sys.argv[3] if len(sys.argv) > 3 else "motion"
os.makedirs(outdir, exist_ok=True)
fake = subprocess.Popen([sys.executable, "-u", os.path.join(here, "fake_watcher.py"), os.path.join(outdir, "end.png"), "30"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
port = fake.stdout.readline().split()[1]
host = subprocess.Popen([bin_, "--port", port, app], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
print(host.stdout.readline().strip())


def cmd(p, s):
    p.stdin.write(s + "\n")
    p.stdin.flush()


def wait_for(p, word, secs=10):
    end = time.time() + secs
    while time.time() < end:
        line = p.stdout.readline()
        if not line:
            break
        if word in line:
            return line.strip()
    return None


print(wait_for(host, "CONNECTED"))
time.sleep(2.5)
cmd(fake, f"png {outdir}/{app}-0.png")
wait_for(fake, "PNG")
cmd(fake, "key right")
time.sleep(1.5)
cmd(fake, f"png {outdir}/{app}-1.png")
wait_for(fake, "PNG")
cmd(fake, "tap 206 300")
time.sleep(1.0)
cmd(fake, f"png {outdir}/{app}-2.png")
wait_for(fake, "PNG")
cmd(host, "stats")
print(wait_for(host, "STATS"))
host.terminate()
fake.terminate()
