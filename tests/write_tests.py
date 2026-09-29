"""Tests metadata editing (incremental update) with pdfprops --set, verified with pikepdf/qpdf.

Usage: python tests/write_tests.py [path/to/pdfprops.exe]   (run tests/make_samples.py first)
"""
import json
import re
import os
import shutil
import subprocess
import sys
import tempfile

import pikepdf

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "samples")
EXE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "build", "pdfprops.exe"))

TITLE = "Nëw tïtle 日本 😀 (x)"
EDITS = ["Title=" + TITLE, "Author=Ann; Bob", "Subject=", "Keywords=k1, k2", "Company=ACME \\ Co"]

WRITABLE = ["classic.pdf", "objstm.pdf", "qdf.pdf", "utf16.pdf", "syn_hybrid.pdf", "syn_incremental.pdf",
            "syn_xmp_only.pdf", "syn_encodings.pdf"]
REFUSED = {"aes128.pdf": "encrypted", "rc4_40.pdf": "encrypted", "user_password.pdf": "encrypted",
           "signed_flags.pdf": "signed", "syn_broken_offsets.pdf": "damaged", "syn_no_xref.pdf": "damaged",
           "syn_broken_objstm.pdf": "damaged", "syn_xref_starts_at_1.pdf": "damaged",
           "syn_random.pdf": "not a PDF"}

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("        FAIL: " + what)


def run(args):
    return subprocess.run([EXE] + args, capture_output=True)


def ours(path):
    p = run(["--json", path])
    return json.loads(p.stdout.decode("utf-8"))


def problems(path):
    # qpdf's full check plus any warnings raised while opening (e.g. xref recovery)
    with pikepdf.open(path) as pdf:
        msgs = set(pdf.check_pdf_syntax()) | set(pdf.get_warnings())
    # messages carry the file name and byte offsets, which differ between the original and the copy
    return {re.sub(r"offset \d+", "offset N", m.replace(path, "FILE")) for m in msgs}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    tmp = tempfile.mkdtemp()
    for name in WRITABLE:
        print("write  " + name)
        src = os.path.join(SAMPLES, name)
        out = os.path.join(tmp, name)
        args = []
        for e in EDITS:
            args += ["--set", e]
        p = run(args + ["--output", out, src])
        check(p.returncode == 0, "%s: pdfprops failed: %s" % (name, p.stderr.decode(errors="replace")))
        if p.returncode:
            continue
        orig = open(src, "rb").read()
        new = open(out, "rb").read()
        check(new[: len(orig)] == orig, name + ": original bytes changed (not an incremental update)")
        check(not (problems(out) - problems(src)), name + ": qpdf reports new problems: %s" % (problems(out) - problems(src)))
        with pikepdf.open(src) as a, pikepdf.open(out) as b:
            check(len(a.pages) == len(b.pages), name + ": page count changed")
            di = b.docinfo
            check(str(di.get("/Title")) == TITLE, name + ": pikepdf Title = %r" % di.get("/Title"))
            check(str(di.get("/Author")) == "Ann; Bob", name + ": pikepdf Author")
            check("/Subject" not in di, name + ": Subject not removed")
            check(str(di.get("/Keywords")) == "k1, k2", name + ": pikepdf Keywords")
            check(str(di.get("/Company")) == "ACME \\ Co", name + ": pikepdf Company = %r" % di.get("/Company"))
            check("/ModDate" in di, name + ": ModDate missing")
            for k in ("/Producer", "/Creator", "/CreationDate", "/Category"):
                if k in a.docinfo:
                    check(str(a.docinfo[k]) == str(di.get(k)), name + ": %s not preserved" % k)
            if "/Metadata" in a.Root:
                m = b.open_metadata()
                check(m.get("dc:title") == TITLE, name + ": XMP dc:title = %r" % m.get("dc:title"))
                check(list(m.get("dc:creator") or []) == ["Ann", "Bob"], name + ": XMP dc:creator = %r" % m.get("dc:creator"))
                check(not m.get("dc:description"), name + ": XMP dc:description not removed")
                check(m.get("pdf:Keywords") == "k1, k2", name + ": XMP pdf:Keywords")
        g = ours(out)
        check(g["title"] == TITLE and g["author"] == "Ann; Bob" and g["subject"] == "" and g["keywords"] == "k1, k2"
              and g["company"] == "ACME \\ Co" and not g["reconstructed"], name + ": our reader got %r" % g)

        # A second update on top of the first one.
        p = run(["--set", "Title=Second", "--in-place", out])
        check(p.returncode == 0, name + ": second update failed: " + p.stderr.decode(errors="replace"))
        with pikepdf.open(out) as b:
            check(str(b.docinfo.get("/Title")) == "Second", name + ": second update Title")
            check(str(b.docinfo.get("/Author")) == "Ann; Bob", name + ": second update lost Author")
        g = ours(out)
        check(g["title"] == "Second" and g["author"] == "Ann; Bob", name + ": our reader after second update")
        check(not (problems(out) - problems(src)), name + ": qpdf problems after second update")

    for name, reason in REFUSED.items():
        print("refuse " + name)
        out = os.path.join(tmp, "refused_" + name)
        p = run(["--set", "Title=x", "--output", out, os.path.join(SAMPLES, name)])
        err = p.stderr.decode(errors="replace")
        check(p.returncode == 1 and reason in err, "%s: expected refusal (%s), got rc=%d %s" % (name, reason, p.returncode, err))
        check(not os.path.exists(out), name + ": output written despite refusal")

    # Through the DLL's COM interface (IPropertyStore::SetValue + Commit), when the test host is built.
    build = os.path.dirname(EXE)
    host, dll = os.path.join(build, "handler_test.exe"), os.path.join(build, "zPDFPropertyHandler.dll")
    if os.path.exists(host) and os.path.exists(dll):
        com = {"classic.pdf": True, "objstm.pdf": True, "syn_xmp_only.pdf": True, "syn_hybrid.pdf": True,
               "aes128.pdf": False, "signed_flags.pdf": False, "syn_broken_offsets.pdf": False}
        for name, writable in com.items():
            print("com    " + name)
            src = os.path.join(SAMPLES, name)
            out = os.path.join(tmp, "com_" + name)
            shutil.copyfile(src, out)
            p = subprocess.run([host, dll, "--write", out], capture_output=True)
            text = p.stdout.decode("utf-8", errors="replace")
            check(p.returncode == 0, name + ": handler_test --write failed: " + text)
            check(("writable=yes commit=0x00000000" in text) == writable and ("writable=no" in text) != writable,
                  name + ": unexpected COM write result: " + text.splitlines()[1] if len(text.splitlines()) > 1 else text)
            if not writable:
                check(open(out, "rb").read() == open(src, "rb").read(), name + ": file changed although read-only")
                continue
            orig = open(src, "rb").read()
            check(open(out, "rb").read()[: len(orig)] == orig, name + ": COM update not incremental")
            check(not (problems(out) - problems(src)), name + ": qpdf problems after COM update")
            with pikepdf.open(out) as b:
                check(str(b.docinfo.get("/Title")) == "COM ✓ edit", name + ": COM Title")
                check(str(b.docinfo.get("/Author")) == "Ann; Bob", name + ": COM Author")
                check("/Subject" not in b.docinfo, name + ": COM Subject not removed")
    else:
        print("(skipping COM tests: build handler_test.exe and the DLL first)")

    shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
