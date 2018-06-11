#define REMAP_CONF_TEST

#define debug_printf printf
#define debug_printf2 printf
#define debug_printf3 printf
#define err_printf printf
#define spindle_free free
#define MAX_PATH_LEN 4096
#include <string.h>

void get_relocated_file(int ldcs_id, char *orig_exec, char **reloc_exec, int *errcode)
{
   *reloc_exec = strdup("./remap_main2");
   *errcode = 0;
}

#include "src/client/client/remap_exec.c"

int dowork()
{
   remap_executable(0);

   char buffer[128];
   snprintf(buffer, 128, "ls -l /proc/%d/exe", getpid());
   system(buffer);
   snprintf(buffer, 128, "cat /proc/%d/maps", getpid());
   system(buffer);
   return 0;
}
