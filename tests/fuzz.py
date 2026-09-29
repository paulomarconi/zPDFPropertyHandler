"""Mutation fuzzing: corrupts sample PDFs and checks pdfprops (reading and editing) neither crashes nor hangs.

Usage: python tests/fuzz.py [iterations-per-sample] [seed]
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "samples")
EXE = os.path.join(HERE, "..", "build", "pdfprops.exe")
TOKENS = [b"obj", b"endobj", b"stream", b"endstream", b"xref", b"trailer", b"startxref", b"<<", b">>", b"[", b"]",
          b"(", b")", b"/Length", b"/Prev", b"/Root", b"/Kids", b" 0 R", b"99999999999", b"-1", b"/FlateDecode"]


def mutate(data, rnd):
    d = bytearray(data)
    for _ in range(rnd.randint(1, 8)):
        op = rnd.randrange(5)
        if not d:
            break
        i = rnd.randrange(len(d))
        if op == 0:
            d[i] = rnd.randrange(256)
        elif op == 1:
            del d[i:i + rnd.randint(1, 64)]
        elif op == 2:
            d[i:i] = rnd.choice(TOKENS)
        elif op == 3:
            j = rnd.randrange(len(d))
            d[i:i] = d[j:j + rnd.randint(1, 200)]
        else:
            del d[i:]
    return bytes(d)


def main():
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
    samples = [os.path.join(SAMPLES, f) for f in sorted(os.listdir(SAMPLES)) if f.endswith(".pdf")]
    bad = 0
    tmpdir = tempfile.mkdtemp()
    for s in samples:
        with open(s, "rb") as f:
            data = f.read()
        for k in range(iters):
            m = mutate(data, rnd)
            p = os.path.join(tmpdir, "fuzz.pdf")
            with open(p, "wb") as f:
                f.write(m)
            edited = os.path.join(tmpdir, "fuzz_edited.pdf")
            if os.path.exists(edited):
                os.remove(edited)
            try:
                # read, then edit (the writer must refuse or succeed, never crash), then read the edit
                cmds = [[EXE, "--json", p], [EXE, "--set", "Title=fuzz", "--output", edited, p]]
                for cmd in cmds:
                    r = subprocess.run(cmd, capture_output=True, timeout=10)
                    if r.returncode not in (0, 1):
                        break
                    if cmd is cmds[-1] and r.returncode == 0:
                        r = subprocess.run([EXE, "--json", edited], capture_output=True, timeout=10)
                if r.returncode not in (0, 1):
                    bad += 1
                    keep = os.path.join(tmpdir, "crash_%d.pdf" % bad)
                    os.replace(p, keep)
                    print("CRASH rc=%d from %s -> %s" % (r.returncode, os.path.basename(s), keep))
            except subprocess.TimeoutExpired:
                bad += 1
                keep = os.path.join(tmpdir, "hang_%d.pdf" % bad)
                os.replace(p, keep)
                print("HANG from %s -> %s" % (os.path.basename(s), keep))
    print("%d runs, %d problem(s)" % (iters * len(samples), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
