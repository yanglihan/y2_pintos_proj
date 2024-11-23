#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/synch.h"
#include "threads/thread.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void process_pass_status (int, void *);

/* A file opened by a user program. */
struct user_file
{
  int fd;
  struct file *file;
  struct list_elem elem;
};

#endif /* userprog/process.h */
