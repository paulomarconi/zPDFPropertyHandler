// pdfprops: prints the metadata the property handler would expose.
//   pdfprops [--json] file.pdf...
//   pdfprops --set Key=Value [--set ...] (--output out.pdf | --in-place) file.pdf
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "../core/PdfMetadata.h"
#include "../core/PdfWriter.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

using namespace pdfprops;

namespace {

class FileSource : public ByteSource {
public:
    explicit FileSource(FILE* f) : f_(f) {
#ifdef _WIN32
        _fseeki64(f_, 0, SEEK_END);
        size_ = static_cast<uint64_t>(_ftelli64(f_));
#else
        fseeko(f_, 0, SEEK_END);
        size_ = static_cast<uint64_t>(ftello(f_));
#endif
    }
    uint64_t Size() override { return size_; }
    size_t ReadAt(uint64_t offset, void* buf, size_t len) override {
        ++reads_;
#ifdef _WIN32
        if (_fseeki64(f_, static_cast<long long>(offset), SEEK_SET) != 0) return 0;
#else
        if (fseeko(f_, static_cast<off_t>(offset), SEEK_SET) != 0) return 0;
#endif
        size_t n = fread(buf, 1, len, f_);
        bytes_ += n;
        return n;
    }
    uint64_t reads_ = 0, bytes_ = 0;

private:
    FILE* f_;
    uint64_t size_ = 0;
};

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

std::string JoinList(const std::vector<std::string>& v) {
    std::string o;
    for (const auto& s : v) o += (o.empty() ? "" : " | ") + s;
    return o;
}

void PrintText(const char* path, const PdfMetadata& md, const FileSource& fs) {
    printf("File:        %s\n", path);
    auto line = [](const char* k, const std::string& v) {
        if (!v.empty()) printf("%-12s %s\n", k, v.c_str());
    };
    line("Title:", md.title);
    line("Author:", md.author);
    line("  (split)", md.author.empty() ? "" : JoinList(SplitList(md.author, false)));
    line("Subject:", md.subject);
    line("Keywords:", md.keywords);
    line("  (split)", md.keywords.empty() ? "" : JoinList(SplitList(md.keywords, true)));
    line("Creator:", md.creator);
    line("Producer:", md.producer);
    line("Company:", md.company);
    line("Category:", md.category);
    line("Comments:", md.comments);
    line("Created:", md.created.ToString());
    line("Modified:", md.modified.ToString());
    if (md.pageCount >= 0) printf("%-12s %d\n", "Pages:", md.pageCount);
    line("PDF version:", md.version);
    line("Encryption:", md.encryption);
    if (md.reconstructed) printf("%-12s %s\n", "Note:", "cross-reference table reconstructed");
    if (md.hasXmp) printf("%-12s %s\n", "XMP:", "present");
    printf("%-12s %llu reads, %llu of %llu bytes\n", "I/O:", static_cast<unsigned long long>(fs.reads_),
           static_cast<unsigned long long>(fs.bytes_), static_cast<unsigned long long>(
                                                           const_cast<FileSource&>(fs).Size()));
}

void PrintJson(const char* path, const PdfMetadata& md, bool ok) {
    printf("{\"file\": \"%s\", \"ok\": %s", JsonEscape(path).c_str(), ok ? "true" : "false");
    auto str = [](const char* k, const std::string& v) { printf(", \"%s\": \"%s\"", k, JsonEscape(v).c_str()); };
    str("title", md.title);
    str("author", md.author);
    str("subject", md.subject);
    str("keywords", md.keywords);
    str("creator", md.creator);
    str("producer", md.producer);
    str("company", md.company);
    str("category", md.category);
    str("comments", md.comments);
    str("created", md.created.ToString());
    str("modified", md.modified.ToString());
    printf(", \"pages\": %d", md.pageCount);
    str("version", md.version);
    printf(", \"encrypted\": %s, \"decryptable\": %s, \"reconstructed\": %s}\n", md.encrypted ? "true" : "false",
           md.decryptable ? "true" : "false", md.reconstructed ? "true" : "false");
}

// Opens a file given as UTF-8 (on Windows through the wide API, with \\?\ for long paths).
FILE* OpenUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    auto widen = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(n > 0 ? n - 1 : 0, wchar_t());
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    };
    std::wstring wpath = widen(path), wmode = widen(mode);
    FILE* f = _wfopen(wpath.c_str(), wmode.c_str());
    if (!f) {
        wchar_t full[32768];
        DWORD n = GetFullPathNameW(wpath.c_str(), 32768, full, nullptr);
        if (n > 0 && n < 32768 && wcsncmp(full, L"\\\\", 2) != 0)
            f = _wfopen((std::wstring(L"\\\\?\\") + full).c_str(), wmode.c_str());
    }
    return f;
#else
    return fopen(path.c_str(), mode);
#endif
}

bool SetField(MetadataEdits& e, const std::string& assignment) {
    size_t eq = assignment.find('=');
    if (eq == std::string::npos) return false;
    std::string key = assignment.substr(0, eq);
    for (char& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    MetadataEdits::Field* f = key == "title"      ? &e.title
                              : key == "author"   ? &e.author
                              : key == "subject"  ? &e.subject
                              : key == "keywords" ? &e.keywords
                              : key == "comments" ? &e.comments
                              : key == "category" ? &e.category
                              : key == "company"  ? &e.company
                                                  : nullptr;
    if (!f) return false;
    f->set = true;
    f->value = assignment.substr(eq + 1);
    return true;
}

// Applies edits to `input` and writes the result to `output` (or appends in place).
int WriteMetadata(const std::string& input, const std::string& output, bool inPlace, MetadataEdits& e) {
    FILE* in = OpenUtf8(input, "rb");
    if (!in) {
        fprintf(stderr, "%s: cannot open\n", input.c_str());
        return 1;
    }
    std::string append;
    UpdateStatus st;
    {
        FileSource fs(in);
        st = BuildMetadataUpdate(fs, e, append);
    }
    if (st != UpdateStatus::kOk) {
        fclose(in);
        fprintf(stderr, "%s: cannot update metadata: %s\n", input.c_str(), UpdateStatusText(st));
        return 1;
    }
    FILE* out = nullptr;
    if (inPlace) {
        fclose(in);
        out = OpenUtf8(input, "ab");
    } else {
        out = OpenUtf8(output, "wb");
        if (out) {
            fseek(in, 0, SEEK_SET);
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
        }
        fclose(in);
    }
    if (!out) {
        fprintf(stderr, "%s: cannot write\n", inPlace ? input.c_str() : output.c_str());
        return 1;
    }
    bool ok = fwrite(append.data(), 1, append.size(), out) == append.size();
    ok = (fclose(out) == 0) && ok;
    if (!ok) {
        fprintf(stderr, "write failed\n");
        return 1;
    }
    printf("updated %s (%zu bytes appended)\n", inPlace ? input.c_str() : output.c_str(), append.size());
    return 0;
}

void Usage() {
    fprintf(stderr,
            "usage: pdfprops [--json] file.pdf...\n"
            "       pdfprops --set Key=Value [--set ...] (--output out.pdf | --in-place) file.pdf\n"
            "       Keys: Title, Author, Subject, Keywords, Comments, Category, Company (empty value removes)\n");
}

}  // namespace

int main(int argc, char** argv) {
    // Arguments as UTF-8 (on Windows, re-read the wide command line so any file name works).
    std::vector<std::string> args(argv + 1, argv + argc);
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        args.clear();
        for (int i = 1; i < wargc; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(n > 0 ? n - 1 : 0, char());
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &a[0], n, nullptr, nullptr);
            args.push_back(a);
        }
        LocalFree(wargv);
    }
#endif
    bool json = false, inPlace = false;
    std::string output;
    MetadataEdits edits;
    std::vector<std::string> files;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json") {
            json = true;
        } else if (a == "--in-place") {
            inPlace = true;
        } else if ((a == "--set" || a == "--output") && i + 1 < args.size()) {
            if (a == "--output") {
                output = args[++i];
            } else if (!SetField(edits, args[++i])) {
                fprintf(stderr, "bad --set argument: %s\n", args[i].c_str());
                Usage();
                return 2;
            }
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) {
        Usage();
        return 2;
    }
    if (edits.Any() || inPlace || !output.empty()) {
        if (!edits.Any() || files.size() != 1 || inPlace == !output.empty()) {
            Usage();
            return 2;
        }
        edits.nowUnix = static_cast<int64_t>(time(nullptr));
        return WriteMetadata(files[0], output, inPlace, edits);
    }
    int rc = 0;
    for (const auto& file : files) {
        const char* path = file.c_str();
        FILE* f = OpenUtf8(file, "rb");
        if (!f) {
            fprintf(stderr, "%s: cannot open\n", path);
            rc = 1;
            continue;
        }
        FileSource fs(f);
        PdfMetadata md;
        bool ok = ExtractMetadata(fs, md);
        fclose(f);
        if (json) {
            PrintJson(path, md, ok);
        } else if (ok) {
            PrintText(path, md, fs);
            printf("\n");
        } else {
            printf("File:        %s\n(not a PDF)\n\n", path);
        }
        if (!ok) rc = 1;
    }
    return rc;
}
