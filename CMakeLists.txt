# Alternative to the Makefile, mainly for MSVC:
#   cmake -S . -B build-msvc -A x64 && cmake --build build-msvc --config Release
cmake_minimum_required(VERSION 3.15)
project(zPDFPropertyHandler CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra)
    add_link_options(-static -static-libgcc -static-libstdc++)
endif()

add_library(pdfcore STATIC
    src/core/Crypto.cpp src/core/Inflate.cpp src/core/PdfParser.cpp
    src/core/PdfSecurity.cpp src/core/PdfMetadata.cpp src/core/PdfWriter.cpp)

add_executable(pdfprops src/cli/pdfprops.cpp)
target_link_libraries(pdfprops PRIVATE pdfcore shell32)

add_executable(crypto_selftest tests/crypto_selftest.cpp src/core/Crypto.cpp)

add_library(zPDFPropertyHandler SHARED
    src/handler/dllmain.cpp src/handler/PdfPropertyHandler.cpp src/handler/Registration.cpp
    src/handler/version.rc src/handler/zPDFPropertyHandler.def)
set_target_properties(zPDFPropertyHandler PROPERTIES PREFIX "")
target_link_libraries(zPDFPropertyHandler PRIVATE pdfcore propsys ole32 oleaut32 uuid shlwapi shell32 advapi32)

add_executable(handler_test tests/handler_test.cpp)
target_link_libraries(handler_test PRIVATE propsys ole32 uuid shlwapi)
if(MSVC)
    target_link_options(handler_test PRIVATE /ENTRY:wmainCRTStartup)
else()
    target_link_options(handler_test PRIVATE -municode)
endif()
