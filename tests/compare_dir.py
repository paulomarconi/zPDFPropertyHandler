"""Compares pdfprops with pikepdf on every PDF under a directory tree (read-only).

Usage: python tests/compare_dir.py DIR [--limit N] [--exe path/to/pdfprops.exe]
"""
import argparse
import json
import os
import subprocess
import sys
import time

import pikepdf

HERE = os.path.dirname(os.path.abspath(__file__))
FIELDS = ("title", "author", "subject", "keywords", "creator", "producer")


def reference(path):
    with pikepdf.open(path) as pdf:
        info = {}
        di = pdf.docinfo
        for k in FIELDS:
            v = di.get("/" + k.capitalize())
            info[k] = str(v).replace("\x00", "").strip() if isinstance(v, pikepdf.String) else ""
        try:
            meta = pdf.open_metadata()
            xmp = {
                "title": meta.get("dc:title"), "author": meta.get("dc:creator"),
                "subject": meta.get("dc:description"), "keywords": meta.get("pdf:Keywords"),
                "creator": meta.get("xmp:CreatorTool"), "producer": meta.get("pdf:Producer"),
            }
            for k, v in xmp.items():
                if not info[k] and v:
                    info[k] = "; ".join(v) if isinstance(v, (list, set)) else str(v).strip()
        except Exception:
            pass
        info["pages"] = len(pdf.pages)
        return info


def norm(s):
    return " ".join(str(s).split())


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--limit", type=int, default=500)
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "pdfprops.exe"))
    a = ap.parse_args()
    files = []
    for root, _, names in os.walk(a.dir):
        files += [os.path.join(root, n) for n in names if n.lower().endswith(".pdf")]
        if len(files) >= a.limit:
            break
    files = files[: a.limit]
    n_ok = n_diff = n_skip = 0
    worst = 0.0
    for f in files:
        try:
            ref = reference(f)
        except Exception as ex:
            n_skip += 1
            continue
        t = time.perf_counter()
        p = subprocess.run([a.exe, "--json", f], capture_output=True, timeout=120)
        worst = max(worst, time.perf_counter() - t)
        if not p.stdout:
            print("CRASH", f, p.returncode)
            n_diff += 1
            continue
        got = json.loads(p.stdout.decode("utf-8"))
        diffs = []
        for k in FIELDS:
            if norm(ref[k]) != norm(got[k]):
                diffs.append("%s: pikepdf=%r ours=%r" % (k, ref[k][:80], got[k][:80]))
        if ref["pages"] != got["pages"]:
            diffs.append("pages: pikepdf=%d ours=%d" % (ref["pages"], got["pages"]))
        if diffs:
            n_diff += 1
            print("DIFF", f)
            for d in diffs:
                print("      " + d)
        else:
            n_ok += 1
    print("\n%d match, %d differ, %d skipped (pikepdf could not open); slowest %.2fs" % (n_ok, n_diff, n_skip, worst))


if __name__ == "__main__":
    main()
