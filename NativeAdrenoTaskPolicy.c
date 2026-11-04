/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoTaskPolicy.h"
#include <kern/task.h>
#include <kern/policy_internal.h>

int NativeAdrenoTaskUsesBackgroundPriority(int *effectiveRole)
{
    const task_t task=current_task();
    const int role=proc_get_effective_task_policy(task,TASK_POLICY_ROLE);
    if(effectiveRole) {
        *effectiveRole=role;
    }

    return proc_get_effective_task_policy(task,TASK_POLICY_DARWIN_BG) ||
        role==TASK_BACKGROUND_APPLICATION || role==TASK_NONUI_APPLICATION ||
        role==TASK_THROTTLE_APPLICATION || role==TASK_DARWINBG_APPLICATION;
}
