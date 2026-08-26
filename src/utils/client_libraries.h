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

#if !defined(CLIENT_LIBRARIES_H_)
#define CLIENT_LIBRARIES_H_

#include "config.h"

#if !defined(PROGLIBDIR)
#error Expected to be built with proglib defined
#endif

char libstr_socket_subaudit[] = PROGLIBDIR "/libspindle_subaudit_socket.so";
char libstr_pipe_subaudit[] = PROGLIBDIR "/libspindle_subaudit_pipe.so";
char libstr_biter_subaudit[] = PROGLIBDIR "/libspindle_subaudit_biter.so";

char libstr_socket_audit[] = PROGLIBDIR "/libspindle_audit_socket.so";
char libstr_pipe_audit[] = PROGLIBDIR "/libspindle_audit_pipe.so";
char libstr_biter_audit[] = PROGLIBDIR "/libspindle_audit_biter.so";

#if defined(COMM_SOCKET)
static char *default_audit_libstr = libstr_socket_audit;
static char *default_subaudit_libstr = libstr_socket_subaudit;
#elif defined(COMM_PIPES)
static char *default_audit_libstr = libstr_pipe_audit;
static char *default_subaudit_libstr = libstr_pipe_subaudit;
#elif defined(COMM_BITER)
static char *default_audit_libstr = libstr_biter_audit;
static char *default_subaudit_libstr = libstr_biter_subaudit;
#else
#error Unknown connection type
#endif

#endif