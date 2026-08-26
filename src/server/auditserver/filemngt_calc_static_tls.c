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

#define _GNU_SOURCE
#include <elf.h>
#include <link.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "config.h"
#include "static_tls.h"
#include "ldcs_api.h"
#include "spindle_debug.h"
#include "client_libraries.h"

typedef struct library_list_t {
   char *path;
   char *interp;
   ssize_t tls_size;
   ssize_t tls_alignment;
   struct library_list_t *next;
   struct library_list_t *hash_next;
} library_list_t;

typedef struct executable_list_t {
   static_tls_info_t *info;
   ssize_t tls_size;
   ssize_t tls_alignment;
   struct executable_list_t *hash_next;
} executable_list_t;

#define EXECUTABLE_HASH_SIZE 64
static executable_list_t *executable_hash_table[EXECUTABLE_HASH_SIZE];
static executable_list_t *executable_hash_lookup(static_tls_info_t *info);
static executable_list_t *executable_hash_add(static_tls_info_t *info);

#define LIBRARY_HASH_SIZE 512
static library_list_t *library_hash_table[LIBRARY_HASH_SIZE];
static library_list_t *library_hash_lookup(const char *path);
static library_list_t *library_hash_add(const char *path);
static void library_list_clean(library_list_t *l);

static int static_tls_size_for_dso(const char *path, char *interp, size_t interp_str_size, int *is_script_fd, ssize_t *tls_size_result, ssize_t *tls_alignment_result);
static library_list_t *add_to_filelist(library_list_t **head, char *path);
static ssize_t reliable_read(int fd, void *buffer, size_t size);
static library_list_t *get_libraries(static_tls_info_t *info, const char *ldso);
static int handle_script(static_tls_info_t *info, int fd, ssize_t *tls_size, ssize_t *tls_align);
static void add_tls_sizes(ssize_t *tls_total, ssize_t *max_alignment, ssize_t tls_new, ssize_t tls_align);
static int calc_static_tls_for_audit_client(char *interp, ssize_t *tls_size, ssize_t *tls_alignment);
static int calc_static_tls_for_executable_nocache(static_tls_info_t *info, char *use_interp, ssize_t *tls_size_result_p, ssize_t *tls_alignment_result_p);
static int calc_static_tls_for_executable_w_interp(static_tls_info_t *info, char *interp, ssize_t *tls_size, ssize_t *tls_alignment);

int static_tls_info_lookup(static_tls_info_t *info, ssize_t *tls_size, ssize_t *tls_alignment)
{
   executable_list_t *e;
   e = executable_hash_lookup(info);
   if (!e) {
      return -1;
   }
   if (e->tls_size == -1 && e->tls_alignment == -1)
      return -1;
   if (tls_size)
      *tls_size = e->tls_size;
   if (tls_alignment)
      *tls_alignment = e->tls_alignment;
   return 0;
}

int static_tls_info_add(static_tls_info_t *info, ssize_t tls_size, ssize_t tls_alignment) 
{
   executable_list_t *e;
   e = executable_hash_add(info);
   if (!e) {
      err_printf("Error adding static tls info for %s\n", info->executable);
      return -1;
   }
   e->tls_size = tls_size;
   e->tls_alignment = tls_alignment;
   return 0;
}

static int calc_static_tls_for_executable_w_interp(static_tls_info_t *info, char *interp, ssize_t *tls_size, ssize_t *tls_alignment) 
{
   int result;
   ssize_t size_result, align_result;
   executable_list_t *e;
   result = calc_static_tls_for_executable_nocache(info, interp, &size_result, &align_result);
   if (result == -1) {
      size_result = -1;
      align_result = -1;
   }

   e = executable_hash_lookup(info);
   if (!e) 
      e = executable_hash_add(info);
   e->tls_size = size_result;
   e->tls_alignment = align_result;

   if (tls_size)
      *tls_size = size_result;
   if (align_result)
      *tls_alignment = align_result;

   return result;
}

int calc_static_tls_for_executable(static_tls_info_t *info, ssize_t *tls_size, ssize_t *tls_alignment) 
{
   int result;
   executable_list_t *e;
   ssize_t size_result, align_result;
   result = calc_static_tls_for_executable_nocache(info, NULL, &size_result, &align_result);
   if (result == -1) {
      size_result = -1;
      align_result = -1;
   }

   e = executable_hash_lookup(info);
   if (!e) 
      e = executable_hash_add(info);
   e->tls_size = size_result;
   e->tls_alignment = align_result;

   if (tls_size)
      *tls_size = size_result;
   if (align_result)
      *tls_alignment = align_result;

   return result;
}

static int calc_static_tls_for_executable_nocache(static_tls_info_t *info, char *use_interp, ssize_t *tls_size_result_p, ssize_t *tls_alignment_result_p) 
{
   ssize_t tls_size_exe = 0, tls_size_auditclient = 0, tls_size = 0, tls_size_total = 0;
   ssize_t tls_align = 0, tls_align_exe = 0, tls_align_auditclient = 0, max_alignment = 0;
   int script_fd = -1, result;
   library_list_t *dt_needed_libs, *i;
   
   char interp[MAX_PATH_LEN+1];
   interp[0] = '\0';

   debug_printf2("Asked to calculate static tls needed for %s with ld_library_path='%s' ; ld_preload='%s' ; cwd='%s'\n",
      info->executable, info->ld_library_path, info->ld_preload, info->cwd);
   debug_printf2("Using interpreter: %s\n", use_interp ? use_interp : "[DEFAULT]\n");
   //Look up TLS size and interpreter for the executable
   result = static_tls_size_for_dso(info->executable, interp, sizeof(interp), &script_fd, &tls_size_exe, &tls_align_exe);
   if (result == -1) {
      err_printf("Could not parse executable %s\n", info->executable);
      return -1;
   }
   if (script_fd != -1) {
      //This was a script that started with #!      
      return handle_script(info, script_fd, tls_size_result_p, tls_alignment_result_p);
   }
   if (use_interp) {
      strncpy(interp, use_interp, sizeof(interp)-1);
      interp[sizeof(interp)-1] = '\0';
   }
   else if (interp[0] == '\0') {
      err_printf("%s was not a dynamic executable\n", info->executable);
      return -1;
   }
   add_tls_sizes(&tls_size_total, &max_alignment, tls_size_exe, tls_align_exe);

   if (!use_interp) {
      result = calc_static_tls_for_audit_client(interp, &tls_size_auditclient, &tls_align_auditclient);
      if (result == -1) {
         err_printf("Could not calculate static TLS needed in audit client\n");      
      }
      else {
         add_tls_sizes(&tls_size_total, &max_alignment, tls_size_auditclient, tls_align_auditclient);
      }
   }
   
   //Get the statically-determinable list of libraries this exe depends on 
   dt_needed_libs = get_libraries(info, interp);
   if (!dt_needed_libs) {
      err_printf("%s could not parse library list\n", info->executable);
      return -1;
   }
   
   for (i = dt_needed_libs; i != NULL; i = i->next) {
      result = static_tls_size_for_dso(i->path, NULL, 0, NULL, &tls_size, &tls_align);
      if (result == -1)
         continue;
      add_tls_sizes(&tls_size_total, &max_alignment, tls_size, tls_align);
   }
   library_list_clean(dt_needed_libs);
   debug_printf2("Calculated static tls for %s as %ld/+%ld\n", info->executable, tls_size_total, max_alignment);
   *tls_size_result_p = tls_size_total;
   *tls_alignment_result_p = max_alignment;
   return tls_size_total;
}

static int handle_script(static_tls_info_t *info, int fd, ssize_t *tls_size, ssize_t *tls_align)
{
   char interp[MAX_PATH_LEN+1];
   char *line = NULL;
   size_t linesize = 0, i, j;
   ssize_t glresult;
   executable_list_t *entry;
   static_tls_info_t *interp_info;
   int result;
      
   FILE *f = fdopen(fd, "r");   
   if (!f) {
      close(fd);
      err_printf("Error reopening FILE for %s\n", info->executable);
      return -1;
   }

   glresult = getline(&line, &linesize, f);
   if (glresult == -1) {
      err_printf("Error reading line in script for %s\n", info->executable);
      fclose(f);
      return -1;
   }
   fclose(f);

   //skip '#!' and then initial spaces
   for (i = 0; i < linesize && (line[i] == ' ' || line[i] == '\t'); i++);
   for (j = 0;
        j < sizeof(interp) && i < linesize && line[i] != ' ' && line[i] != '\t' && line[i] != '\n';
        j++, i++)
   {
      interp[j] = line[i];
   }
   if (j == sizeof(interp))
      j--;
   interp[j] = '\0';

   //We have the hashbang string after the intepreter. Make a new static_tls_info_t for the 
   // script interpreter and Re-run the whole calculation on that.
   interp_info = static_tls_info_dup(info);
   free(interp_info->executable);
   interp_info->executable = strdup(interp);

   result = static_tls_info_lookup(interp_info, tls_size, tls_align);
   if (result != -1) {
      static_tls_info_free(interp_info);
      free(line);
      return result;
   }

   result = calc_static_tls_for_executable_nocache(interp_info, NULL, tls_size, tls_align);

   //Associate the TLS size from the interpreter with the script
   entry = executable_hash_add(interp_info);
   entry->tls_size = *tls_size;
   entry->tls_alignment = *tls_align;

   free(line);   
   return 0;
}

static int static_tls_size_for_dso(const char *path, char *interp, size_t interp_str_size, int *is_script_fd, ssize_t *tls_size_result, ssize_t *tls_alignment_result)
{
   int fd = -1, i, has_static_tls = 0;
   void *buffer = NULL;
   char *cbuffer;
   ssize_t result;
   off_t oresult;
   ElfW(Ehdr) *ehdr;
   ElfW(Phdr) *phdr, *phdrs;
   ElfW(Half) phnum, phentsize;
   ElfW(Off) phoff = 0, dynamic_off = 0, interp_off = 0;
   ElfW(Xword) dynamic_size = 0, interp_size = 0;
   ElfW(Dyn) *dyn = NULL, *end_dyn;
   ssize_t tls_size = -1, return_result = -1, tls_align = -1;
   library_list_t *lib;
   char ldso[MAX_PATH_LEN+1];
   ldso[0] = '\0';
   
   debug_printf3("Calculating file-specific static TLS needed for %s\n", path);
   //Check cache if library has already been parsed.
   lib = library_hash_lookup(path);
   if (lib) {
      if (interp) {
         strncpy(interp, lib->interp, interp_str_size);
      }
      if (tls_size_result)
         *tls_size_result = lib->tls_size;
      if (tls_alignment_result)
         *tls_alignment_result = lib->tls_alignment;
      return 0;
   }
   
   fd = open(path, O_RDONLY);
   if (fd == -1 && path[0] == '/') {
      err_printf("Could not open '%s'\n", path);
      goto done;
   }
   else if (fd == -1) {
      goto done;
   }

   buffer = (void *) malloc(sizeof(ElfW(Ehdr)));
   cbuffer = (char *) buffer;

   //First check the initial two characters for script hashbang '#!'
   result = reliable_read(fd, buffer, 2);
   if (result < 2) {
      err_printf("Not an executable or script %s\n", path);
      goto done;
   }
   else if (cbuffer[0] == '#' && cbuffer[1] == '!') {
      *is_script_fd = fd;
      free(buffer);
      return 0;
   }
   else if (cbuffer[0] == ELFMAG0 && cbuffer[1] == ELFMAG1) {
      //Fall through to the below DSO handling
   }
   else {
      err_printf("Not an executable or script %s\n", path);
      goto done;
   }

   //Read an elf header (minus the two bytes we already read)
   result = reliable_read(fd, cbuffer+2, sizeof(ElfW(Ehdr))-2);
   if (result == -1) {
      err_printf("Could not read elf eheader %s\n", path);
      goto done;
   }
   if (result != sizeof(ElfW(Ehdr))-2) {
      err_printf("Not an executable or script %s\n", path);
      return -1;
   }

   ehdr = (ElfW(Ehdr) *) buffer;
   if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
       ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3) {
      err_printf("Is not an elf file or script %s\n", path);
      goto done;
   }

   phnum = ehdr->e_phnum;
   phentsize = ehdr->e_phentsize;
   phoff = ehdr->e_phoff;
   
   free(buffer);

   //Using info from the elf header, seek to and read the program headers.
   buffer = malloc(phentsize * phnum);
   oresult = lseek(fd, phoff, SEEK_SET);
   if (result == -1) {
      err_printf("Could not seek to program headers %s\n", path);
      goto done;
   }
   result = reliable_read(fd, buffer, phentsize * phnum);
   if (result == -1) {
      err_printf("Could not read program header %s\n", path);
      goto done;
   }

   //Iterate through program headers for necessary info (TLS size,
   // where's the interpreter string, where's the dynamic section.
   phdrs = (ElfW(Phdr) *) buffer;
   for (i = 0; i < phnum; i++) {
      phdr = phdrs + i;
      if (phdr->p_type == PT_TLS) {
         tls_size = phdr->p_memsz;
         tls_align = phdr->p_align;
      }
      if (phdr->p_type == PT_DYNAMIC) {
         dynamic_off = phdr->p_offset;
         dynamic_size = phdr->p_filesz;
      }
      if (phdr->p_type == PT_INTERP) {
         interp_off = phdr->p_offset;
         interp_size = phdr->p_filesz;
      }
   }
   free(buffer);
   buffer = NULL;

   if (!dynamic_off && !dynamic_size) {
      debug_printf3("Is a static binary: %s\n", path);
      goto done;
   }

   //Read the interpreter string.
   if (interp_off && interp_size) {
      oresult = lseek(fd, interp_off, SEEK_SET);
      if (oresult == -1) {
         err_printf("Could not seek to interpreter for %s\n", path);
         goto done;
      }
      
      if (interp_size >= sizeof(ldso))
         interp_size = sizeof(ldso) - 1;
      result = reliable_read(fd, ldso, interp_size);
      if (result == -1) {
         err_printf("Could not read interpreter for %s\n", path);
         goto done;
      }
      ldso[interp_size] = '\0';
   }
   
   //Read the dynamic section.
   buffer = malloc(dynamic_size);

   oresult = lseek(fd, dynamic_off, SEEK_SET);
   if (oresult == -1) {
      err_printf("Could not seek to dynamic section for %s\n", path);
      goto done;
   }

   result = reliable_read(fd, buffer, dynamic_size);
   if (result == -1) {
      err_printf("Could not read dynamic section for %s\n", path);
      goto done;
   }

   //Look through the dynamic section for the DT_FLAGS and if DT_STATIC_TLS
   // is set in it.
   end_dyn = (ElfW(Dyn) *) (((unsigned char *) buffer) + dynamic_size);
   dyn = (ElfW(Dyn) *) buffer;
   for (dyn = (ElfW(Dyn) *) buffer; (dyn->d_tag != DT_NULL) && (dyn < end_dyn); dyn++) {
      if (dyn->d_tag == DT_FLAGS) {
         has_static_tls = (dyn->d_un.d_val & DF_STATIC_TLS) ? 1 : 0;
         break;
      }
   }

   (void) has_static_tls;
   //if (!has_static_tls)
   //tls_size = 0;

   if (tls_size_result)
      *tls_size_result = tls_size;
   if (tls_alignment_result)
      *tls_alignment_result = tls_align;
   return_result = 0;

  done:
   lib = library_hash_add(path);
   lib->tls_size = return_result != -1 ? tls_size : -1;
   lib->tls_alignment = return_result != -1 ? tls_align : -1;
   lib->interp = ldso[0] ? strdup(ldso) : "";
   if (interp) {
      strncpy(interp, ldso, interp_size);
   }
   debug_printf2("Calculated file-specific static TLS for %s as %ld/+%ld\n", path, tls_size, tls_align);
   
   if (buffer)
      free(buffer);
   if (fd != -1)
      close(fd);
   return return_result;
}

static library_list_t *add_to_filelist(library_list_t **head, char *path)
{
   library_list_t *newfile;

   newfile = (library_list_t *) malloc(sizeof(library_list_t));
   newfile->path = strdup(path);
   newfile->interp = NULL;
   newfile->tls_size = 0;
   newfile->next = *head;
   newfile->hash_next = NULL;
   *head = newfile;
   
   return newfile;
}

#define PRD 0
#define PWR 1
static library_list_t *get_libraries(static_tls_info_t *info, const char *ldso)
{
   int pipe_fd[2];
   int result, status, i;
   ssize_t sresult;
   const char* args[4];
   char *line = NULL;
   size_t line_size = 0;
   pid_t pid;
   FILE *f = NULL;
   library_list_t *filelist = NULL;

   args[0] = ldso;
   args[1] = "--list";
   args[2] = info->executable;
   args[3] = NULL;
   pipe_fd[0] = pipe_fd[1] = -1;

   result = pipe(pipe_fd);
   if (result == -1) {
      err_printf("Error creating pipe\n");
      goto done;
   }

   if (spindle_debug_prints > 2) {
      debug_printf3("Getting static tls library list with ");
      for (i = 0; args[i]; i++) {
         bare_printf3("%s ", args[i]);
      }
      bare_printf3("\n");
   }
   pid = fork();
   if (pid == -1) {
      err_printf("Error forking new process\n");
      goto done;
   }
   if (pid == 0) {
      close(pipe_fd[PRD]);
      if (info->ld_library_path)
         setenv("LD_LIBRARY_PATH", info->ld_library_path, 1);
      else
         unsetenv("LD_LIBRARY_PATH");
      if (info->ld_preload)
         setenv("LD_PRELOAD", info->ld_preload, 1);
      else
         unsetenv("LD_PRELOAD");
      if (info->cwd)
         chdir(info->cwd);
      dup2(pipe_fd[PWR], 1);
      dup2(pipe_fd[PWR], 2);
      execv(args[0], (char * const*) args);
      err_printf("Error execing ld.so %s\n", ldso);
      exit(-1);
   }
   close(pipe_fd[PWR]);
   pipe_fd[PWR] = -1;
   result = waitpid(pid, &status, 0);
   if (result == -1) {
      err_printf("Error during waitpid on %s --list %s: %s\n",
              ldso, info->executable, strerror(errno));
      goto done;
   }
   if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      err_printf("App exited with error on %d %s\n",
              (int) WEXITSTATUS(status), ldso);
      goto done;
   }
   if (WIFSIGNALED(status)) {
      err_printf("App exited with signal %d on %s\n",
              (int) WTERMSIG(status), ldso);
      goto done;
   }
   if (!WIFEXITED(status)) {
      err_printf("App did not exit via status %d on %s\n",
              (int) status, ldso);
      goto done;
   }

   f = fdopen(pipe_fd[PRD], "r");
   if (!f) {
      err_printf("Could not convert pipe fd to stream\n");
      goto done;
   }

   for (;;) {
      char *arrow, *paren, *start, *end;
      sresult = getline(&line, &line_size, f);
      if (sresult == -1)
         break;
      start = line;
      //skip whitespace and blank lines
      while ((*start == '\t' || *start == ' ') && (*start != '\0'))
         start++;
      if (*start == '\0')
         continue;
      arrow = strstr(line, " => ");
      if (arrow) {
         start = arrow + 4;
         if (!*start) {
            err_printf("parse error at arrow line %s\n", line);
            continue;
         }
      }
      paren = strrchr(start, '(');
      end = paren ? paren-1 : line+strlen(line)-1;
      while ((*end == '\t' || *end == ' ') && (end != start)) end--;
      end++;
      *end = '\0';

      add_to_filelist(&filelist, start);
   };

  done:
   if (f) {
      fclose(f);
   }
   else if (pipe_fd[PRD] != -1) {
      close(pipe_fd[PRD]);
   }
   if (pipe_fd[PWR] != -1) {
      close(pipe_fd[PRD]);
   }
   if (line) {
      free(line);
   }
   
   return filelist;   
}

static unsigned long hash_func(static_tls_info_t *info)
{
   unsigned long hash = 5381;
   int c;
   char *s;

   if (info->executable) {
      s = info->executable;
      while ((c = *s++))
         hash = ((hash << 5) + hash) + c;
   }
   if (info->ld_library_path) {
      s = info->ld_library_path;
      while ((c = *s++))
         hash = ((hash << 5) + hash) + c;
   }
   if (info->ld_preload) {
      s = info->ld_preload;
      while ((c = *s++))
         hash = ((hash << 5) + hash) + c;
   }
   if (info->cwd) {
      s = info->cwd;
      while ((c = *s++))
         hash = ((hash << 5) + hash) + c;
   }
   return hash;
}

static unsigned long str_hash_func(const char *s)
{
   unsigned long hash = 5381;
   int c;
   while ((c = *s++))
      hash = ((hash << 5) + hash) + c;
   return hash;
}

static executable_list_t *executable_hash_lookup(static_tls_info_t *info)
{
   unsigned long key;
   executable_list_t *i;

   key = hash_func(info) % EXECUTABLE_HASH_SIZE;
   for (i = executable_hash_table[key]; i != NULL; i = i->hash_next) {
      if (static_tls_info_equal(i->info, info))
         return i;
   }
   return NULL;
}

static executable_list_t *executable_hash_add(static_tls_info_t *info)
{
   unsigned long key;
   executable_list_t *newh;

   key = hash_func(info) % EXECUTABLE_HASH_SIZE;
   newh = (executable_list_t *) malloc(sizeof(executable_list_t));
   newh->info = static_tls_info_dup(info);
   newh->hash_next = executable_hash_table[key];
   executable_hash_table[key] = newh;

   return newh;
}

static library_list_t *library_hash_lookup(const char *path)
{
   unsigned long key;
   library_list_t *i;

   key = str_hash_func(path) % LIBRARY_HASH_SIZE;
   for (i = library_hash_table[key]; i != NULL; i = i->hash_next) {
      if (strcmp(path, i->path) == 0) {
         return i;
      }
   }
   return NULL;
}

static library_list_t *library_hash_add(const char *path)
{
   unsigned long key;
   library_list_t *newh;

   key = str_hash_func(path) % LIBRARY_HASH_SIZE;
   newh = (library_list_t *) malloc(sizeof(library_list_t));
   newh->path = strdup(path);
   newh->hash_next = library_hash_table[key];
   library_hash_table[key] = newh;

   return newh;
}

static ssize_t reliable_read(int fd, void *buffer, size_t size)
{
   ssize_t bytes_read = 0, result;
   int retry_count = 10, error;

   do {
      result = read(fd, ((unsigned char *) buffer) + bytes_read, size - bytes_read);
      if (result == -1) {
         if (errno == EINTR) {
            continue;
         }
         else if (errno == EAGAIN  && retry_count-- >= 0) {
            continue;
         }
         error = errno;
         err_printf("read returned error %d\n", error);
         errno = error;
         return -1;
      }
      else if (result == 0) {
         return bytes_read;
      }
      bytes_read += result;
   } while (bytes_read < size);
   
   return bytes_read;
}

static void add_tls_sizes(ssize_t *tls_total, ssize_t *max_alignment, ssize_t tls_new, ssize_t tls_align)
{
   *tls_total += tls_new;
   if (tls_align > *max_alignment)
      *max_alignment = tls_align;
   //We don't know the order the linker will use for libraries, so we can't calculate an accurate alignment.
   //Instead we'll add the alignment amount to the allocation size an an upper-bound.
   *tls_total += tls_align;
}

static void library_list_clean(library_list_t *l)
{
   library_list_t *j, *next;
   next = NULL;
   for (j = l; j != NULL; j = next) {
      next = j->next;
      free(j->path);
      if (j->interp && j->interp[0])
         free(j->interp);
      free(j);
   }
}

static int calc_static_tls_for_audit_client(char *interp, ssize_t *tls_size, ssize_t *tls_alignment)
{
   static_tls_info_t info;
   ssize_t audit_tls_size = 0, audit_tls_alignment = 0;
   ssize_t subaudit_tls_size = 0, subaudit_tls_alignment = 0;
   int audit_result = -1, subaudit_result = -1;
   static ssize_t cached_audit_client_tls_size = -2;
   static ssize_t cached_audit_client_tls_align;

   if (cached_audit_client_tls_size != -2) {
      *tls_size = cached_audit_client_tls_size;
      *tls_alignment = cached_audit_client_tls_align;
      return 0;
   }

   if (default_audit_libstr && *default_audit_libstr) {
      info.executable = default_audit_libstr;
      info.ld_library_path = NULL;
      info.ld_preload = NULL;
      info.cwd = NULL;
      audit_result = calc_static_tls_for_executable_w_interp(&info, interp, &audit_tls_size, &audit_tls_alignment);
   }

   if (default_subaudit_libstr && *default_subaudit_libstr) {
      info.executable = default_subaudit_libstr;
      info.ld_library_path = NULL;
      info.ld_preload = NULL;
      info.cwd = NULL;
      subaudit_result = calc_static_tls_for_executable_w_interp(&info, interp, &subaudit_tls_size, &subaudit_tls_alignment);
   }

   if (audit_result == -1 && subaudit_result == -1) {
      *tls_size = cached_audit_client_tls_size = -1;
      *tls_alignment = cached_audit_client_tls_align = -1;
      return -1;
   }

   cached_audit_client_tls_size = audit_tls_size > subaudit_tls_size ? audit_tls_size : subaudit_tls_size;
   cached_audit_client_tls_align = audit_tls_alignment > subaudit_tls_alignment ? audit_tls_alignment : subaudit_tls_alignment;

   return 0;
}
