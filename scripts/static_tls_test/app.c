#include <pthread.h>
#include <stdio.h>

extern void set_bigtls();
extern int get_bigtls();

void *runthrd(void *name)
{
   get_bigtls();
   return NULL;
}

int main(int argc, char *argv[])
{
   pthread_t thrd;

   pthread_create(&thrd, NULL, runthrd, "thrd");
   set_bigtls();
   runthrd("main");
   pthread_join(thrd, NULL);
   printf("Passed\n");
   return 0;
}
   
