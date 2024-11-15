#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

/* A user process. */
struct process
  {
   char file_name[16];
   bool is_load;
   int pid;
   struct semaphore load_sema;
   struct thread *t;
   struct list *children;
  };

#endif /* userprog/process.h */
