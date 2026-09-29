"""Generates test PDFs into tests/samples/ and writes tests/samples/expected.json.

Usage: python tests/make_samples.py
Requires pikepdf. Expected values for pikepdf-written files come from pikepdf itself
(docinfo / XMP / page count); hand-built files carry explicit expectations.
"""
import json
import os
import random
import re
import zlib

import pikepdf
from pikepdf import Name, String

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "samples")

INFO = {
    "Title": "Sample Title",
    "Author": "Alice Example; Bob Example",
    "Subject": "Testing the property handler",
    "Keywords": "pdf, metadata, explorer",
    "Creator": "make_samples.py",
    "Producer": "pikepdf",
    "CreationDate": "D:20240102030405+01'00'",
    "ModDate": "D:20250607080910Z",
    "Company": "Example Corp",
    "Category": "Tests",
    "Comments": "Custom Info keys",
}

expected = {}


def path(name):
    return os.path.join(OUT, name)


def make_doc(pages=3, info=INFO, xmp=False):
    pdf = pikepdf.new()
    for _ in range(pages):
        pdf.add_blank_page(page_size=(200, 200))
    for k, v in info.items():
        pdf.docinfo[Name("/" + k)] = String(v)
    if xmp:
        with pdf.open_metadata(set_pikepdf_as_editor=False, update_docinfo=False) as m:
            m["dc:title"] = "XMP Title"
    return pdf


def expect_from_pikepdf(fname, password=""):
    """Expected values as pikepdf sees them (Info first, XMP fills gaps, like the handler)."""
    with pikepdf.open(path(fname), password=password) as pdf:
        di = pdf.docinfo
        def g(k):
            v = di.get("/" + k)
            return str(v).strip() if v is not None else ""
        e = {k.lower(): g(k) for k in ("Title", "Author", "Subject", "Keywords", "Creator", "Producer",
                                       "Company", "Category", "Comments")}
        e["pages"] = len(pdf.pages)
        e["version"] = pdf.pdf_version
        e["created_raw"] = g("CreationDate")
        e["modified_raw"] = g("ModDate")
    return e


def save(pdf, fname, **kw):
    pdf.save(path(fname), **kw)


# ---------------------------------------------------------------------------
# Hand-built PDFs
# ---------------------------------------------------------------------------

class Builder:
    """Minimal raw PDF writer for synthetic edge cases."""

    def __init__(self, version="1.4", prefix=b""):
        self.buf = bytearray(prefix + b"%PDF-" + version.encode() + b"\n%\xe2\xe3\xcf\xd3\n")
        self.offsets = {}

    def obj(self, num, body, stream=None, gen=0):
        self.offsets[num] = len(self.buf)
        self.buf += b"%d %d obj\n" % (num, gen)
        self.buf += body if isinstance(body, bytes) else body.encode("latin-1")
        if stream is not None:
            self.buf += b"\nstream\n" + stream + b"\nendstream"
        self.buf += b"\nendobj\n"

    def xref_table(self, trailer, start_at_one_bug=False, nums=None):
        nums = sorted(nums if nums is not None else self.offsets)
        pos = len(self.buf)
        size = max(nums) + 1
        self.buf += b"xref\n"
        if start_at_one_bug:
            self.buf += b"1 %d\n" % size
        else:
            self.buf += b"0 %d\n" % size
        self.buf += b"0000000000 65535 f\r\n"
        for n in range(1, size):
            if n in self.offsets and n in nums:
                self.buf += b"%010d 00000 n\r\n" % self.offsets[n]
            else:
                self.buf += b"0000000000 00000 f\r\n"
        self.buf += b"trailer\n" + trailer.encode("latin-1") + b"\n"
        return pos

    def startxref(self, pos):
        self.buf += b"startxref\n%d\n%%%%EOF\n" % pos

    def write(self, fname):
        with open(path(fname), "wb") as f:
            f.write(self.buf)


def basic_objects(b, info_body, pages=2, first_page=3):
    kids = " ".join("%d 0 R" % (first_page + i) for i in range(pages))
    b.obj(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.obj(2, "<< /Type /Pages /Kids [%s] /Count %d >>" % (kids, pages))
    for i in range(pages):
        b.obj(first_page + i, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] >>")
    b.obj(10, info_body)


def xref_stream_bytes(entries, size, w=(1, 4, 2), predictor=False):
    rows = b""
    for n in range(size):
        t, f2, f3 = entries.get(n, (0, 0, 65535 if n == 0 else 0))
        rows += t.to_bytes(w[0], "big") + f2.to_bytes(w[1], "big") + f3.to_bytes(w[2], "big")
    if predictor:
        width = sum(w)
        prev = bytes(width)
        out = b""
        for i in range(0, len(rows), width):
            row = rows[i:i + width]
            out += b"\x02" + bytes((row[j] - prev[j]) & 0xFF for j in range(width))
            prev = row
        rows = out
    return zlib.compress(rows)


def build_synthetic():
    # 1. xref subsection header "1 N" bug + PDFDocEncoding title with special chars
    b = Builder()
    basic_objects(b, r"<< /Title (Bullet \200 Euro \240 quote \215x\216 fi\223) /Author (Doc Enc) >>")
    pos = b.xref_table("<< /Size 11 /Root 1 0 R /Info 10 0 R >>", start_at_one_bug=True)
    b.startxref(pos)
    b.write("syn_xref_starts_at_1.pdf")
    expected["syn_xref_starts_at_1.pdf"] = {"title": "Bullet • Euro € quote “x” fiﬁ",
                                            "author": "Doc Enc", "pages": 2, "version": "1.4"}

    # 2. Wrong offsets everywhere -> reconstruction
    b = Builder()
    basic_objects(b, "<< /Title (Broken offsets) /Producer (garbage) >>", pages=3)
    b.offsets = {n: o + 7 for n, o in b.offsets.items()}
    pos = b.xref_table("<< /Size 11 /Root 1 0 R /Info 10 0 R >>")
    b.startxref(pos + 3)
    b.write("syn_broken_offsets.pdf")
    expected["syn_broken_offsets.pdf"] = {"title": "Broken offsets", "producer": "garbage", "pages": 3,
                                          "reconstructed": True}

    # 3. No xref / trailer / startxref at all, and junk before the header
    b = Builder(prefix=b"JUNK" * 10 + b"\n")
    basic_objects(b, "<< /Title (No xref at all) /CreationDate (D:2001) >>", pages=1)
    b.write("syn_no_xref.pdf")
    expected["syn_no_xref.pdf"] = {"title": "No xref at all", "pages": 1, "created": "2001-01-01 00:00:00",
                                   "reconstructed": True}

    # 4. Wrong and indirect /Length on the XMP stream, Info-less file (XMP only)
    xmp = ("<?xpacket begin='﻿' id='W5M0MpCehiHzreSzNTczkc9d'?>"
           "<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>"
           "<rdf:Description rdf:about='' xmlns:dc='http://purl.org/dc/elements/1.1/' "
           "xmlns:xmp='http://ns.adobe.com/xap/1.0/' xmlns:pdf='http://ns.adobe.com/pdf/1.3/' "
           "xmlns:pdfx='http://ns.adobe.com/pdfx/1.3/' pdfx:Category='XMP Category' "
           "pdf:Producer='Attr &amp; Producer' xmp:CreateDate='2020-05-06T07:08:09-03:00'>"
           "<pdfx:Company>XMP Company</pdfx:Company>"
           "<dc:title><rdf:Alt><rdf:li xml:lang='de-DE'>Deutscher Titel</rdf:li>"
           "<rdf:li xml:lang='x-default'>XMP &lt;Title&gt; &#x263A;</rdf:li></rdf:Alt></dc:title>"
           "<dc:creator><rdf:Seq><rdf:li>First Author</rdf:li><rdf:li>Second Author</rdf:li></rdf:Seq></dc:creator>"
           "<dc:description><rdf:Alt><rdf:li xml:lang='x-default'>XMP description</rdf:li></rdf:Alt></dc:description>"
           "<dc:subject><rdf:Bag><rdf:li>alpha</rdf:li><rdf:li>beta</rdf:li></rdf:Bag></dc:subject>"
           "<xmp:ModifyDate>2021-01-02T03:04:05Z</xmp:ModifyDate>"
           "<xmp:CreatorTool>Hand Tool</xmp:CreatorTool>"
           "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end='w'?>").encode("utf-8")
    b = Builder(version="1.6")
    b.obj(1, "<< /Type /Catalog /Pages 2 0 R /Metadata 5 0 R /Version /1.7 >>")
    b.obj(2, "<< /Type /Pages /Kids [3 0 R 4 0 R] >>")  # no /Count: pages must be counted
    b.obj(3, "<< /Type /Page /Parent 2 0 R >>")
    b.obj(4, "<< /Type /Pages /Parent 2 0 R /Kids [7 0 R 8 0 R] >>")
    b.obj(5, "<< /Type /Metadata /Subtype /XML /Length 6 0 R >>", stream=xmp)
    b.obj(6, "%d" % (len(xmp) + 40))  # wrong length -> endstream search
    b.obj(7, "<< /Type /Page /Parent 4 0 R >>")
    b.obj(8, "<< /Type /Page /Parent 4 0 R >>")
    pos = b.xref_table("<< /Size 9 /Root 1 0 R >>")
    b.startxref(pos)
    b.write("syn_xmp_only.pdf")
    expected["syn_xmp_only.pdf"] = {
        "title": "XMP <Title> ☺", "author": "First Author; Second Author", "subject": "XMP description",
        "keywords": "alpha; beta", "creator": "Hand Tool", "producer": "Attr & Producer",
        "created": "2020-05-06 07:08:09-03:00", "modified": "2021-01-02 03:04:05+00:00",
        "company": "XMP Company", "category": "XMP Category", "comments": "",
        "pages": 3, "version": "1.7"}

    # 5. Hybrid file: classic table + /XRefStm; Info lives in an object stream.
    b = Builder(version="1.5")
    b.obj(1, "<< /Type /Catalog /Pages 2 0 R >>")
    b.obj(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.obj(3, "<< /Type /Page /Parent 2 0 R >>")
    members = [(10, "<< /Title (Hybrid \\(objstm\\)) /Author <FEFF00C9006D0069006C0065> >>"), (11, "42")]
    hdr = b""
    body = b""
    for n, s in members:
        hdr += b"%d %d " % (n, len(body))
        body += s.encode() + b"\n"
    data = zlib.compress(hdr + body)
    b.obj(4, "<< /Type /ObjStm /N 2 /First %d /Filter /FlateDecode /Length %d >>" % (len(hdr), len(data)), stream=data)
    xs_off = len(b.buf)
    entries = {1: (1, b.offsets[1], 0), 2: (1, b.offsets[2], 0), 3: (1, b.offsets[3], 0),
               4: (1, b.offsets[4], 0), 10: (2, 4, 0), 11: (2, 4, 1), 12: (1, xs_off, 0)}
    xdata = xref_stream_bytes(entries, 13, predictor=True)
    b.obj(12, "<< /Type /XRef /Size 13 /W [1 4 2] /Filter /FlateDecode "
              "/DecodeParms << /Predictor 12 /Columns 7 >> /Length %d >>" % len(xdata), stream=xdata)
    pos = b.xref_table("<< /Size 13 /Root 1 0 R /Info 10 0 R /XRefStm %d >>" % xs_off, nums=[1, 2, 3, 4])
    hybrid_body = bytes(b.buf)
    b.startxref(pos)
    b.write("syn_hybrid.pdf")
    expected["syn_hybrid.pdf"] = {"title": "Hybrid (objstm)", "author": "Émile", "pages": 1}

    # 5b. Same file with a broken startxref: reconstruction finds the trailer (so no full object scan),
    # and the Info object, which lives in an object stream, is indexed lazily on the first lookup miss.
    b.buf = bytearray(hybrid_body)
    b.startxref(0)
    b.write("syn_broken_objstm.pdf")
    expected["syn_broken_objstm.pdf"] = {"title": "Hybrid (objstm)", "author": "Émile", "pages": 1,
                                         "reconstructed": True}

    # 6. Incremental update with a /Prev chain; newest Info wins
    b = Builder()
    basic_objects(b, "<< /Title (Old title) /Author (Old author) >>", pages=2)
    pos1 = b.xref_table("<< /Size 11 /Root 1 0 R /Info 10 0 R >>")
    b.startxref(pos1)
    b.obj(10, "<< /Title <FEFF004E0065007700200074006900740025> /Keywords (a;b) >>")
    pos2 = len(b.buf)
    b.buf += b"xref\n0 1\n0000000000 65535 f\r\n10 1\n%010d 00000 n\r\n" % b.offsets[10]
    b.buf += b"trailer\n<< /Size 11 /Root 1 0 R /Info 10 0 R /Prev %d >>\n" % pos1
    b.startxref(pos2)
    b.write("syn_incremental.pdf")
    expected["syn_incremental.pdf"] = {"title": "New tit%", "author": "", "keywords": "a;b", "pages": 2}

    # 7. UTF-16LE BOM (non-standard), language escape, ASCIIHex + ASCII85 filtered XMP
    b = Builder()
    title_le = "﻿LE tïtle".encode("utf-16-le")
    title_hex = title_le.hex().upper()
    lang_title = "FEFF001B656E001B0048006900"  # <ESC>en<ESC>Hi + trailing NUL
    xmp = (b"<x:xmpmeta xmlns:x='adobe:ns:meta/'><rdf:RDF><rdf:Description xmlns:dc='http://purl.org/dc/elements/1.1/'>"
           b"<dc:description><rdf:Alt><rdf:li xml:lang='x-default'>Hex85 subject</rdf:li></rdf:Alt></dc:description>"
           b"</rdf:Description></rdf:RDF></x:xmpmeta>")
    import base64
    a85 = base64.a85encode(xmp.hex().encode()) + b"~>"
    b.obj(1, "<< /Type /Catalog /Pages 2 0 R /Metadata 5 0 R >>")
    b.obj(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    b.obj(3, "<< /Type /Page /Parent 2 0 R >>")
    b.obj(5, "<< /Type /Metadata /Filter [/A85 /AHx] /Length %d >>" % len(a85), stream=a85)
    b.obj(10, "<< /Title <%s> /Author <%s> >>" % (title_hex, lang_title))
    pos = b.xref_table("<< /Size 11 /Root 1 0 R /Info 10 0 R >>")
    b.startxref(pos)
    b.write("syn_encodings.pdf")
    expected["syn_encodings.pdf"] = {"title": "LE tïtle", "author": "Hi", "subject": "Hex85 subject", "pages": 1}

    # 8. Pathological: deep nesting, reference cycles, self-referencing /Pages, huge /Count
    b = Builder()
    b.obj(1, "<< /Type /Catalog /Pages 2 0 R /Deep %s >>" % ("[" * 5000 + "]" * 5000))
    b.obj(2, "<< /Type /Pages /Kids [2 0 R 3 0 R 2 0 R] >>")
    b.obj(3, "<< /Type /Page /Parent 2 0 R >>")
    b.obj(10, "<< /Title 11 0 R >>")
    b.obj(11, "12 0 R")
    b.obj(12, "11 0 R")
    pos = b.xref_table("<< /Size 13 /Root 1 0 R /Info 10 0 R >>")
    b.startxref(pos)
    b.write("syn_pathological.pdf")
    expected["syn_pathological.pdf"] = {"title": "", "pages": 1}

    # 9. Garbage and truncated inputs must not crash
    random.seed(1234)
    with open(path("syn_random.pdf"), "wb") as f:
        f.write(bytes(random.getrandbits(8) for _ in range(50000)))
    expected["syn_random.pdf"] = {"ok": False}
    with open(path("classic.pdf"), "rb") as f:
        data = f.read()
    with open(path("syn_truncated.pdf"), "wb") as f:
        f.write(data[: len(data) * 2 // 3])
    expected["syn_truncated.pdf"] = {"ok": True}
    with open(path("syn_empty.pdf"), "wb") as f:
        pass
    expected["syn_empty.pdf"] = {"ok": False}


def main():
    os.makedirs(OUT, exist_ok=True)

    pdf = make_doc()
    save(pdf, "classic.pdf", object_stream_mode=pikepdf.ObjectStreamMode.disable, compress_streams=False)
    save(pdf, "objstm.pdf", object_stream_mode=pikepdf.ObjectStreamMode.generate)
    save(pdf, "qdf.pdf", qdf=True)  # qpdf's QDF mode (classic, uncompressed, comments)

    utf = make_doc(pages=1, info={"Title": "Ünïcödé 日本語 😀", "Author": "Zoë; Ærø", "Keywords": "a;b;c"})
    save(utf, "utf16.pdf", object_stream_mode=pikepdf.ObjectStreamMode.generate)

    enc = [
        ("rc4_40.pdf", dict(R=2, metadata=False, aes=False)),
        ("rc4_128.pdf", dict(R=3, metadata=False, aes=False)),
        ("aes128.pdf", dict(R=4, aes=True)),
        ("aes128_meta_clear.pdf", dict(R=4, aes=True, metadata=False)),
        ("rc4_128_r4.pdf", dict(R=4, aes=False, metadata=False)),
        ("aes256_r6.pdf", dict(R=6)),
    ]
    for fname, kw in enc:
        d = make_doc(pages=2, xmp=True)
        save(d, fname, encryption=pikepdf.Encryption(owner="owner-secret", user="", **kw),
             object_stream_mode=pikepdf.ObjectStreamMode.generate)
    try:
        d = make_doc(pages=2)
        save(d, "aes256_r5.pdf", encryption=pikepdf.Encryption(owner="owner-secret", user="", R=5))
    except Exception as ex:  # qpdf may refuse the deprecated R5
        print("skipping R5:", ex)

    # Signature flags set (no real signature needed): metadata edits must be refused.
    d = make_doc(pages=1)
    d.Root.AcroForm = pikepdf.Dictionary(Fields=pikepdf.Array(), SigFlags=3)
    save(d, "signed_flags.pdf")

    d = make_doc(pages=4)
    save(d, "user_password.pdf", encryption=pikepdf.Encryption(owner="o", user="secret", R=6),
         object_stream_mode=pikepdf.ObjectStreamMode.disable)

    for fname in os.listdir(OUT):
        if fname.endswith(".pdf") and not fname.startswith("syn_"):
            try:
                expected[fname] = expect_from_pikepdf(fname)
            except pikepdf.PasswordError:
                with pikepdf.open(path(fname), password="secret") as p:
                    expected[fname] = {"pages": len(p.pages), "title": "", "encrypted": True,
                                       "decryptable": False, "version": p.pdf_version}
    xmp_file = "aes128_meta_clear.pdf"
    for fname in ("rc4_40.pdf", "rc4_128.pdf", "aes128.pdf", xmp_file, "rc4_128_r4.pdf", "aes256_r6.pdf",
                  "aes256_r5.pdf", "user_password.pdf"):
        if fname in expected:
            expected[fname]["encrypted"] = True

    build_synthetic()
    with open(path("expected.json"), "w", encoding="utf-8") as f:
        json.dump(expected, f, indent=1, ensure_ascii=False, sort_keys=True)
    print("wrote %d samples to %s" % (len(expected), OUT))


if __name__ == "__main__":
    main()
