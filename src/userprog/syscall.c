#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include <stdio.h>
#include <list.h>
#include <syscall-nr.h>
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
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
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);
static void process_termination_msg (char *name, int code);
static struct user_file *find_user_file(int fd);

struct list open_files;             /* List of all opened files. */

static struct lock filesys_lock;    /* Lock for the file system. */
static struct lock file_lock;       /* Lock for OPEN_FILES. */
static int cur_fd = 2;              /* Current file descriptor. Skips STDIN and
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
  printf("%s: exit(%d)\n", name, code);
}

/* Finds the file with given file descriptor in OPEN_FILES. Returns NULL if the
  file was not found. */
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

static bool
is_user_ptr_valid (const void *ptr)
{
  uint32_t *pd = thread_current ()->pagedir;
  return (ptr != NULL) && (is_user_vaddr (ptr)) &&
         (pagedir_get_page (pd, ptr) != NULL);
}

/* return the pointer to the end of the file name.
   return NULL if the length of file name is larger then NAME_MAX */
static const char *
find_file_name_end (const void *file)
{
  ASSERT (file != NULL);
  int len = 0;
  const char *end;
  for (end = file; (len < NAME_MAX) && (*end != '\0'); end++)
    len++;
  return (*end == '\0') ? end : NULL;
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
  int syscall_num = *((int *) f->esp);
  void *param1 = f->esp + 4;
  void *param2 = f->esp + 8;
  void *param3 = f->esp + 12;

  unsigned *retval = &f->eax;

  switch (syscall_num)
    { 
      case SYS_HALT:
        halt();
        break;
      case SYS_EXIT:
        exit (*((int *) param1));
        break;
      case SYS_CREATE:
        *retval = create (*((char **) param1), *((unsigned *) param2));
        break;
      case SYS_REMOVE:
        *retval = remove (*((char **) param1));
        break;
      case SYS_FILESIZE:
        *retval = filesize (*((int *) param1));
        break;
      case SYS_OPEN:
        *retval = open (*((char **) param1));
        break;
      case SYS_READ:
        *retval = read (*((int *) param1), *((void **) param2), *((unsigned *) param3));
        break;
      case SYS_WRITE:
        *retval = write (*((int *) param1), *((void **) param2), *((unsigned *) param3));
        break;
      case SYS_CLOSE:
        close (*((int *) param1));
        break;
      case SYS_TELL:
        *retval = tell (*((int *) param1));
        break;
      case SYS_SEEK:
        seek (*((int *) param1), *((unsigned *) param2));
        break;
    }
}

/* Terminates PintOS. */
static void
halt (void)
{
  shutdown_power_off();
}

/* Terminates teh current user program. */
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
  if (!is_user_ptr_valid (file))
    exit (-1);
  const char *end = find_file_name_end (file);
  if (end == NULL)
    return false;
  if (!is_user_ptr_valid (end))
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
  if (!is_user_ptr_valid (file))
    exit (-1);
  const char *end = find_file_name_end (file);
  if (end == NULL)
    return false;
  if (!is_user_ptr_valid (end))
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
  if (!is_user_ptr_valid (file))
    exit (-1);
  const char *end = find_file_name_end (file);
  if (end == NULL)
    return -1;
  if (!is_user_ptr_valid (end))
    exit (-1);

  lock_acquire (&filesys_lock);
  struct file *ret_file = filesys_open (file);
  lock_release (&filesys_lock);

  if (ret_file == NULL)
    return -1;
  
  /* Generates a new file descriptor and add the file to OPEN_FILES. */
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
  if (fd == STDIN_FILENO)
    {
      /* Reads from STDIN. Always reads the full size. */
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

/* Write SIZE bites to an opened file. Returns the size actually written. Also
  works for STDOUT. */
static int
write (int fd, const void *buffer, unsigned size)
{
  if (fd == STDOUT_FILENO)
  {
    /* Writes to STDOUT. Always writes the full size. */
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
  file_seek(file->file, position);
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
  lock_try_acquire (&filesys_lock);

  lock_acquire (&file_lock);
  list_remove (&file->elem);
  lock_release (&file_lock);

  free (file);
}
