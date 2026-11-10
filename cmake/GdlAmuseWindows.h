#pragma once

// The pinned Amuse Common.hpp uses CRT filesystem declarations without their
// Windows headers. Supply declarations only; filename conversion remains the
// real standalone Boost.Nowide implementation.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#ifndef S_ISREG
// NOLINTNEXTLINE(readability-identifier-naming): the POSIX name the Amuse sources call
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
// NOLINTNEXTLINE(readability-identifier-naming): the POSIX name the Amuse sources call
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#endif
