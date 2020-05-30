# SPDX-License-Identifier: BSD-2-Clause
# pylint: disable=unused-wildcard-import

import gdb # pylint: disable=import-error
from .base_utils import *

def get_all_tasks():

   tasks = []
   root = gdb.lookup_symbol("tree_by_tid_root")[0]

   def walk(tasks_list, task):

      if int(task) == 0:
         return

      tasks_list.append(task)

      left = task['tree_by_tid_node']['left_obj'].cast(type_task_p)
      right = task['tree_by_tid_node']['right_obj'].cast(type_task_p)

      walk(tasks_list, left)
      walk(tasks_list, right)

   walk(tasks, root.value())
   tasks = sorted(tasks, key = lambda t: int(t['tid']))
   return tasks

def get_children_list(proc):

   children_list = proc['children'].address
   curr = children_list.cast(list_node_p)['next']
   res = []

   while int(curr) != int(children_list):

      obj = container_of(int(curr), "struct task", "siblings_node")
      res.append(obj)
      curr = curr['next']

   return res


def get_task(tid):

   tasks = get_all_tasks()

   for t in tasks:

      task_tid = int(t['tid'])

      if task_tid == tid:
         return t

   return None

def get_proc(pid):

   tasks = get_all_tasks()

   for t in tasks:

      pi = t['pi']

      if pi['pid'] == pid:
         return pi

   return None
