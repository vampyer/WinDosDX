/*
 *  Copyright (C) 2002-2010  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * WinDosDX forced-include prelude for the DOSBox machine core.
 *
 * The core is POSIX-flavoured C++ from 2009 being built against a 2022
 * MSVC targeting ReactOS's C runtime.  Rather than patch the vendored sources
 * for each mismatch, this header is force-included into every DOSBox
 * translation unit (see the /FI flag in windos_core/CMakeLists.txt) and fixes
 * the differences in one place:
 *
 *  1. ReactOS's crt/math.h defines the C++ floating-point overloads itself,
 *     which collide with the ones stlport's stl/_cmath.h adds.  Defining
 *     _CMATH_ before it is reached suppresses the SDK's copies, and stlport
 *     supplies the standard set instead.
 *
 *  2. The core calls the POSIX spellings stricmp/getcwd/mkdir/access/unlink
 *     and uses S_IFDIR/stat.  ReactOS's CRT provides the _-prefixed names and
 *     _S_IFDIR, so the POSIX spellings are mapped onto them.
 *
 *  3. The CD-ROM sources use the FRAMES_TO_MSF / MSF_TO_FRAMES macros that
 *     DOSBox took from SDL_cdrom.h.
 */

#ifndef WINDOS_MSVC_PRELUDE_H
#define WINDOS_MSVC_PRELUDE_H

/* (1) Claim the C++ math overloads before crt/math.h sees them. */
#ifndef _CMATH_
#define _CMATH_
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

/*
 * The core calls std::toupper / std::tolower (misc/support.cpp).  stlport
 * imports the C library names into std only when it owns a separate namespace,
 * which ReactOS's configuration disables, so they are declared here instead.
 */
namespace std {
    using ::tolower;
    using ::toupper;
}

/* (2) POSIX spellings used by the core, mapped to the ReactOS CRT.
 * strcasecmp/strncasecmp are already handled by DOSBox's own include/support.h
 * under _MSC_VER, so only the spellings it does not cover are mapped here.
 * `stat` is deliberately NOT redirected: ReactOS declares both stat() and
 * fstat() against `struct stat`, with an inline shim down to _fstat().
 */
#undef stricmp
#define stricmp     _stricmp
#undef getcwd
#define getcwd      _getcwd
#undef chdir
#define chdir       _chdir
#undef mkdir
#define mkdir(p)    _mkdir(p)
#undef rmdir
#define rmdir       _rmdir
#undef unlink
#define unlink      _unlink
#undef access
#define access      _access
#ifndef S_IFDIR
#define S_IFDIR     _S_IFDIR
#endif
#ifndef S_IFREG
#define S_IFREG     _S_IFREG
#endif
#ifndef S_IFMT
#define S_IFMT      _S_IFMT
#endif

/* (3) CD-ROM frame/minute/second/frame conversions, as in SDL_cdrom.h. */
#define FRAMES_TO_MSF(f, m, s, ff)          \
    do {                                    \
        int _f = (int)(f);                  \
        *(m) = (unsigned char)(_f / (75 * 60)); \
        _f -= (*(m)) * (75 * 60);           \
        *(s) = (unsigned char)(_f / 75);    \
        _f -= (*(s)) * 75;                  \
        *(ff) = (unsigned char)_f;          \
    } while (0)

#define MSF_TO_FRAMES(m, s, f)              \
    ((m) * 75 * 60 + (s) * 75 + (f))

#endif /* WINDOS_MSVC_PRELUDE_H */
