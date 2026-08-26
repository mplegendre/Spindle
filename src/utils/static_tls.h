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

#if !defined(STATIC_TLS_H_)
#define STATIC_TLS_H_

typedef struct {
    char *executable;
    char *ld_library_path;
    char *ld_preload;
    char *cwd;
} static_tls_info_t;

int static_tls_info_equal(const static_tls_info_t *info_a, const static_tls_info_t *info_b);
int static_tls_info_encode_size_needed(const char *executable, const char *ld_library_path, const char *ld_preload, const char *cwd);
int static_tls_info_encode_size_needed2(const static_tls_info_t *info);
int static_tls_info_encode(const char *executable, const char *ld_library_path, const char *ld_preload, const char *cwd, char *buffer, int buffer_in_size, int *buffer_out_size);
int static_tls_info_encode2(const static_tls_info_t *info, char *buffer, int buffer_in_size, int *buffer_out_size);
int static_tls_info_decode(char *buffer, int buffer_len, static_tls_info_t **info);
static_tls_info_t *static_tls_info_dup(static_tls_info_t *info);
void static_tls_info_free(static_tls_info_t *info);
char *static_tls_info_to_idstr(static_tls_info_t *info);

#endif