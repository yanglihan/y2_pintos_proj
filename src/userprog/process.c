#include "userprog/process.h"
#include "devices/timer.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Child process for parent thread's CHILDREN. This must be on
   the parent's page, because otherwise it will be lost when the
   child thread terminates. STATUS should be -1 at first and
   updated in a call to exit(). */
struct child_proc
{
  struct list_elem elem;      /* For the list CHILDREN in a thread. */
  tid_t tid;                  /* Effectively PID. */
  struct semaphore semaphore; /* Semaphore for process_wait(). */
  int status;                 /* Exit status, default to -1. */
  void **ref;                 /* Reference to the process pointer in thread. */
};

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static bool set_user_stack (char *file_name, char *save_path, void **esp);
static void push_to_user_stack (void **esp, void *src, size_t size);

/* Argument package for start_process(). */
struct child_proc_loader
{
  char *fn;
  struct semaphore semaphore;
  struct child_proc *proc;
  bool success;
};

/* Starts a new thread running a user program loaded from
   FILENAME. The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name)
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /* Extract FILE_NAME. */
  char extracted_fn[NAME_MAX + 1];
  char *tmp;
  strlcpy (extracted_fn, file_name, NAME_MAX + 1);
  strtok_r (extracted_fn, " ", &tmp);

  /* Create a child process. Status is initialized to -1. */
  struct child_proc *proc = malloc (sizeof (struct child_proc));
  proc->status = -1;
  sema_init (&proc->semaphore, 0);
  list_push_back (&thread_current ()->children, &proc->elem);

  /* Create a new thread to execute FILE_NAME. */
  struct child_proc_loader loader;
  loader.fn = fn_copy;
  loader.proc = proc;
  loader.success = false;
  sema_init (&loader.semaphore, 0);
  tid = thread_create (extracted_fn, PRI_DEFAULT, start_process, &loader);
  proc->tid = tid;

  sema_down (&loader.semaphore);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy);
  if (!loader.success)
      tid = TID_ERROR;
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *loader_)
{
  struct child_proc_loader *loader = loader_;
  struct thread *t = thread_current ();
  struct child_proc *p = loader->proc;
  char *file_name = loader->fn;
  char *save_path;
  char *extracted_fn;
  struct intr_frame if_;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  /* Separate file name from argument. */
  extracted_fn = strtok_r (file_name, " ", &save_path);
  loader->success = load (extracted_fn, &if_.eip, &if_.esp);
  if (loader->success)
    loader->success = (set_user_stack (extracted_fn, save_path, &if_.esp));

  /* Link the thread's corresponding child_proc. */
  t->process = p;
  p->ref = &t->process;

  /* Notify process_execute(). */
  sema_up (&loader->semaphore);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!loader->success)
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED ();
}

/* Pushes a copy of src to the user stack. */
static void
push_to_user_stack (void **esp, void *src, size_t size)
{
  *esp -= size;
  memcpy (*esp, src, size);
}
/* Tokenizes FILE_NAME and push arguments to the user stack. */
static bool
set_user_stack (char *file_name, char *save_path, void **esp)
{
  char *token;
  int argc = 1;
  void *sp;
  void *base = *esp;
  char *null_addr = NULL;

  /* Push the file name. */
  push_to_user_stack (esp, file_name, strlen (file_name) + 1);

  /* Push arguments. */
  while ((token = strtok_r (NULL, " ", &save_path)) != NULL)
    {
      argc++;

      /* The PintOS implementation of strtok_r() saves the pointer
         to the final char to SAVE_PTR when reaching the end of
         string. We add the additional 1 (which is !*save_path) in
         this case to get token length. */
      size_t len = (size_t)(save_path - token) + !*save_path;

      /* Check if stack exceeds page limit after pushing.
         Includev 4 extras - RETADDR, ARGC, ARGV and NULL pointer. */
      sp = *esp - len;
      sp = (void *)(((uint32_t)sp >> 2) << 2);
      sp = sp - (argc + 4) * sizeof (void *);
      if (base - sp >= PGSIZE)
        return false;

      push_to_user_stack (esp, token, len);
    }
  sp = *esp;

  /* Round down the address for word-alignment. */
  *esp = (void *)(((uint32_t)*esp >> 2) << 2);
  memset (*esp, 0, sp - *esp);

  /* Push the address of arguments. */
  push_to_user_stack (esp, &null_addr, sizeof (char *));
  for (int i = 0; i < argc; i++)
    {
      push_to_user_stack (esp, &sp, sizeof (char *));
      sp += (strlen (sp) + 1);
    }
  sp = *esp;
  push_to_user_stack (esp, &sp, sizeof (char **));

  /* Push ARGC. */
  push_to_user_stack (esp, &argc, sizeof (int));

  /* Push the return address */
  push_to_user_stack (esp, &null_addr, sizeof (void *));

  return true;
}

/* Waits for thread TID to die and returns its exit status.
 * If it was terminated by the kernel (i.e. killed due to an exception),
 * returns -1.
 * If TID is invalid or if it was not a child of the calling process, or if
 * process_wait() has already been successfully called for the given TID,
 * returns -1 immediately, without waiting.
 *
 * This function will be implemented in task 2.
 * For now, it does nothing. */
int
process_wait (tid_t child_tid)
{
  struct list_elem *e;
  struct list *children = &thread_current ()->children;

  for (e = list_begin (children); e != list_end (children); e = list_next (e))
    {
      struct child_proc *proc = list_entry (e, struct child_proc, elem);
      if (child_tid == proc->tid)
        {
          sema_down (&proc->semaphore);
          int status = proc->status;
          list_remove (&proc->elem);
          free (proc);
          return status;
        }
    }
  return -1;
}

/* Frees the current process's resources and releases saved child
   processes information. */
void
process_exit (void)
{
  struct thread *t = thread_current ();
  uint32_t *pd;
  struct list_elem *e;
  struct list *children = &thread_current ()->children;
  struct list *files = &thread_current ()->files;
  struct child_proc *process = t->process;

  /* Close the running executable file. NULL check and allow write
     already performed by file_close(). */
  file_close (t->exec_file);

  /* Close all opened files. */
  while (!list_empty (files))
    {
      e = list_begin (files);
      struct user_file *file = list_entry (e, struct user_file, elem);
      file_close (file->file);
      list_remove (e);
      free (file);
    }

  /* Release all remaining children information. */
  while (!list_empty (children))
    {
      e = list_begin (children);
      struct child_proc *p = list_entry (e, struct child_proc, elem);
      list_remove (e);
      if (p->ref != NULL)
        {
          *p->ref = NULL;
        }
      free (p);
    }

  if (process != NULL)
    {
      /* Print process exit message. */
      printf ("%s: exit(%d)\n", t->name, process->status);

      /* Prevent parent removing process reference. */
      process->ref = NULL;

      /* Inform parent to retrieve exit status. */
      sema_up (&process->semaphore);
    }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = t->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         t->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      t->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* Passes STATUS back to parent. Does nothing if the parent is
   no longer maintaining the process. */
void
process_pass_status (int status, void *process_)
{
  if (process_ != NULL)
  {
    struct child_proc *process = process_;
    process->status = status;
  }
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
  
  t->exec_file = file;
  file_deny_write (file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      
      /* Check if virtual page already allocated */
      struct thread *t = thread_current ();
      uint8_t *kpage = pagedir_get_page (t->pagedir, upage);
      
      if (kpage == NULL){
        
        /* Get a new page of memory. */
        kpage = palloc_get_page (PAL_USER);
        if (kpage == NULL){
          return false;
        }
        
        /* Add the page to the process's address space. */
        if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }     
        
      } else {
        
        /* Check if writable flag for the page should be updated */
        if(writable && !pagedir_is_writable(t->pagedir, upage)){
          pagedir_set_writable(t->pagedir, upage, writable); 
        }
        
      }

      /* Load data into the page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes){
        return false; 
      }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
