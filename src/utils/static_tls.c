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

#include "static_tls.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

static int strcmp_w_null(const char *a, const char *b) 
{
    return strcmp(a ? a : "", b ? b : "");
}
int static_tls_info_equal(const static_tls_info_t *info_a, const static_tls_info_t *info_b)
{
    return (strcmp_w_null(info_a->executable, info_b->executable) == 0) &&
        (strcmp_w_null(info_a->ld_library_path, info_b->ld_library_path) == 0) &&
        (strcmp_w_null(info_a->ld_preload, info_b->ld_preload) == 0) &&
        (strcmp_w_null(info_a->cwd, info_b->cwd) == 0) ? 1 : 0;
}

int static_tls_info_encode_size_needed(const char *executable, const char *ld_library_path, const char *ld_preload, const char *cwd)
{
    return (int) (sizeof(int)*4 + 
        (executable ? strlen(executable) + 1 : 0) + 
        (ld_library_path ? strlen(ld_library_path) + 1 : 0) + 
        (ld_preload ? strlen(ld_preload) + 1 : 0) + 
        (cwd ? strlen(cwd) + 1 : 0) + 1);
}

int static_tls_info_encode_size_needed2(const static_tls_info_t *info)
{
    return static_tls_info_encode_size_needed(info->executable, info->ld_library_path, info->ld_preload, info->cwd); 
}

int static_tls_info_encode(const char *executable, const char *ld_library_path, const char *ld_preload, const char *cwd, char *buffer, int buffer_in_size, int *buffer_out_size)
{
    int executable_len, library_len, preload_len, cwd_len;
    int len, cur = 0;

    executable_len = executable ? strlen(executable) + 1 : 0;
    library_len = ld_library_path ? strlen(ld_library_path) + 1 : 0;
    preload_len = ld_preload ? strlen(ld_preload) + 1 : 0;
    cwd_len = cwd ? strlen(cwd) + 1 : 0;

    len = sizeof(int)*4 + executable_len + library_len + preload_len + cwd_len + 1;
    assert(buffer_in_size <= len);

    memcpy(buffer+cur, &executable_len, sizeof(executable_len));
    cur += sizeof(executable_len);
   
    memcpy(buffer+cur, &library_len, sizeof(library_len));
    cur += sizeof(library_len);

    memcpy(buffer+cur, &preload_len, sizeof(preload_len));
    cur += sizeof(preload_len);

    memcpy(buffer+cur, &cwd_len, sizeof(cwd_len));
    cur += sizeof(cwd_len);

    if (executable) {
        memcpy(buffer+cur, executable, executable_len);
        cur += executable_len;
    }
   
    if (ld_library_path) {
        memcpy(buffer+cur, ld_library_path, library_len);
        cur += library_len;
    }
    if (ld_preload) {
        memcpy(buffer+cur, ld_preload, preload_len);
        cur += preload_len;
    }
    if (cwd) {
        memcpy(buffer+cur, cwd, cwd_len);
        cur += cwd_len;
    }

    *buffer_out_size = cur;
    return 0;
}

int static_tls_info_encode2(const static_tls_info_t *info, char *buffer, int buffer_in_size, int *buffer_out_size)
{
    return static_tls_info_encode(info->executable, info->ld_library_path, info->ld_preload, info->cwd, buffer, buffer_in_size, buffer_out_size);
}

int static_tls_info_decode(char *buffer, int buffer_len, static_tls_info_t **info)
{
    int executable_len, library_len, preload_len, cwd_len;
    int cur = 0;
    static_tls_info_t *newinfo;

    *info = NULL;
    if (buffer_len < sizeof(int) * 4) 
        return -1;
   
    memcpy(&executable_len, buffer + cur, sizeof(executable_len));
    cur += sizeof(executable_len);
    memcpy(&library_len, buffer + cur, sizeof(library_len));
    cur += sizeof(library_len);
    memcpy(&preload_len, buffer + cur, sizeof(preload_len));
    cur += sizeof(preload_len);   
    memcpy(&cwd_len, buffer + cur, sizeof(cwd_len));
    cur += sizeof(cwd_len);

    if (buffer_len < cur + executable_len + library_len + preload_len + cwd_len)
        return -1;

    newinfo = (static_tls_info_t *) malloc(sizeof(static_tls_info_t));

    newinfo->executable = executable_len ? strdup(buffer + cur) : NULL;
    cur += executable_len;
    newinfo->ld_library_path = library_len ? strdup(buffer + cur) : NULL;
    cur += library_len;
    newinfo->ld_preload = preload_len ? strdup(buffer + cur) : NULL;
    cur += preload_len;
    newinfo->cwd = cwd_len ? strdup(buffer + cur) : NULL;
    cur += cwd_len;

    *info = newinfo;
    return 0;
}

void static_tls_info_free(static_tls_info_t *info) {
    if (info->executable)
        free(info->executable);
    if (info->ld_library_path)
        free(info->ld_library_path);
    if (info->ld_preload)
        free(info->ld_preload);
    if (info->cwd)
        free(info->cwd);
    free(info);
}

char *static_tls_info_to_idstr(static_tls_info_t *info)
{
    char *s, *executable, *ld_library, *ld_preload, *cwd;
    size_t size, executable_size, ld_library_size, ld_preload_size, cwd_size;

    executable = info->executable ? info->executable : "NONE";
    ld_library = info->ld_library_path ? info->ld_library_path : "NONE";
    ld_preload = info->ld_preload ? info->ld_preload : "NONE";
    cwd = info->cwd ? info->cwd : "NONE";

    executable_size = info->executable ? strlen(info->executable) : 0;
    ld_library_size = info->ld_library_path ? strlen(info->ld_library_path) : 0;
    ld_preload_size = info->ld_preload ? strlen(info->ld_preload) : 0;
    cwd_size = info->cwd ? strlen(info->cwd) : 0;

    size = strlen(executable) + strlen(ld_library) + strlen(ld_preload) + strlen(cwd) + 8 + 16*4;
    s = (char *) malloc(size);
    s[0] = '\0';
    snprintf(s, size, "%s|%s|%s|%s|%lu|%lu|%lu|%lu", executable, ld_library, ld_preload, cwd, executable_size, ld_library_size, ld_preload_size, cwd_size);
    return s;
}

static_tls_info_t *static_tls_info_dup(static_tls_info_t *info)
{
    static_tls_info_t *newinfo;
    newinfo = (static_tls_info_t *) malloc(sizeof(static_tls_info_t));
    newinfo->executable = info->executable ? strdup(info->executable) : NULL;
    newinfo->ld_library_path = info->ld_library_path ? strdup(info->ld_library_path) : NULL;
    newinfo->ld_preload = info->ld_preload ? strdup(info->ld_preload) : NULL;
    newinfo->cwd = info->cwd ? strdup(info->cwd) : NULL;
    return newinfo;
}