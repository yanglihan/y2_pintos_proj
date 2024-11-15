#include "userprog/syscall.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include <list.h>
#include <stdio.h>
#include <syscall-nr.h>

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
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);
static pid_t exec (const char *cmd_line);
static int wait (pid_t pid);
static void process_termination_msg (char *name, int code);
static struct user_file *find_user_file (int fd);

static bool is_mem_valid (const void *ptr, size_t size);
static bool is_str_mem_valid (const char *ptr, size_t size);

struct list open_files;          /* List of all opened files. */
static struct lock filesys_lock; /* Lock for the file system. */
static struct lock file_lock;    /* Lock for OPEN_FILES. */
static int cur_fd = 2;           /* Current file descriptor. Skips STDIN and
                                   STDOUT. */

/* A file opened by a user program. */
struct user_file
{
  int fd;
  struct file *file;
  struct list_elem elem;
};

/* Prints a process termination message. */
static void
process_termination_msg (char *name, int code)
{
  printf ("%s: exit(%d)\n", name, code);
}

/* Finds the file with given file descriptor in OPEN_FILES. Returns NULL if the
  file was not found. */
static struct user_file *
find_user_file (int fd)
{
  lock_acquire (&file_lock);
  struct list_elem *e;
  for (e = list_begin (&open_files); e != list_end (&open_files);
       e = list_next (e))
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

/* Returns if the memory slice of size SIZE is valid at PTR. */
static bool
is_mem_valid (const void *ptr, size_t size)
{
  uint32_t *pd = thread_current ()->pagedir;
  return (ptr != NULL) && (is_user_vaddr (ptr))
         && (pagedir_get_page (pd, ptr) != NULL)
         && (is_user_vaddr (ptr + size - 1))
         && (pagedir_get_page (pd, ptr + size - 1) != NULL);
}

/* Returns if the string at most of size SIZE is valid at PTR. This function
  uses a loop and should be used sparingly. */
static bool
is_str_mem_valid (const char *ptr, size_t size)
{
  uint32_t *pd = thread_current ()->pagedir;
  if (ptr == NULL)
    return false;
  for (char *p = ptr; p < ptr + size; p++)
    {
      if (is_user_vaddr (p) && (pagedir_get_page (pd, p) != NULL))
        {
          if (*p == '\0')
            return true;
        }
      else
        {
          return false;
        }
    }
  return true;
}

void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&file_lock);
  lock_init (&filesys_lock);
  list_init (&open_files);
}

/* Handles a system call. */
static void
syscall_handler (struct intr_frame *f)
{
  if (!is_mem_valid (f->esp, 4))
    exit (-1);
  int syscall_num = *((int *)f->esp);
  void *param1 = f->esp + 4;
  void *param2 = f->esp + 8;
  void *param3 = f->esp + 12;

  unsigned *retval = &f->eax;

  switch (syscall_num)
    {
    case SYS_HALT:
      halt ();
      break;
    case SYS_EXIT:
      if (!is_mem_valid (f->esp, 8))
        exit (-1);
      exit (*((int *)param1));
      break;
    case SYS_CREATE:
      if (!is_mem_valid (f->esp, 12))
        exit (-1);
      *retval = create (*((char **)param1), *((unsigned *)param2));
      break;
    case SYS_REMOVE:
      if (!is_mem_valid (f->esp, 8))
        exit (-1);
      *retval = remove (*((char **)param1));
      break;
    case SYS_FILESIZE:
      if (!is_mem_valid (f->esp, 8))
        exit (-1);
      *retval = filesize (*((int *)param1));
      break;
    case SYS_OPEN:
      if (!is_mem_valid (f->esp, 8))
        exit (-1);
      *retval = open (*((char **)param1));
      break;
    case SYS_READ:
      if (!is_mem_valid (f->esp, 16))
        exit (-1);
      *retval
          = read (*((int *)param1), *((void **)param2), *((unsigned *)param3));
      break;
    case SYS_WRITE:
      if (!is_mem_valid (f->esp, 16))
        exit (-1);
      *retval = write (*((int *)param1), *((void **)param2),
                       *((unsigned *)param3));
      break;
    case SYS_CLOSE:
      if (!is_mem_valid (f->esp, 8))
        exit (-1);
      close (*((int *)param1));
      break;
    case SYS_TELL:
      if (!is_mem_valid (f->esp, 8))
        exit (-1);
      *retval = tell (*((int *)param1));
      break;
    case SYS_SEEK:
      if (!is_mem_valid (f->esp, 16))
        exit (-1);
      seek (*((int *)param1), *((unsigned *)param2));
      break;
    case SYS_EXEC:
      if (!is_mem_valid (f->esp, 4))
        exit (-1);
      exec (*((char **)param1));
    }
}

/* Terminates PintOS. */
static void
halt (void)
{
  shutdown_power_off ();
}

/* Terminates the current user program. */
static void
exit (int status)
{
  struct thread *t = thread_current ();

  process_termination_msg (t->name, status);

  thread_exit ();
}

/* Creates a file. */
static bool
create (const char *file, unsigned initial_size)
{
  if (!is_str_mem_valid (file, sizeof (char) * 16))
    exit (-1);

  lock_acquire (&filesys_lock);
  bool is_created = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return is_created;
}

/* Removes a file. */
static bool
remove (const char *file)
{
  if (!is_str_mem_valid (file, sizeof (char) * 16))
    exit (-1);

  lock_acquire (&filesys_lock);
  bool is_removed = filesys_remove (file);
  lock_release (&filesys_lock);
  return is_removed;
}

/* Get the size of a file. */
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

/* Opens a file. Returns the file descriptor or -1 if the file could not be
  opened. */
static int
open (const char *file)
{
  if (!is_str_mem_valid (file, sizeof (char) * 16))
    exit (-1);

  lock_acquire (&filesys_lock);
  struct file *ret_file = filesys_open (file);
  lock_release (&filesys_lock);

  if (ret_file == NULL)
    return -1;

  /* Generate a new file descriptor and add the file to OPEN_FILES. */
  struct user_file *new_file = calloc (1, sizeof (struct user_file));
  lock_acquire (&file_lock);
  new_file->fd = cur_fd++;
  new_file->file = ret_file;
  list_push_front (&open_files, &new_file->elem);
  lock_release (&file_lock);

  return new_file->fd;
}

/* Reads SIZE bites from an opened file. Returns the size actually read or -1
  if the file could not be read. Also works for STDIN. */
static int
read (int fd, void *buffer, unsigned size)
{
  if (!is_str_mem_valid (buffer, size))
    exit (-1);
  if (fd == STDIN_FILENO)
    {
      /* Read from STDIN. Always reads the full size. */
      for (unsigned i = 0; i < size; i++)
        *(uint8_t *)(buffer + i) = input_getc ();
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

/* Write SIZE bites to an opened file. Returns the size actually written. Also
  works for STDOUT. */
static int
write (int fd, const void *buffer, unsigned size)
{
  if (!is_str_mem_valid (buffer, size))
    exit (-1);
  if (fd == STDOUT_FILENO)
    {
      /* Write to STDOUT. Always writes the full size. */
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

/* Changes the position in an opened file. Can reach pass current EOF. */
static void
seek (int fd, unsigned position)
{
  struct user_file *file = find_user_file (fd);
  if (file == NULL)
    return;

  lock_acquire (&filesys_lock);
  file_seek (file->file, position);
  lock_release (&filesys_lock);
}

/* Returns the position in an opened file. An invalid FD returns 0. */
static unsigned
tell (int fd)
{
  struct user_file *file = find_user_file (fd);
  if (file == NULL)
    return 0;

  lock_acquire (&filesys_lock);
  unsigned pos = file_tell (file->file);
  lock_release (&filesys_lock);

  return pos;
}

/* Closes an opened file. */
static void
close (int fd)
{
  struct user_file *file = find_user_file (fd);
  if (file == NULL)
    return;

  lock_acquire (&filesys_lock);
  file_close (file->file);
  lock_release (&filesys_lock);

  lock_acquire (&file_lock);
  list_remove (&file->elem);
  lock_release (&file_lock);

  free (file);
}

static pid_t 
exec (const char *cmd_line)
{
  struct thread *t = thread_current ();
  tid_t tid = process_execute (cmd_line);
  if (!t->process->is_load)
    return -1;
  return tid;
}

static int
wait (pid_t pid)
{

}
