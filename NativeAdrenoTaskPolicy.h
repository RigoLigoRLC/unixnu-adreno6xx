/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_TASK_POLICY_H
#define NATIVE_ADRENO_TASK_POLICY_H

#ifdef __cplusplus
extern "C" {
#endif

/* The submitting task's effective role and Darwin background state select
 * the connection's background priority when this function returns nonzero.
 * effectiveRole receives the XNU task role for diagnostic records. */
int NativeAdrenoTaskUsesBackgroundPriority(int *effectiveRole);

#ifdef __cplusplus
}
#endif
#endif
