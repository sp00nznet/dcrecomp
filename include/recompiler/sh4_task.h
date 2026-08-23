/**
 * Cooperative SH-4 tasks.
 *
 * Recompiled functions are ordinary C functions running on the host stack, so a
 * routine that suspends itself and resumes later cannot be expressed as a plain
 * call and return. Katana SDK code does exactly that: a driver starts an
 * operation, then yields so the rest of the system can drive it to completion,
 * and resumes where it left off. Translated naively that becomes a function
 * which returns to the wrong place - or, if the yield is stubbed out, a wait
 * loop nothing can ever satisfy.
 *
 * Giving each SH-4 task its own host stack makes a yield mean what it means on
 * hardware. Windows fibers provide that directly; elsewhere the API degrades to
 * running the task inline, where a yield simply returns and the caller must
 * poll instead.
 *
 * Typical use from a game bootstrap:
 *
 *     sh4_task_init();
 *     SH4Task *game = sh4_task_spawn(func_8C0898A0, &cpu);
 *     while (!sh4_task_finished(game)) {
 *         sh4_task_resume(game);      // runs until it yields or returns
 *         pump_vblank_and_input();    // what the task yielded for
 *     }
 */

#ifndef SH4_TASK_H
#define SH4_TASK_H

#include "recompiler/sh4_cpu.h"

typedef struct SH4Task SH4Task;

/* Make the calling thread the scheduler. Call once before spawning anything.
 * Returns false if the platform has no fiber support, in which case tasks still
 * work but run inline and sh4_task_yield() is a no-op. */
bool sh4_task_init(void);

/* Create a task that will run entry(cpu) on its own stack. Nothing executes
 * until the first sh4_task_resume(). */
SH4Task *sh4_task_spawn(void (*entry)(SH4CPU *cpu), SH4CPU *cpu);

/* Run a task until it yields or returns. Returns false once it has finished.
 * Only the scheduler calls this. */
bool sh4_task_resume(SH4Task *task);

/* Suspend the running task and return to the scheduler. Called from inside a
 * task - typically from a game's own yield routine. No-op outside a task. */
void sh4_task_yield(void);

bool sh4_task_finished(const SH4Task *task);

/* The task currently running, or NULL if this is the scheduler. */
SH4Task *sh4_task_current(void);

void sh4_task_destroy(SH4Task *task);
void sh4_task_shutdown(void);

#endif /* SH4_TASK_H */
