#include "userprog/syscall.h"
#include <stdio.h>
#include <list.h>
#include <syscall-nr.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/shutdown.h"
#include "devices/input.h"

typedef int pid_t;

static void syscall_handler (struct intr_frame *);
static void halt (void);
static void exit (int status);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static int filesize (int fd);
static int write (int fd, const void *buffer, unsigned size);
static int read (int fd, void *buffer, unsigned size);
static void process_termination_msg (char *name, int code);
static struct user_file *find_user_file(int fd);

struct list open_files;

static struct lock filesys_lock;
static struct lock file_lock;
static int cur_fd = 2;

struct user_file
  {
    int fd;
    struct file *file;
    struct list_elem elem;
  };

static void
process_termination_msg (char *name, int code)
{
  printf("%s: exit(%d)\n", name, code);
}

/* find the file with given file descriptor in open_files,
   return NULL if it is not found */
static struct user_file *
find_user_file(int fd)
{ 
  lock_acquire (&file_lock);
  struct list_elem *e = list_begin (&open_files);
  for (e = list_next (e); e != list_end (&open_files); e = list_next (e))
    {
      struct user_file *file = list_entry (e, struct user_file, elem);
      if (file->fd == fd)
       {
        lock_release (&file_lock);
        return file;
       }
    }
  lock_release (&file_lock);
  return NULL;
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  list_init (&open_files);
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
      case SYS_HALT:
        halt();
        break;
      case SYS_EXIT:
        exit (*((int *) param1));
        break;
      case SYS_WRITE:
        write (*((int *) param1), *((void **) param2), *((unsigned *) param3));
        break;
    }
}

static void
halt (void)
{
  shutdown_power_off();
}

static void
exit (int status)
{
  // terminates the current user program, sending its exit status to the kernel
  struct thread *t = thread_current ();

  if (t->pagedir == NULL) // if t == kernel thread or when halt system call is invoked, do not call the msg
    process_termination_msg (t->name, status);

  // exit
  thread_exit ();
}

static bool
create (const char *file, unsigned initial_size)
{
  /* create a file using the file system */
  lock_acquire (&filesys_lock);
  bool is_created = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return is_created;
}

static bool
remove (const char *file)
{ 
  /* remove the file using the file system */
  lock_acquire (&filesys_lock);
  bool is_removed = filesys_remove (file);
  lock_release (&filesys_lock);
  return is_removed;
}

static int 
filesize (int fd)
{
  struct user_file *file = find_user_file (fd);
  ASSERT (file != NULL);
  lock_acquire (&filesys_lock);
  int size = file_length (file->file);
  lock_release (&filesys_lock);
  return size;
}

static int
open (const char *file)
{ 
  /* open the file using the file system*/
  lock_acquire (&filesys_lock);
  struct file *ret_file = filesys_open (file);
  lock_release (&filesys_lock);
  if (ret_file == NULL)
    return -1;
  
  /* generate new file descriptor and keep track of the file */
  struct user_file new_file;
  lock_acquire (&file_lock);
  new_file.fd = cur_fd++;
  new_file.file = ret_file;
  list_push_front (&open_files, &new_file.elem);
  lock_release (&file_lock);

  return new_file.fd;
}

static int
read (int fd, void *buffer, unsigned size)
{ 
  /* Fd 0 read from keyboard */
  if (fd == STDIN_FILENO)
    {
      for (unsigned i = 0; i < size; i++)
        *(uint8_t *) (buffer + i) = input_getc();
      return size;
    }
  else
    {
      struct user_file *file = find_user_file (fd);
      if (file == NULL)
        return -1;
      lock_acquire (&filesys_lock);
      int bytes = file_read (file->file, buffer, size);
      lock_release (&filesys_lock);
      return bytes;
    }
}

static int
write (int fd, const void *buffer, unsigned size)
{
  /* Fd 1 writes to the console. */
  if (fd == STDOUT_FILENO)
  {
    putbuf (buffer, size);
    return size;
  } 
  else
  {
    struct user_file *file = find_user_file (fd);
    if (file == NULL)
      return -1;
    lock_acquire (&filesys_lock);
    int bytes = file_write (file->file, buffer, size);
    lock_release (&filesys_lock);
    return bytes;
  }
}