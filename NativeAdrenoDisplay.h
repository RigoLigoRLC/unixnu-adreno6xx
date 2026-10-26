/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_DISPLAY_H
#define NATIVE_ADRENO_DISPLAY_H
#include <IOKit/IOTypes.h>
#include <stdint.h>

/* The display captures once after all producer submissions and polls that same
 * value until completion. Each call tries the GPU lock once and returns timeout
 * if it is busy. Capture returns zero when the GPU is unpublished. The caller
 * must keep the surface's read fence active throughout capture, polling and
 * scanout. A failed capture has no usable fence value. */
extern "C" IOReturn native_adreno_capture_surface_producer(uint32_t *captured_fence);

/* Polling performs one hardware observation without sleeping. A completed fence
 * still retires completed jobs and maintains imported memory before success;
 * this CPU work is not bounded by a zero hardware-wait timeout. */
extern "C" IOReturn native_adreno_poll_surface_producer(uint32_t captured_fence);

/* The display caller must receive the surface after its producer has submitted
 * every command buffer, and retain it without further writes until scanout ends.
 * This call captures the shared ring's newest fence, waits for that fence, and
 * retires completed imports with their cache maintenance. It may sleep without
 * holding the GPU lock. The display owns CPU-write publication and scanout.
 * An unpublished GPU succeeds with fence zero. Faults and timeouts return an
 * error; the caller must keep the previous scanout and the pending surface. */
extern "C" IOReturn native_adreno_wait_surface_producer(uint32_t timeout_us,
    uint32_t *captured_fence);

#endif
