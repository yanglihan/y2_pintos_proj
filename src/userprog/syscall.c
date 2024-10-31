#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

typedef int pid_t;

static void syscall_handler (struct intr_frame *);

void
process_terminatation_msg (char *name, int code)
{
  printf("%s: exit(%d)\n", name, code);
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{ 
  int syscall_num = *((int *) f->esp);
  void *param1 = f->esp + 4;
  void *param2 = f->esp + 8;
  void *param3 = f->esp + 12;
   
  switch (syscall_num)
    {
      case SYS_EXIT:
        exit (*((int *) param1));
        break;
      case SYS_WRITE:
        write (*((int *) param1), *((void **) param2), *((unsigned *) param3));
        break;
    }
}

void
exit (int status)
{
  // terminates the current user program, sending its exit status to the kernel
  struct thread *t = thread_current ();

  if (t->pagedir == NULL) // if t == kernel thread or when halt system call is invoked, do not call the msg
    process_terminatation_msg (t->name, status);

  // exit
  thread_exit ();
}

int
write (int fd, const void *buffer, unsigned size)
{
  /* Fd 1 writes to the console. */
  if (fd == 1)
  {
    putbuf (buffer, size);

    return size; // returns the no. of bytes actually written, which may be less than size if some bytes could not be written
  } 
  else /* Otherwise, writes to file. */
  {
    // write to file
    // TODO:
  }
}