# Build with MSYS2 (mingw64 or ucrt64) g++:  mingw32-make   (or: make)
# Targets: all (default), cli, dll, selftest, handlertest, test, fuzz, clean
#          x86, test-x86: 32-bit DLL + test host in $(OUT)/x86 (needs mingw-w64-i686-gcc,
#          i.e. i686-w64-mingw32-g++ on PATH, or pass CXX32=/path/to/g++)

CXX      = g++
WINDRES  = windres
OUT      ?= build
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDSTATIC  = -static -static-libgcc -static-libstdc++ -s
PYTHON   ?= python
CXX32     = i686-w64-mingw32-g++
WINDRES32 = windres -F pe-i386

CORE_SRC = src/core/Crypto.cpp src/core/Inflate.cpp src/core/PdfParser.cpp src/core/PdfSecurity.cpp \
           src/core/PdfMetadata.cpp src/core/PdfWriter.cpp
CORE_HDR = $(wildcard src/core/*.h)
DLL_SRC  = src/handler/dllmain.cpp src/handler/PdfPropertyHandler.cpp src/handler/Registration.cpp
DLL_HDR  = src/handler/Common.h

.PHONY: all cli dll selftest handlertest test fuzz clean x86 test-x86

all: cli dll selftest handlertest

cli: $(OUT)/pdfprops.exe
dll: $(OUT)/zPDFPropertyHandler.dll
selftest: $(OUT)/crypto_selftest.exe
handlertest: $(OUT)/handler_test.exe

$(OUT):
	mkdir -p $(OUT)

$(OUT)/pdfprops.exe: src/cli/pdfprops.cpp $(CORE_SRC) $(CORE_HDR) | $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ src/cli/pdfprops.cpp $(CORE_SRC) $(LDSTATIC)

$(OUT)/version.o: src/handler/version.rc | $(OUT)
	$(WINDRES) $< -O coff -o $@

$(OUT)/zPDFPropertyHandler.dll: $(DLL_SRC) $(DLL_HDR) $(CORE_SRC) $(CORE_HDR) src/handler/zPDFPropertyHandler.def $(OUT)/version.o
	$(CXX) $(CXXFLAGS) -shared -o $@ $(DLL_SRC) $(CORE_SRC) $(OUT)/version.o src/handler/zPDFPropertyHandler.def \
	    $(LDSTATIC) -Wl,--kill-at,--enable-stdcall-fixup -lpropsys -lole32 -loleaut32 -luuid -lshlwapi -lshell32 -ladvapi32

$(OUT)/crypto_selftest.exe: tests/crypto_selftest.cpp src/core/Crypto.cpp src/core/Crypto.h | $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ tests/crypto_selftest.cpp src/core/Crypto.cpp $(LDSTATIC)

$(OUT)/handler_test.exe: tests/handler_test.cpp | $(OUT)
	$(CXX) $(CXXFLAGS) -municode -o $@ $< $(LDSTATIC) -lpropsys -lole32 -luuid -lshlwapi

test: cli dll selftest handlertest
	$(OUT)/crypto_selftest.exe
	$(PYTHON) tests/make_samples.py
	$(PYTHON) tests/run_tests.py $(OUT)/pdfprops.exe
	$(OUT)/handler_test.exe $(OUT)/zPDFPropertyHandler.dll tests/samples/*.pdf
	$(PYTHON) tests/write_tests.py $(OUT)/pdfprops.exe

x86:
	$(MAKE) dll handlertest CXX="$(CXX32)" WINDRES="$(WINDRES32)" OUT=$(OUT)/x86

# Run after "test" (which generates tests/samples)
test-x86: x86
	$(OUT)/x86/handler_test.exe $(OUT)/x86/zPDFPropertyHandler.dll tests/samples/*.pdf

fuzz: cli
	$(PYTHON) tests/fuzz.py 100

clean:
	rm -rf $(OUT)
