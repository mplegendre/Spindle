/*
This file is part of Spindle.  For copyright information see the COPYRIGHT 
file in the top level directory, or at 
https://github.com/hpc/Spindle/blob/master/COPYRIGHT

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License (as published by the Free Software
Foundation) version 2.1 dated February 1999.  This program is distributed in the
hope that it will be useful, but WITHOUT ANY WARRANTY; without even the IMPLIED
WARRANTY OF MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the terms 
and conditions of the GNU General Public License for more details.  You should 
have received a copy of the GNU Lesser General Public License along with this 
program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "lmon_api/common.h"
#include "lmon_api/lmon_proctab.h"
#include "lmon_api/lmon_fe.h"
#include "parse_launcher.h"
#include "spindle_usrdata.h"
#include "config.h"
#include "parseargs.h"
#ifdef __cplusplus
extern "C" {
#endif

#include "ldcs_audit_server_md.h"
#include "ldcs_api.h"
#include "ldcs_api_opts.h"

#ifdef __cplusplus
}
#endif

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <set>
#include <algorithm>
#include <string>
#include <iterator>
#include <pthread.h>

#include "ldcs_api_opts.h"
#include "parse_preload.h"

using namespace std;

#if !defined(BINDIR)
#error Expected BINDIR to be defined
#endif

char spindle_bootstrap[] = BINDIR "/spindle_bootstrap";
char spindle_daemon[] = BINDIR "/spindle_be";
unsigned long opts;
unsigned int shared_secret;

#if defined(USAGE_LOGGING_FILE)
static const char *logging_file = USAGE_LOGGING_FILE;
#else
static const char *logging_file = NULL;
#endif

#define DEFAULT_LDCS_NAME_PREFIX "/tmp/"
std::string ldcs_location_str;
static const char *ldcs_location;
static unsigned int ldcs_number;
static unsigned int ldcs_port;
static void logUser();

double __get_time()
{
  struct timeval tp;
  gettimeofday (&tp, (struct timezone *)NULL);
  return tp.tv_sec + tp.tv_usec/1000000.0;
}

static pthread_mutex_t completion_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t completion_condvar = PTHREAD_COND_INITIALIZER;
static bool spindle_done = false;

static void signal_done()
{
   pthread_mutex_lock(&completion_lock);
   spindle_done = true;
   pthread_cond_signal(&completion_condvar);
   pthread_mutex_unlock(&completion_lock);
}

static void waitfor_done()
{
  pthread_mutex_lock(&completion_lock);
  while (!spindle_done) {
     pthread_cond_wait(&completion_condvar, &completion_lock);
  }
  pthread_mutex_unlock(&completion_lock);
}

static int onLaunchmonStatusChange(int *pstatus) {
   int status = *pstatus;
   if (WIFKILLED(status) || WIFDETACHED(status)) {
      signal_done();
   }
   return 0;
}

static unsigned int get_shared_secret()
{
   int fd = open("/dev/urandom", O_RDONLY);
   if (fd == -1)
      fd = open("/dev/random", O_RDONLY);
   if (fd == -1) {
      fprintf(stderr, "Error: Could not open /dev/urandom or /dev/random for shared secret. Aborting Spindle\n");
      exit(-1);
   }
      
   int result = read(fd, &shared_secret, sizeof(shared_secret));
   close(fd);
   if (result == -1) {
      fprintf(stderr, "Error: Could not read from /dev/urandom or /dev/random for shared secret. Aborting Spindle\n");
      exit(-1);
   }

   return shared_secret;
}

string pt_to_string(const MPIR_PROCDESC_EXT &pt) { return pt.pd.host_name; }
const char *string_to_cstr(const std::string &str) { return str.c_str(); }

int main (int argc, char* argv[])
{  
  int aSession    = 0;
  int launcher_argc, i;
  char **launcher_argv        = NULL;
  const char **daemon_opts = NULL;
  int launchers_to_use;
  int result;
  const char *bootstrapper = spindle_bootstrap;
  const char *daemon = spindle_daemon;
  char ldcs_number_s[32];
  lmon_rc_e rc;
  void *md_data_ptr;
  spindle_daemon_args daemon_args;
  string python_prefixes;

  LOGGING_INIT(const_cast<char *>("FE"));

  debug_printf("Spindle Command Line: ");
  for (int i = 0; i < argc; i++) {
     bare_printf("%s ", argv[i]);
  }
  bare_printf("\n");

  opts = parseArgs(argc, argv);

  if (strlen(LAUNCHMON_BIN_DIR)) {
     setenv("LMON_LAUNCHMON_ENGINE_PATH", LAUNCHMON_BIN_DIR "/launchmon", 0);
     setenv("LMON_PREFIX", LAUNCHMON_BIN_DIR "/..", 0);
  }

  /**
   * If number and location weren't set, then set them.
   **/
  ldcs_port = getPort();
  ldcs_number = getpid(); //Just needs to be unique across spindle jobs shared
                          // by the same user with overlapping nodes.  Launcher
                          // pid should suffice
  ldcs_location_str = getLocation(ldcs_number);
  ldcs_location = ldcs_location_str.c_str();
  python_prefixes = getPythonPrefixes();

  snprintf(ldcs_number_s, 32, "%d", ldcs_number);

  debug_printf("Location = %s, Number = %u, Port = %u\n", ldcs_location, ldcs_number, ldcs_port);

  /**
   * Setup the launcher command line
   **/
  launchers_to_use = TEST_PRESETUP;
  /* TODO: Add more launchers, then use autoconf to select the correct TEST_* values */
  launchers_to_use |= TEST_SLURM;
  result = createNewCmdLine(argc, argv, &launcher_argc, &launcher_argv, bootstrapper, 
                            ldcs_location, ldcs_number_s, opts, launchers_to_use);
  if (result != 0) {
     fprintf(stderr, "Error parsing command line:\n");
     if (result == NO_LAUNCHER) {
        fprintf(stderr, "Could not find a job launcher (mpirun, srun, ...) in the command line\n");
     }
     if (result == NO_EXEC) {
        fprintf(stderr, "Could not find an executable in the given command line\n");
     }
     return EXIT_FAILURE;
  }

  /**
   * Setup the daemon command line
   **/
  daemon_opts = (const char **) malloc(10 * sizeof(char *));
  i = 0;
  //daemon_opts[i++] = "/usr/local/bin/valgrind";
  //daemon_opts[i++] = "--tool=memcheck";
  //daemon_opts[i++] = "--leak-check=full";
  daemon_opts[i++] = daemon;
  daemon_opts[i++] = NULL;

  daemon_args.number = ldcs_number;
  daemon_args.port = ldcs_port;
  daemon_args.opts = opts;
  daemon_args.shared_secret = get_shared_secret();
  daemon_args.location = const_cast<char*>(ldcs_location);
  daemon_args.pythonprefix = const_cast<char*>(python_prefixes.c_str());

  /**
   * Setup LaunchMON
   **/
  rc = LMON_fe_init(LMON_VERSION);
  if (rc != LMON_OK )  {
      err_printf("[LMON FE] LMON_fe_init FAILED\n" );
      return EXIT_FAILURE;
  }

  if (isLoggingEnabled()) {
     logUser();
  }

  rc = LMON_fe_createSession(&aSession);
  if (rc != LMON_OK)  {
     err_printf(  "[LMON FE] LMON_fe_createFEBESession FAILED\n");
    return EXIT_FAILURE;
  }

  rc = LMON_fe_regStatusCB(aSession, onLaunchmonStatusChange);
  if (rc != LMON_OK) {
    err_printf(  "[LMON FE] LMON_fe_regStatusCB FAILED\n");     
    return EXIT_FAILURE;
  }
  
  rc = LMON_fe_regPackForFeToBe(aSession, packfebe_cb);
  if (rc != LMON_OK) {
    err_printf("[LMON FE] LMON_fe_regPackForFeToBe FAILED\n");
    return EXIT_FAILURE;
  } 
  
  debug_printf2("launcher: ");
  for (i = 0; launcher_argv[i]; i++) {
     bare_printf2("%s ", launcher_argv[i]);
  }
  bare_printf2("\n");
  debug_printf2("daemon: ");
  for (i = 0; daemon_opts[i]; i++) {
     bare_printf2("%s ", daemon_opts[i]);
  }
  bare_printf2("\n");

  ldcs_message_t *preload_msg = NULL;
  if (opts & OPT_PRELOAD) {
     string preload_file = string(getPreloadFile());
     preload_msg = parsePreloadFile(preload_file);
     if (!preload_msg) {
        fprintf(stderr, "Failed to parse preload file %s\n", preload_file.c_str());
        return EXIT_FAILURE;
     }
  }

  rc = LMON_fe_launchAndSpawnDaemons(aSession, NULL,
                                     launcher_argv[0], launcher_argv,
                                     daemon_opts[0], const_cast<char **>(daemon_opts+1),
                                     NULL, NULL);
  if (rc != LMON_OK) {
     err_printf("[LMON FE] LMON_fe_launchAndSpawnDaemons FAILED\n");
     return EXIT_FAILURE;
  }

  rc = LMON_fe_sendUsrDataBe(aSession, (void *) &daemon_args);
  if (rc != LMON_OK) {
     err_printf("LMON_fe_sendUsrDataBe failure\n");
     return EXIT_FAILURE;
  }

  /* Get the process table */
  unsigned int ptable_size, actual_size;
  rc = LMON_fe_getProctableSize(aSession, &ptable_size);
  if (rc != LMON_OK) {
     err_printf("LMON_fe_getProctableSize failed\n");
     return EXIT_FAILURE;
  }
  MPIR_PROCDESC_EXT *proctab = (MPIR_PROCDESC_EXT *) malloc(sizeof(MPIR_PROCDESC_EXT) * ptable_size);
  rc = LMON_fe_getProctable(aSession, proctab, &actual_size, ptable_size);
  if (rc != LMON_OK) {
     err_printf("LMON_fe_getProctable failed\n");
     return EXIT_FAILURE;
  }

  /* Transform the process table to a list of hosts.  Pass through std::set to make hostnames unique */
  set<string> allHostnames;
  transform(proctab, proctab+ptable_size, inserter(allHostnames, allHostnames.begin()), pt_to_string);
  size_t hosts_size = allHostnames.size();
  const char **hosts = (const char **) malloc(hosts_size * sizeof(char *));
  transform(allHostnames.begin(), allHostnames.end(), hosts, string_to_cstr);
  free(proctab);

  /* SPINDLE FE start */
  ldcs_audit_server_fe_md_open( const_cast<char **>(hosts), hosts_size, ldcs_port, &md_data_ptr );

  if (preload_msg) {
     debug_printf("Sending message with preload information\n");
     ldcs_audit_server_fe_broadcast(preload_msg, md_data_ptr);
     cleanPreloadMsg(preload_msg);
  }

  waitfor_done();

  /* Close the server */
  ldcs_audit_server_fe_md_close(md_data_ptr);

  debug_printf("Exiting with success\n");
  return EXIT_SUCCESS;
}

static void logUser()
{
   /* Collect username */
   char *username = NULL;
   if (getenv("USER")) {
      username = getenv("USER");
   }
   if (!username) {
      struct passwd *pw = getpwuid(getuid());
      if (pw) {
         username = pw->pw_name;
      }
   }
   if (!username) {
      username = getlogin();
   }
   if (!username) {
      err_printf("Could not get username for logging\n");
   }
      
   /* Collect time */
   struct timeval tv;
   gettimeofday(&tv, NULL);
   struct tm *lt = localtime(& tv.tv_sec);
   char time_str[256];
   time_str[0] = '\0';
   strftime(time_str, sizeof(time_str), "%c", lt);
   time_str[255] = '\0';
   
   /* Collect version */
   const char *version = VERSION;

   /* Collect hostname */
   char hostname[256];
   hostname[0] = '\0';
   gethostname(hostname, sizeof(hostname));
   hostname[255] = '\0';

   string log_message = string(username) + " ran Spindle v" + version + 
                        " at " + time_str + " on " + hostname;
   debug_printf("Logging usage: %s\n", log_message.c_str());

   FILE *f = fopen(logging_file, "a");
   if (!f) {
      err_printf("Could not open logging file %s\n");
      return;
   }
   fprintf(f, "%s\n", log_message.c_str());
   fclose(f);
}
