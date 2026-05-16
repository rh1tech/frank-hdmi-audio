/*
 * Header-only specialisation of pico_util's queue for 32-bit elements:
 * drop the size byte that the generic API copies around, and inline the
 * hot push/pop paths so the IRQ handler doesn't take a function call per
 * scanline. Only used internally by frank_dvi.c.
 *
 * (c) 2026 Mikhail Matveev <xtreme@rh1.tech>, https://rh1.tech
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Based on libdvi by Luke Wren and contributors
 * (https://github.com/Wren6991/PicoDVI).
 *
 * Copyright (c) 2021 Luke Wren and contributors.
 */
#ifndef FRANK_QUEUE_INLINE_H_
#define FRANK_QUEUE_INLINE_H_

// Faster versions of the functions found in pico/util/queue.h, for the common
// case of 32-bit-sized elements. Can be used on the same queue data
// structure, and mixed freely with the generic access methods, as long as
// element_size == 4.

#include "pico/util/queue.h"
#include "hardware/sync.h"

static inline uint16_t _queue_inc_index_u32(queue_t *q, uint16_t index) {
    if (++index > q->element_count) { // > because we have element_count + 1 elements
        index = 0;
    }
    return index;
}

static inline bool queue_try_add_u32(queue_t *q, void *data) {
    bool success = false;
    uint32_t flags = spin_lock_blocking(q->core.spin_lock);
    if (queue_get_level_unsafe(q) != q->element_count) {
        ((uint32_t*)q->data)[q->wptr] = *(uint32_t*)data;
        q->wptr = _queue_inc_index_u32(q, q->wptr);
        success = true;
    }
    spin_unlock(q->core.spin_lock, flags);
    if (success) __sev();
    return success;
}

static inline bool queue_try_remove_u32(queue_t *q, void *data) {
    bool success = false;
    uint32_t flags = spin_lock_blocking(q->core.spin_lock);
    if (queue_get_level_unsafe(q) != 0) {
        *(uint32_t*)data = ((uint32_t*)q->data)[q->rptr];
        q->rptr = _queue_inc_index_u32(q, q->rptr);
        success = true;
    }
    spin_unlock(q->core.spin_lock, flags);
    if (success) __sev();
    return success;
}

static inline bool queue_try_peek_u32(queue_t *q, void *data) {
    bool success = false;
    uint32_t flags = spin_lock_blocking(q->core.spin_lock);
    if (queue_get_level_unsafe(q) != 0) {
        *(uint32_t*)data = ((uint32_t*)q->data)[q->rptr];
        success = true;
    }
    spin_unlock(q->core.spin_lock, flags);
    return success;
}

static inline void queue_add_blocking_u32(queue_t *q, void *data) {
    bool done;
    do {
        done = queue_try_add_u32(q, data);
        if (done) break;
        __wfe();
    } while (true);
}

static inline void queue_remove_blocking_u32(queue_t *q, void *data) {
    bool done;
    do {
        done = queue_try_remove_u32(q, data);
        if (done) break;
        __wfe();
    } while (true);
}

static inline void queue_peek_blocking_u32(queue_t *q, void *data) {
    bool done;
    do {
        done = queue_try_peek_u32(q, data);
        if (done) break;
        __wfe();
    } while (true);
}

#endif
