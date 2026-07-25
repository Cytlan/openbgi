/*
 * platform_detection.h
 *
 * A portable, single-header C/C++ preprocessor utility for compile-time detection of:
 *   - Compiler type and version (GCC, Clang/Apple Clang/Emscripten, MSVC, MinGW-w64)
 *   - Operating system / platform (Windows, macOS, Linux/Android, FreeBSD, other BSDs, WebAssembly)
 *   - CPU architecture (x86, x86_64, ARM, ARM64, wasm32/64)
 *
 * Designed for the most common modern compilers and desktop/server/WASM platforms.
 * Excludes specialized/embedded (e.g. microcontrollers) and ancient compilers.
 *
 * Usage example:
 *   #include "compiler_platform_detection.h"
 *
 *   #if defined(COMPILER_CLANG)
 *       // Clang-specific code or optimizations
 *   #endif
 *
 *   printf("Built with %s version %d.%d.%d on %s (%s)\n",
 *          COMPILER_NAME,
 *          COMPILER_VERSION_MAJOR, COMPILER_VERSION_MINOR, COMPILER_VERSION_PATCH,
 *          PLATFORM_NAME, ARCH_NAME);
 *
 *   #if defined(PLATFORM_WINDOWS) && defined(ARCH_X86_64)
 *       // Windows x64 specific
 *   #endif
 *
 *   // Version checks (example)
 *   #if defined(COMPILER_GCC) && (COMPILER_VERSION_MAJOR > 11 || \
 *       (COMPILER_VERSION_MAJOR == 11 && COMPILER_VERSION_MINOR >= 3))
 *       // GCC 11.3+
 *   #endif
 */

#ifndef COMPILER_PLATFORM_DETECTION_H
#define COMPILER_PLATFORM_DETECTION_H

/* ============================================
 *           STRINGIFICATION HELPERS
 * ============================================ */

#define COMPILER_STRINGIFY_HELPER(x) #x
#define COMPILER_STRINGIFY(x)        COMPILER_STRINGIFY_HELPER(x)

/* ============================================
 *           COMPILER DETECTION
 * ============================================ */

#if defined(__clang__)
/*
 * Clang (including Apple Clang from Xcode, Emscripten, and clang-cl on Windows).
 * Note: Clang defines __GNUC__ for compatibility, so this check MUST come before GCC.
 */
#  define COMPILER_CLANG
#  define COMPILER_NAME                "Clang"
#  define COMPILER_VERSION_MAJOR       __clang_major__
#  define COMPILER_VERSION_MINOR       __clang_minor__
#  define COMPILER_VERSION_PATCH       __clang_patchlevel__
#  define COMPILER_VERSION_STRING      "Clang " \
                                       COMPILER_STRINGIFY(__clang_major__) "." \
                                       COMPILER_STRINGIFY(__clang_minor__) "." \
                                       COMPILER_STRINGIFY(__clang_patchlevel__)

#  if defined(__apple_build_version__)
     /* Apple Clang (Xcode's compiler on macOS/iOS) */
#    define COMPILER_APPLE_CLANG
#    define COMPILER_APPLE_BUILD_VERSION __apple_build_version__
#    undef  COMPILER_NAME
#    define COMPILER_NAME              "Apple Clang"
     /* The __clang_*__ versions reflect the underlying Clang/LLVM version (recommended for feature checks) */
#  endif

#  if defined(__EMSCRIPTEN__)
     /* Emscripten (compiles to WebAssembly/asm.js) */
#    define COMPILER_EMSCRIPTEN
#    undef  COMPILER_NAME
#    define COMPILER_NAME              "Emscripten (Clang-based)"
#  endif

#elif defined(__GNUC__)
/*
 * GCC or GCC-compatible compilers.
 * This also catches MinGW/MinGW-w64 (which are GCC ports to Windows).
 */
#  define COMPILER_GCC
#  define COMPILER_NAME                "GCC"
#  define COMPILER_VERSION_MAJOR       __GNUC__
#  define COMPILER_VERSION_MINOR       __GNUC_MINOR__
#  define COMPILER_VERSION_PATCH       __GNUC_PATCHLEVEL__
#  define COMPILER_VERSION_STRING      "GCC " \
                                       COMPILER_STRINGIFY(__GNUC__) "." \
                                       COMPILER_STRINGIFY(__GNUC_MINOR__) "." \
                                       COMPILER_STRINGIFY(__GNUC_PATCHLEVEL__)

#  if defined(__MINGW32__) || defined(__MINGW64__)
     /* MinGW or MinGW-w64 (GCC on Windows) */
#    define COMPILER_MINGW
#    if defined(__MINGW64__)
#      define COMPILER_MINGW64
#      undef  COMPILER_NAME
#      define COMPILER_NAME            "MinGW-w64 (GCC)"
#    else
#      define COMPILER_MINGW32
#      undef  COMPILER_NAME
#      define COMPILER_NAME            "MinGW (GCC)"
#    endif
#  endif

#elif defined(_MSC_VER)
/*
 * Microsoft Visual C++ compiler (MSVC).
 * Note: clang-cl (Clang in MSVC compatibility mode) defines both __clang__ and _MSC_VER,
 * but is correctly detected as Clang because __clang__ is checked first.
 */
#  define COMPILER_MSVC
#  define COMPILER_NAME                "MSVC"
#  define COMPILER_VERSION_MAJOR       (_MSC_VER / 100)
#  define COMPILER_VERSION_MINOR       (_MSC_VER % 100)
#  define COMPILER_VERSION_PATCH       0
#  define COMPILER_VERSION_STRING      "MSVC " COMPILER_STRINGIFY(_MSC_VER)
   /* Full version with build number for advanced use */
#  define COMPILER_MSVC_FULL_VER       _MSC_FULL_VER

#else
/* Fallback for unknown or rare compilers */
#  define COMPILER_UNKNOWN
#  define COMPILER_NAME                "Unknown"
#  define COMPILER_VERSION_MAJOR       0
#  define COMPILER_VERSION_MINOR       0
#  define COMPILER_VERSION_PATCH       0
#  define COMPILER_VERSION_STRING      "Unknown compiler"
#endif

/* ============================================
 *           PLATFORM / OS DETECTION
 * ============================================ */

#if defined(_WIN32) || defined(_WIN64)
   /* Windows (covers MSVC, MinGW, clang-cl, etc.) */
#  define PLATFORM_WINDOWS
#  define PLATFORM_NAME                "Windows"
#  if defined(_WIN64)
#    define PLATFORM_64BIT
#  else
#    define PLATFORM_32BIT
#  endif

#elif defined(__APPLE__) && defined(__MACH__)
   /* macOS / Darwin (includes macOS on Apple Silicon and Intel) */
#  define PLATFORM_MACOS
#  define PLATFORM_NAME                "macOS"

#elif defined(__linux__)
   /* Linux (includes most desktop/server distros) */
#  define PLATFORM_LINUX
#  define PLATFORM_NAME                "Linux"
#  if defined(__ANDROID__)
     /* Android NDK (Linux-based, usually Clang) */
#    define PLATFORM_ANDROID
#    undef  PLATFORM_NAME
#    define PLATFORM_NAME              "Android"
#  endif

#elif defined(__FreeBSD__)
#  define PLATFORM_FREEBSD
#  define PLATFORM_NAME                "FreeBSD"

#elif defined(__NetBSD__)
#  define PLATFORM_NETBSD
#  define PLATFORM_NAME                "NetBSD"

#elif defined(__OpenBSD__)
#  define PLATFORM_OPENBSD
#  define PLATFORM_NAME                "OpenBSD"

#elif defined(__EMSCRIPTEN__) || defined(__wasm__) || defined(__wasm32__) || defined(__wasm64__)
   /* WebAssembly target (Emscripten or bare WASI Clang, etc.) */
#  define PLATFORM_WASM
#  define PLATFORM_NAME                "WebAssembly"

#else
#  define PLATFORM_UNKNOWN
#  define PLATFORM_NAME                "Unknown"
#endif

/* ============================================
 *           ARCHITECTURE DETECTION
 * ============================================ */

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#  define ARCH_X86_64
#  define ARCH_NAME                    "x86_64"

#elif defined(__i386__) || defined(_M_IX86)
#  define ARCH_X86
#  define ARCH_NAME                    "x86"

#elif defined(__aarch64__) || defined(_M_ARM64)
   /* ARM 64-bit (Apple Silicon, AWS Graviton, etc.) */
#  define ARCH_ARM64
#  define ARCH_NAME                    "arm64"

#elif defined(__arm__) || defined(_M_ARM)
#  define ARCH_ARM
#  define ARCH_NAME                    "arm"

#elif defined(__wasm32__)
#  define ARCH_WASM32
#  define ARCH_NAME                    "wasm32"

#elif defined(__wasm64__)
#  define ARCH_WASM64
#  define ARCH_NAME                    "wasm64"

#else
#  define ARCH_UNKNOWN
#  define ARCH_NAME                    "Unknown"
#endif

/* ============================================
 *           END OF HEADER
 * ============================================ */

#endif /* COMPILER_PLATFORM_DETECTION_H */
