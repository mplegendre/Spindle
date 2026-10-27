/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU Lesser General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#if !defined(USE_PLUGIN_DEBUG)
#include "spindle_debug.h"
#else
#include "plugin_utils.h"
#define SPINDLE_DEBUG_H_
#endif

#include "ldcs_api.h"
#include "config.h"
#include "spindle_debug.h"

#if defined(TRACK_MKDIR)
extern void track_mkdir(const char *dir);
#endif
#if defined(LOOKUP_PREV_MKDIR)
extern int lookup_prev_mkdir(const char *dir);
#endif

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Keep filesystem calls in one place for the startup replacement layer. */
static int spindle_mkdir_stat(const char *path, struct stat *buf)
{
   return stat(path, buf);
}

static int spindle_mkdir_lstat(const char *path, struct stat *buf)
{
   return lstat(path, buf);
}

static ssize_t spindle_mkdir_readlink(const char *path, char *buf, size_t size)
{
   return readlink(path, buf, size);
}

static int spindle_mkdir_mkdir(const char *path, mode_t mode)
{
   return mkdir(path, mode);
}

static int within(const char *path, const char *prefix, size_t length)
{
   return !strncmp(path, prefix, length) &&
      (length == 1 || path[length] == '/' || path[length] == '\0');
}

static int append(char *path, size_t *length, const char *part, size_t size)
{
   size_t separator = !(*length == 1 && path[0] == '/');

   if (*length + separator > PATH_MAX - 1 ||
       size > PATH_MAX - 1 - *length - separator) {
      errno = ENAMETOOLONG;
      debug_printf3("WARNING: apend failing with ENAMETOLONG on '%s' and '%s'\n", path, part);
      return -1;
   }
   if (separator)
      path[(*length)++] = '/';
   memcpy(path + *length, part, size);
   *length += size;
   path[*length] = '\0';
   return 0;
}

/* Move up from a resolved path. Relative paths retain leading '..' because
 * the current directory cannot be named absolutely with the allowed calls. */
static int parent(char path[PATH_MAX], size_t *length)
{
   if (path[0] != '/' &&
       ((*length == 1 && path[0] == '.') ||
        (*length >= 3 && path[*length - 3] == '/' &&
         path[*length - 2] == '.' && path[*length - 1] == '.')))
      return append(path, length, "..", 2);

   while (*length > 1 && path[*length - 1] != '/')
      --*length;
   if (*length > 1)
      --*length;
   path[*length] = '\0';
   return 0;
}

/* Resolve suffix onto path in place. path starts at "/", ".", or the already
 * resolved prefix; pending holds components not yet applied to path.
 *
 * Symlinks are expanded before later '..' components. With floor, '..' stops
 * at the prefix and symlink targets reached inside it must stay inside it.
 * required is set only while resolving the prefix, which must already exist. */
static int resolve(const char *suffix, const char *floor, int required,
                   char path[PATH_MAX])
{
   char pending[PATH_MAX], target[PATH_MAX];
   size_t length = strlen(path), floor_length = floor ? strlen(floor) : 0;
   size_t cursor = 0, pending_length = strlen(suffix);
   int links = 0;

   if (pending_length >= sizeof pending) {
      errno = ENAMETOOLONG;
      debug_printf3("WARNING: resolve ENAMETOLONG on '%s'\n", suffix);
      return -1;
   }
   /* Expansion can insert NUL separators, so pending_length, not strlen,
    * defines the part of pending still in use. */
   memcpy(pending, suffix, pending_length);

   while (cursor < pending_length) {
      size_t start, end, old_length;
      size_t boundary;
      struct stat st;
      ssize_t n;

      /* Skip empty components. An inserted NUL ends one symlink target;
       * check it before applying the caller's remaining components. */
      while (cursor < pending_length && pending[cursor] == '/')
         ++cursor;
      if (cursor == pending_length)
         break;
      /* A NUL marks the end of a symlink target, before its caller's tail. */
      if (pending[cursor] == '\0') {
         if (floor && !within(path, floor, floor_length)) {
            debug_printf3("WARNING within failing with EACCES on '%s' in '%s'\n", path, floor);
            errno = EACCES;
            return -1;
         }
         ++cursor;
         continue;
      }
      start = cursor;
      while (cursor < pending_length && pending[cursor] && pending[cursor] != '/')
         ++cursor;
      end = cursor;
      if (end - start == 1 && pending[start] == '.')
         continue;
      if (end - start == 2 && pending[start] == '.' && pending[start + 1] == '.') {
         /* Clamp at floor, just as '..' at '/' stays at '/'. */
         if (!floor || !within(path, floor, floor_length) ||
             length > floor_length) {
            if (parent(path, &length) < 0) {
               debug_printf3("resolv failing because could not determine parent of '%s' with floor '%s'\n",
                             path ? path : "[NULL]", floor ? floor : "[NULL]");
               return -1;
            }
         }
         continue;
      }

      old_length = length;
      if (append(path, &length, pending + start, end - start) < 0)
         return -1;
      /* Missing components are kept in the resolved path for the later
       * mkdir pass. Existing non-directories cannot be traversed. */
      if (spindle_mkdir_lstat(path, &st) < 0) {
         if (errno == ENOENT && !required)
            continue;
         debug_printf3("lstat failed during resolve for '%s'\n", path);
         return -1;
      }
      if (!S_ISLNK(st.st_mode)) {
         if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            debug_printf3("resolve failing for '%s' because ENOTDIR\n", path);
            return -1;
         }
         continue;
      }
      /* Check targets of links inside the prefix, even if the caller's
       * remaining path would later return inside. Links in a prefix alias
       * may resolve to an ancestor, so they get no boundary check here. */
      boundary = floor && within(path, floor, floor_length);
      if (++links > 40) {
         errno = ELOOP;
         debug_printf3("resolve failing for '%s' because ELOOP\n", path);
         return -1;
      }
      n = spindle_mkdir_readlink(path, target, sizeof target - 1);
      if (n < 0) {
         debug_printf3("resolve failing for '%s' because readlink failed\n", path);         
         return -1;
      }
      if (n == 0) {
         errno = ENOENT;
         debug_printf3("resolve failing for '%s' with ENOENT\n", path);
         return -1;
      }
      if ((size_t)n + boundary + pending_length - end >= sizeof pending) {
         errno = ENAMETOOLONG;
         debug_printf3("resolve failing for '%s' with ENAMETOOLONG\n", path);
         return -1;
      }
      /* Replace the link with its target and keep the unread tail. The
       * optional NUL marks where this target ends. Nested expansions keep
       * earlier markers in that tail without a separate stack. */
      memmove(pending + n + boundary, pending + end, pending_length - end);
      memcpy(pending, target, n);
      if (boundary)
         pending[n] = '\0';
      pending_length = (size_t)n + boundary + pending_length - end;
      cursor = 0;
      /* Absolute targets restart at root; relative targets restart at the
       * link's parent, saved in old_length. */
      if (target[0] == '/') {
         path[0] = '/';
         path[1] = '\0';
         length = 1;
      } else {
         length = old_length;
         path[length] = '\0';
      }
   }
   /* Also reject an ordinary path that ends outside the prefix. */
   if (floor && !within(path, floor, floor_length)) {
      errno = EACCES;
      debug_printf3("resolve failing for '%s' with EACCESs and floor of %s\n", path, floor);
      return -1;
   }
   return 0;
}

/* Leading relative '..' components name ancestors, not directories to make. */
static size_t creation_start(const char *path)
{
   size_t i = 0;

   while (path[i] == '.') {
      size_t dots = path[i + 1] == '.' ? 2 : 1;
      if (path[i + dots] != '/' && path[i + dots] != '\0')
         break;
      i += dots;
      if (path[i] == '/')
         ++i;
   }
   return i;
}

static int check_directory(const char *path)
{
   struct stat st;

   if (spindle_mkdir_stat(path, &st) < 0) {
      debug_printf3("WARNING: Failed to stat %s\n", path);
      return -1;
   }
   if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      debug_printf3("WARNING: %s is not a directory\n", path);
      return -1;
   }
   if (st.st_uid != geteuid() || st.st_gid != getegid() ||
       (st.st_mode & 07777) != 0700) {
      errno = EACCES;
      debug_printf3("WARNING: Permissions error on %s\n", path);
      return -1;
   }
   return 0;
}

/* EEXIST may come from another process; verify what is there either way. */
static int make_directory(const char *path, int should_check_perms)
{
   int result;
   if (spindle_mkdir_mkdir(path, 0700) < 0 && errno != EEXIST) {
      debug_printf3("WARNING: Failed to mkdir(%s)\n", path);
      return -1;
   }
   result = check_directory(path);
   if (result == -1) {
      debug_printf3("WARNING: check_directory failed here\n");
      return -1;
   }
#if defined(TRACK_MKDIR)
   track_mkdir(path);
#endif
   return result;
}

static int spindle_mkdir_impl(const char *path, const char *cachepath)
{
   char resolved[PATH_MAX], floor[PATH_MAX], literal[PATH_MAX];
   size_t prefix_length, physical_length, length, i;

   if (!path || !*path || (cachepath && !*cachepath)) {
      debug_printf3("WARNING: spindle_mkdir failing with EINVAL on '%s' with cachepath '%s'\n",
                    path ? path : "[NULL]", cachepath ? cachepath : "[NULL]");
      errno = EINVAL;
      return -1;
   }
   if (cachepath) {
      /* Resolve the existing prefix to establish a physical boundary. */
      if (cachepath[0] != '/' || path[0] != '/') {
         debug_printf3("WARNING: spindle_mkdir failing with EINVAL on '%s' and '%s'\n",
                    path ? path : "[NULL]", cachepath ? cachepath : "[NULL]");
         errno = EINVAL;
         return -1;
      }
      prefix_length = strlen(cachepath);
      while (prefix_length > 1 && cachepath[prefix_length - 1] == '/')
         --prefix_length;
      if (prefix_length >= PATH_MAX) {
         debug_printf3("WARNING: spindle_mkdir failing with ENAMETOOLONG on '%s'\n",
                       path ? path : "[NULL]");
         errno = ENAMETOOLONG;
         return -1;
      }
      if (!within(path, cachepath, prefix_length)) {
         debug_printf3("WARNING: spindle_mkdir failing with EACCESS on '%s' and '%s'\n",
                       path ? path : "[NULL]", cachepath ? cachepath : "[NULL]");
         errno = EACCES;
         return -1;
      }
      strcpy(resolved, "/");
      if (resolve(cachepath, NULL, 1, resolved) < 0) {
         debug_printf3("WARNING: resolve failed here\n");
         return -1;
      }
      if (check_directory(resolved) < 0) {
         debug_printf3("WARNING: resolve failed here\n");
         return -1;
      }

      strcpy(floor, resolved);
      physical_length = strlen(resolved);
      /* Resolve below that boundary, then map back to the literal prefix
       * so every mkdir call uses the spelling required by cleanup. */
      if (resolve(path + prefix_length, floor, 0, resolved) < 0)
         return -1;
      length = strlen(resolved) - physical_length;
      if (length > PATH_MAX - 1 - prefix_length) {
         debug_printf3("WARNING: spindle_mkdir failing with ENAMETOOLONG on '%s'\n",
                       resolved ? resolved : "[NULL]");
         errno = ENAMETOOLONG;
         return -1;
      }
      memcpy(literal, cachepath, prefix_length);
      memcpy(literal + prefix_length, resolved + physical_length, length + 1);
      length += prefix_length;
      i = prefix_length;
   } else {
      /* Relative paths stay relative; absolute paths start at root. */
      strcpy(resolved, path[0] == '/' ? "/" : ".");
      if (resolve(path, NULL, 0, resolved) < 0) {
         debug_printf3("WARNING: resolve failed here\n");         
         return -1;
      }
      length = strlen(resolved);
      memcpy(literal, resolved, length + 1);
      i = creation_start(literal);
   }

   /* Walk the resolved path once, creating and verifying each directory. */
   while (i < length) {
      int should_check_perms;
      size_t end, next;
      char saved;

      while (i < length && literal[i] == '/')
         ++i;
      if (i == length)
         break;
      end = i;
      while (end < length && literal[end] != '/')
         ++end;
      next = end;
      while (next < length && literal[next] == '/')
         ++next;
      saved = literal[end];
      literal[end] = '\0';
      /* Public and system-owned directories may lead to an unprefixed
       * path. Directories managed by this function must be private. */
      if (!cachepath && next < length) {
         struct stat st;
         if (spindle_mkdir_stat(literal, &st) == 0 && S_ISDIR(st.st_mode)) {
            literal[end] = saved;
            i = end;
            continue;
         }
      }

      /* Only check permissions on the parent when making withinin the cachepath.
         When making cachepath or commpaths we may be in some other directories */
      should_check_perms = (cachepath != NULL);
      if (make_directory(literal, should_check_perms) < 0) {
         debug_printf3("WARNING: make_directory failed here\n");
         return -1;
      }
      literal[end] = saved;
      i = end;
   }
   return 0;
}

int spindle_mkdir(const char *path)
{
   return spindle_mkdir_impl(path, NULL);
}

int spindle_mkdir_in_cache(const char *path, const char *cachepath)
{
   return spindle_mkdir_impl(path, cachepath);
}
