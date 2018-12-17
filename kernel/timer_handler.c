/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>

#include <tilck/kernel/process.h>
#include <tilck/kernel/hal.h>
#include <tilck/kernel/irq.h>
#include <tilck/kernel/timer.h>
#include <tilck/kernel/elf_utils.h>

volatile u64 __ticks; /* ticks since the timer started */
volatile u32 disable_preemption_count = 1;

kthread_timer_sleep_obj timers_array[64];

int set_task_to_wake_after(task_info *task, u64 ticks)
{
   DEBUG_ONLY(check_not_in_irq_handler());
   ASSERT(ticks > 0);

   for (uptr i = 0; i < ARRAY_SIZE(timers_array); i++) {
      if (BOOL_COMPARE_AND_SWAP(&timers_array[i].task, NULL, (void *)1)) {
         timers_array[i].task = task;
         timers_array[i].ticks_to_sleep = ticks;
         wait_obj_set(&task->wobj, WOBJ_TIMER, &timers_array[i]);
         return i;
      }
   }

   // TODO: consider implementing a fallback here. For example use a linkedlist.
   panic("Unable to find a free slot in timers_array.");
}

void cancel_timer(int timer_num, task_info *ti)
{
   if (BOOL_COMPARE_AND_SWAP(&timers_array[timer_num].task, ti, NULL)) {
      wait_obj_reset(&ti->wobj);
   }
}

static task_info *tick_all_timers(void)
{
   task_info *last_ready_task = NULL;
   uptr var;

   for (u32 i = 0; i < ARRAY_SIZE(timers_array); i++) {

      /*
       * Ignore 0 (NULL) and 1 as values of task.
       * We need such a check because in set_task_to_wake_after() we temporarely
       * set task to 1, in order to reserve the slot.
       */
      if ((uptr)timers_array[i].task <= 1)
         continue;

      disable_interrupts(&var);

      if (--timers_array[i].ticks_to_sleep == 0) {
         last_ready_task = timers_array[i].task;

         /* In no case a sleeping task could go to kernel and get here */
         ASSERT(get_curr_task() != last_ready_task);

         cancel_timer(i, last_ready_task);
         task_change_state(last_ready_task, TASK_STATE_RUNNABLE);
      }

      enable_interrupts(&var);
   }

   return last_ready_task;
}

void kernel_sleep(u64 ticks)
{
   uptr var;

   if (ticks) {
      disable_interrupts(&var);
      set_task_to_wake_after(get_curr_task(), ticks);
      task_change_state(get_curr_task(), TASK_STATE_SLEEPING);
      enable_interrupts(&var);
   }

   kernel_yield();
}

#if KERNEL_TRACK_NESTED_INTERRUPTS
   static u32 slow_timer_irq_handler_count = 0;

   void print_slow_timer_irq_handler_counter(void)
   {
      printk("slow_timer_irq_handler_counter: %u\n",
             slow_timer_irq_handler_count);
   }
#endif


void debug_check_tasks_lists(void)
{
   task_info *pos, *temp;
   ptrdiff_t off;
   const char *what_str = "?";
   uptr var;

   disable_interrupts(&var);

   list_for_each(pos, temp, &sleeping_tasks_list, sleeping_node) {

      if (pos->state != TASK_STATE_SLEEPING) {

         if (is_kernel_thread(pos))
            what_str = find_sym_at_addr_safe((uptr)pos->what, &off, NULL);

         panic("%s task %d [w: %s] in the sleeping_tasks_list with state: %d",
               is_kernel_thread(pos) ? "kernel" : "user",
               pos->tid, what_str, pos->state);
      }
   }

   enable_interrupts(&var);
}

int timer_irq_handler(regs *context)
{
#if KERNEL_TRACK_NESTED_INTERRUPTS
   if (in_nested_irq0()) {
      slow_timer_irq_handler_count++;
      return 0;
   }
#endif

   __ticks++;
   account_ticks();
   task_info *last_ready_task = tick_all_timers();

   /*
    * Here we have to check that disabled_preemption_count is > 1, not > 0
    * since as the way the handle_irq() is implemented, that counter will be
    * always 1 when this function is called. We must not call schedule()
    * if there has been another part of the code that disabled the preemption.
    */
   if (disable_preemption_count > 1) {
      return 0;
   }

   ASSERT(disable_preemption_count == 1); // again, for us disable = 1 means 0.

   DEBUG_ONLY(debug_check_tasks_lists());

   /*
    * We CANNOT allow the timer to call the scheduler if it interrupted an
    * interrupt handler. Interrupt handlers MUST always to run with preemption
    * disabled.
    *
    * Therefore, the ASSERT checks that:
    *
    * nested_interrupts_count == 1
    *     meaning the timer is the only current interrupt: a kernel or an user
    *     task was running regularly.
    *
    * OR
    *
    * nested_interrupts_count == 2
    *     meaning that the timer interrupted a syscall working with preemption
    *     enabled.
    */

#if defined(DEBUG) && KERNEL_TRACK_NESTED_INTERRUPTS
   {
      uptr var;
      disable_interrupts(&var); /* under #if KERNEL_TRACK_NESTED_INTERRUPTS */
      int c = get_nested_interrupts_count();
      ASSERT(c == 1 || (c == 2 && in_syscall()));
      enable_interrupts(&var);
   }
#endif

   if (last_ready_task) {

      if (get_curr_task()->state == TASK_STATE_RUNNING) {
         task_change_state(get_curr_task(), TASK_STATE_RUNNABLE);
      }

      save_current_task_state(context);
      switch_to_task(last_ready_task, X86_PC_TIMER_IRQ);
   }

   if (need_reschedule()) {
      save_current_task_state(context);
      schedule(X86_PC_TIMER_IRQ);
   }

   return 0;
}

void init_timer(void)
{
   timer_set_freq(TIMER_HZ);
   irq_install_handler(X86_PC_TIMER_IRQ, timer_irq_handler);
}
