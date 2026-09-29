"""Runs pdfprops --json on tests/samples/*.pdf and compares with expected.json.

Usage: python tests/run_tests.py [path/to/pdfprops.exe]
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "samples")
EXE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "pdfprops.exe"))


def pdf_date_to_str(raw):
    """Mirror of the handler's formatting for D:YYYYMMDDHHmmSSOHH'mm' dates."""
    m = re.match(r"(?:D:)?(\d{4})(\d{2})?(\d{2})?(\d{2})?(\d{2})?(\d{2})?(Z|[+-]\d{2}'?\d{0,2}'?)?", raw or "")
    if not m:
        return ""
    y, mo, d, h, mi, s, tz = m.groups()
    out = "%s-%s-%s %s:%s:%s" % (y, mo or "01", d or "01", h or "00", mi or "00", s or "00")
    if tz:
        if tz == "Z":
            out += "+00:00"
        else:
            digits = re.sub(r"\D", "", tz)
            out += "%s%s:%s" % (tz[0], digits[:2], (digits[2:4] or "00"))
    return out


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    with open(os.path.join(SAMPLES, "expected.json"), encoding="utf-8") as f:
        expected = json.load(f)
    failures = 0
    for fname in sorted(expected):
        exp = dict(expected[fname])
        p = subprocess.run([EXE, "--json", os.path.join(SAMPLES, fname)], capture_output=True, timeout=60)
        if p.returncode not in (0, 1) or not p.stdout:
            print("CRASH %-28s rc=%d %s" % (fname, p.returncode, p.stderr.decode(errors="replace")))
            failures += 1
            continue
        got = json.loads(p.stdout.decode("utf-8"))
        if "created_raw" in exp:
            exp["created"] = pdf_date_to_str(exp.pop("created_raw"))
        if "modified_raw" in exp:
            exp["modified"] = pdf_date_to_str(exp.pop("modified_raw"))
        diffs = []
        for k, v in exp.items():
            g = got.get(k)
            if g != v:
                diffs.append("%s: expected %r, got %r" % (k, v, g))
        if diffs:
            failures += 1
            print("FAIL  %s" % fname)
            for d in diffs:
                print("        " + d)
        else:
            print("ok    %s" % fname)
    print("\n%d file(s), %d failure(s)" % (len(expected), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
