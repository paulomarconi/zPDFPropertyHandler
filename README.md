# zPDFPropertyHandler

A Windows shell **property handler** for `.pdf` files. It shows PDF metadata in Explorer (Details tab, preview
pane, tooltips and columns), makes it searchable in Windows Search, and lets you **edit** it from the Details tab.
It needs no PDF reader and no third-party libraries.

| PDF metadata | Explorer property | Editable |
|---|---|---|
| Title | Title | yes |
| Author (split on `;`) | Authors | yes |
| Subject | Subject | yes |
| Keywords (split on `;`, else `,`) | Tags | yes |
| custom `Company` / `Category` / `Comments` | Company / Categories / Comments | yes |
| Creator | Program name | |
| CreationDate / ModDate | Content created / Date last saved | |
| page count | Pages | |
| Producer | PDF producer (custom property) | |
| PDF version | PDF version (custom property, e.g. `1.7`) and Version (`PDF 1.7`) | |

Values come from the document Info dictionary. XMP metadata fills anything the Info dictionary lacks, including
Office's `pdfx:Company` / `pdfx:Category` / `pdfx:Comments`.

## Features

- Reads only what it needs through a small block cache: typically ~100 KB, even for a 50 MB PDF. It never loads
  the whole file.
- Supports classic xref tables, xref streams, hybrid files, incremental updates (`/Prev` chains) and object
  streams.
- Recovers damaged files: wrong offsets, a missing xref or trailer, and wrong stream `/Length` are rebuilt by
  scanning the file (at most the first 256 MB).
- Reads encrypted PDFs that have no user password (the common "owner password only" case): RC4 40/128-bit,
  AES-128 and AES-256 (R5/R6). If a user password is set, you still get what's readable, such as the page count.
- Handles text in UTF-16BE/LE, UTF-8 and PDFDocEncoding, with language escapes removed. Dates without a time zone
  are treated as local time.
- Hardened against malformed input: recursion, size and output limits, and it catches every exception. Reading and
  editing are tested with mutation fuzzing.

## Editing

Edit Title, Subject, Authors, Tags, Comments, Categories or Company in the Details tab of a PDF's Properties. The
change is saved as a standard PDF **incremental update**: the original bytes are left untouched, and a new Info
dictionary, an updated XMP packet (if the file has one) and a new cross-reference section are appended. `ModDate`
and `xmp:ModifyDate` are set to the current time, so "Date last saved" changes too.

Editing is refused (the fields are read-only) for:
- encrypted files;
- signed files (`/SigFlags` or `/Perms`): appending would show as "modified after signing";
- damaged files whose cross-reference data had to be repaired, since other readers might not apply the same repair.

The command-line tool can apply the same edits (see below).

## Install

Download a build from the Actions artifacts or build it yourself (see below). Then run:

```
scripts\install.cmd
```

The script asks for administrator rights, copies the 64-bit DLL to `%ProgramFiles%\zPDFPropertyHandler` and
registers it. If a 32-bit build exists (`build\x86`), it also installs it to `...\zPDFPropertyHandler\x86` and
registers it with the 32-bit `regsvr32`, so file dialogs in 32-bit applications show PDF properties too.
Registration also writes and registers the property schema (`zPDFPropertyHandler.propdesc`) for the two custom
properties.

Afterwards, restart Explorer (Task Manager → Windows Explorer → Restart) or sign out and back in. To update
existing search results, rebuild the index: Indexing Options → Advanced → Rebuild.

When updating, a DLL still loaded by Explorer is renamed and deleted at the next restart.

## Uninstall

```
scripts\uninstall.cmd
```

Registering takes over the system-wide `.pdf` property handler and sets the Explorer property lists (`FullDetails`,
`PreviewDetails`, `InfoTip`). Any previous values, for example from another PDF property extension, are backed up
and restored on uninstall. If you have another PDF property extension installed, uninstall it first.

## Build

Requirements: [MSYS2](https://www.msys2.org/) MinGW-w64 (`mingw-w64-x86_64-gcc`, `make`; add `mingw-w64-i686-gcc`
for the 32-bit DLL). Python with `pikepdf` is needed only for the tests.

```
make            # build/pdfprops.exe, build/zPDFPropertyHandler.dll, test programs
make test       # crypto self-test, sample PDFs, read + write tests (checked with pikepdf/qpdf), DLL through COM
make fuzz       # mutation fuzzing of reading and editing
make x86        # 32-bit DLL + test host in build/x86 (needs i686-w64-mingw32-g++ on PATH)
make test-x86   # run the 32-bit DLL through COM on the samples (after make test)
```

The binaries are statically linked and depend only on system DLLs. A `CMakeLists.txt` is included for MSVC:

```
cmake -S . -B build-msvc -A x64
cmake --build build-msvc --config Release
```

## Command-line tool

`pdfprops.exe` prints what the handler sees, without installing anything:

```
pdfprops [--json] file.pdf...
```

```
File:        report.pdf
Title:       Quarterly Report
Author:      Alice Example; Bob Example
Created:     2024-01-02 03:04:05+01:00
Pages:       36
PDF version: 1.7
Encryption:  AES-256 (V5 R6), no user password
I/O:         4 reads, 241690 of 11972634 bytes
```

It can also edit metadata, writing either to a new file or in place:

```
pdfprops --set "Title=New title" --set "Author=Ann; Bob" --set "Subject=" --output out.pdf in.pdf
pdfprops --set "Keywords=a, b" --in-place file.pdf
```

Keys: `Title`, `Author`, `Subject`, `Keywords`, `Comments`, `Category`, `Company`. An empty value removes the entry.

## Tests

| Script | Purpose |
|---|---|
| `tests/crypto_selftest.cpp` | known-answer tests for MD5, SHA-2, RC4, AES |
| `tests/make_samples.py` | generates sample PDFs with pikepdf plus hand-built edge cases, and the expected values |
| `tests/run_tests.py` | runs `pdfprops --json` on the samples and compares with the expected values |
| `tests/write_tests.py` | edits the samples (CLI and COM), checks with pikepdf/qpdf that the result is valid, incremental and correct, and that encrypted/signed/damaged files are refused |
| `tests/compare_dir.py DIR` | compares with pikepdf on every PDF under a folder |
| `tests/fuzz.py` | corrupts the samples randomly and checks that reading and editing never crash or hang |
| `tests/handler_test.cpp` | loads the DLL through `DllGetClassObject` and prints the property store as Explorer formats it (no registration needed); `--write` edits through `IPropertyStore`; `--shell` goes through the installed handler like Explorer |

## Layout

```
src/core/     portable C++17: lexer, xref, object streams, filters, encryption, metadata, incremental-update writer
src/handler/  COM DLL: IInitializeWithStream / IPropertyStore / IPropertyStoreCapabilities, registration, schema
src/cli/      pdfprops.exe
tests/        test programs and scripts
scripts/      install.cmd / uninstall.cmd
```

## Limitations and notes

- **User-password PDFs** show only unencrypted information (page count, version). Certificate-encrypted PDFs
  (`/Adobe.PubSec`) are not decrypted either.
- **Keywords** without `;` are split on commas, so a single keyword containing a comma becomes two tags.
- **Explorer's field lists** are set on `SystemFileAssociations\.pdf`. If your default PDF application defines its
  own `FullDetails`/`PreviewDetails` on its ProgID (e.g. `Acrobat.Document.DC`), Explorer shows that list instead.
  The metadata is still available as columns and in search.
- **Full-text search** of the PDF body comes from a PDF IFilter (installed by most PDF readers), not from this
  handler, which only provides metadata properties.
- **Editing** adds a few hundred bytes per save (incremental update). It doesn't linearize or compact the file.
- The DLL is **unsigned**, so Smart App Control and UAC could be necessary to disable.

## Author

paulomarconi 
https://paulomarconi.github.io/

## License

[MIT](LICENSE)
