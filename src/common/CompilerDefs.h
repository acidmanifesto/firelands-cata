/*
 * This file is part of the FirelandsCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FC_COMPILERDEFS_H
#define FC_COMPILERDEFS_H

#define FC_PLATFORM_WINDOWS 0
#define FC_PLATFORM_UNIX    1
#define FC_PLATFORM_APPLE   2
#define FC_PLATFORM_INTEL   3

// must be first (win 64 also define _WIN32)
#if defined( _WIN64 )
#  define FC_PLATFORM FC_PLATFORM_WINDOWS
#elif defined( __WIN32__ ) || defined( WIN32 ) || defined( _WIN32 )
#  define FC_PLATFORM FC_PLATFORM_WINDOWS
#elif defined( __APPLE_CC__ )
#  define FC_PLATFORM FC_PLATFORM_APPLE
#elif defined( __INTEL_COMPILER )
#  define FC_PLATFORM FC_PLATFORM_INTEL
#else
#  define FC_PLATFORM FC_PLATFORM_UNIX
#endif

#define FC_COMPILER_MICROSOFT 0
#define FC_COMPILER_GNU       1
#define FC_COMPILER_BORLAND   2
#define FC_COMPILER_INTEL     3

#ifdef _MSC_VER
#  define FC_COMPILER FC_COMPILER_MICROSOFT
#elif defined( __BORLANDC__ )
#  define FC_COMPILER FC_COMPILER_BORLAND
#elif defined( __INTEL_COMPILER )
#  define FC_COMPILER FC_COMPILER_INTEL
#elif defined( __GNUC__ )
#  define FC_COMPILER FC_COMPILER_GNU
#  define GCC_VERSION (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#else
#  error "FATAL ERROR: Unknown compiler."
#endif

#endif
