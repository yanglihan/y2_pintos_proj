#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void process_pass_status (int, void*);

/* Child process for parent thread's CHILDREN. This must be on
   the parent's page, because otherwise it will be lost when the
   child thread terminates. STATUS should be -1 at first and
   updated in a call to exit(). */
struct child_proc
{
  struct list_elem elem;
  tid_t tid;
  struct semaphore semaphore;
  int status;
  bool is_exit;
};

#endif /* userprog/process.h */
