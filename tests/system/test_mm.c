/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck_gen_headers/config_mm.h>

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include "devshell.h"
#include "sysenter.h"
#include "test_common.h"

int cmd_brk(int argc, char **argv)
{
   const size_t alloc_size = 1024 * 1024;

   void *orig_brk = (void *)syscall(SYS_brk, 0);
   void *b = orig_brk;

   size_t tot_allocated = 0;

   for (int i = 0; i < 128; i++) {

      void *new_brk = b + alloc_size;

      b = (void *)syscall(SYS_brk, b + alloc_size);

      if (b != new_brk)
         break;

      tot_allocated += alloc_size;
   }

   //printf("tot allocated: %u KB\n", tot_allocated / 1024);

   b = (void *)syscall(SYS_brk, orig_brk);

   if (b != orig_brk) {
      printf("Unable to free mem with brk()\n");
      return 1;
   }

   return 0;
}

int cmd_mmap(int argc, char **argv)
{
   const int iters_count = 10;
   const size_t alloc_size = 1 * MB;
   void *arr[1024];
   int max_mb = -1;

   ull_t tot_duration = 0;

   for (int iter = 0; iter < iters_count; iter++) {

      int i;
      ull_t start = RDTSC();

      for (i = 0; i < 64; i++) {

         errno = 0;

         void *res = mmap(NULL,
                          alloc_size,
                          PROT_READ | PROT_WRITE,
                          MAP_ANONYMOUS | MAP_PRIVATE,
                          -1,
                          0);

         if (res == (void*) -1) {
            i--;
            break;
         }

         arr[i] = res;
      }

      i--;
      tot_duration += (RDTSC() - start);

      if (max_mb < 0) {

         max_mb = i;

      } else {

         if (i != max_mb) {
            printf("[iter: %u] Unable to alloc max_mb (%u) as previous iters\n",
                   iter, max_mb);
            return 1;
         }
      }

      printf("[iter: %u][mmap_test] Mapped %u MB\n", iter, i + 1);

      start = RDTSC();

      for (; i >= 0; i--) {

         int rc = munmap(arr[i], alloc_size);

         if (rc != 0) {
            printf("munmap(%p) failed with error: %s\n",
                   arr[i], strerror(errno));
            return 1;
         }
      }

      tot_duration += (RDTSC() - start);
   }

   printf("\nAvg. cycles for mmap + munmap %u MB: %llu million\n",
          max_mb + 1, (tot_duration / iters_count) / 1000000);

   return 0;
}

static void no_munmap_bad_child(void)
{
   const size_t alloc_size = 128 * KB;

   void *res = mmap(NULL,
                    alloc_size,
                    PROT_READ | PROT_WRITE,
                    MAP_ANONYMOUS | MAP_PRIVATE,
                    -1,
                    0);

   if (res == (void*) -1) {
      printf(STR_CHILD "mmap %d KB failed!\n", alloc_size / KB);
      exit(1);
   }

   /* DO NOT munmap the memory, expecting the kernel to do that! */
   exit(0);
}

int cmd_mmap2(int argc, char **argv)
{
   int child;
   int wstatus;

   child = fork();

   if (!child)
      no_munmap_bad_child();

   waitpid(child, &wstatus, 0);
   return 0;
}

static size_t fork_oom_alloc_size;

static void fork_oom_child(void *buf)
{
   printf("Child [%d]: writing to the whole CoW buffer...\n", getpid());
   memset(buf, 0xBB, fork_oom_alloc_size);
   printf("Child [%d]: done, without failing! [unexpected]\n", getpid());
   exit(0);
}

/*
 * This is a simply code to empirically discover how much memory we can commit
 * at the moment.
 */
static void estimate_usable_mem_child(int rfd, int wfd)
{
   size_t sz = 1 * MB;
   size_t mem = 0;
   int rc;

   printf(STR_CHILD "Pid: %d\n", getpid());

   while (true) {

      char *buf = malloc(sz);
      memset(buf, 'A', sz);
      mem += sz;

      //printf(STR_CHILD "Committed mem: %zu MB\n", mem / MB);
      rc = write(wfd, &mem, sizeof(mem));

      if (rc < 0) {
         printf(STR_CHILD "write on pipe failed: %s\n", strerror(errno));
         break;
      }
   }

   /* We're not supposed to get here */
}

size_t mm_estimate_usable_mem(void)
{
   int rc, pipefd[2];
   int rfd, wfd, wstatus;
   size_t msg, mem = 0;
   pid_t childpid;

   printf(STR_PARENT "Estimating usable memory..\n");

   rc = pipe(pipefd);
   DEVSHELL_CMD_ASSERT(rc >= 0);

   rfd = pipefd[0];
   wfd = pipefd[1];

   childpid = fork();
   DEVSHELL_CMD_ASSERT(childpid >= 0);

   if (!childpid) {
      estimate_usable_mem_child(rfd, wfd);
      exit(0);
   }

   rc = fcntl(rfd, F_SETFL, O_NONBLOCK);

   if (rc < 0) {
      printf(STR_PARENT "fcntl failed: %s\n", strerror(errno));
      goto out;
   }

   while (true) {

      rc = read(rfd, &msg, sizeof(msg));

      if (rc < 0) {

         if (errno == EAGAIN) {

            rc = waitpid(childpid, &wstatus, WNOHANG);

            if (rc < 0) {
               printf(STR_PARENT "waitpid failed: %s\n", strerror(errno));
               break;
            }

            if (rc == childpid) {

               if (WIFEXITED(wstatus)) {
                  printf(STR_PARENT "[unexpected] child exited with: %d\n",
                         WEXITSTATUS(wstatus));
               } else {
                  printf(STR_PARENT "Child killed by signal %d\n",
                         WTERMSIG(wstatus));
               }

               break;
            }

            usleep(50 * 1000);
            continue;
         }

         printf(STR_PARENT "read from pipe failed: %s\n", strerror(errno));
         mem = 0;
         goto out;
      }

      if (rc == 0) {

         if (mem > 0)
            printf(STR_PARENT "read 0\n");
         else
            printf(STR_PARENT "unexpected read 0\n");

         break;
      }

      /* Update the max memory we were able to commit */
      mem = msg;
   }

   if (mem) {
      printf(STR_PARENT "Estimated usable memory: %zu MB\n", mem / MB);
   }

out:
   close(rfd);
   close(wfd);
   return mem;
}

/*
 * Alloc a lot of CoW memory and check that the kernel kills the process in
 * case an attempt to copy a CoW page fails because we're out of memory.
 */
int cmd_fork_oom(int argc, char **argv)
{
   void *buf;
   int rc;

   if (FORK_NO_COW) {
      printf(PFX "[SKIP] because FORK_NO_COW=1\n");
      return 0;
   }

   if (!getenv("TILCK")) {
      printf(PFX "[SKIP] because we're not running on Tilck\n");
      return 0;
   }

   fork_oom_alloc_size = mm_estimate_usable_mem();

   if (!fork_oom_alloc_size) {
      printf("ERROR: unable to estimate usable memory!\n");
      return 1;
   }

   /*
    * Alloc just a bit more than half of the available memory, because in any
    * case it won't be possible both the parent and child process to commit all
    * of that. This makes the test a bit faster ;-)
    */
   fork_oom_alloc_size /= 2;
   fork_oom_alloc_size += 4 * MB;

   printf("Alloc %d MB...\n", fork_oom_alloc_size / MB);
   buf = malloc(fork_oom_alloc_size);

   if (!buf) {
      printf("Alloc of %d MB failed!\n", fork_oom_alloc_size / MB);
      exit(1);
   }

   printf("Write to the buffer...\n");
   memset(buf, 0xAA, fork_oom_alloc_size);
   printf("Done. Now, fork()..\n");

   rc = test_sig(&fork_oom_child, buf, SIGKILL, 0, 0);
   free(buf);
   return rc;
}
