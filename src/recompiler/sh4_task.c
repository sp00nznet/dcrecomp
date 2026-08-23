/**
 * Cooperative SH-4 tasks on host fibers. See sh4_task.h.
 */

#include "recompiler/sh4_task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define HAVE_FIBERS 1
#else
#define HAVE_FIBERS 0
#endif

/* Recompiled call chains get deep - Crazy Taxi links with a 64MB stack - so a
 * task stack has to be sized like the main one, not like a thread pool's. */
#define TASK_STACK_RESERVE (64u * 1024u * 1024u)
#define TASK_STACK_COMMIT  (1u * 1024u * 1024u)

/* The architectural state a switch has to carry. Same set the interrupt path
 * saves: whatever the task had live in cpu->r[] belongs to that task, because
 * recompiled code keeps everything there rather than in C locals. */
typedef struct {
    uint32_t r[16];
    uint32_t pr, sr, gbr, mach, macl, fpul, fpscr;
    float fr[16], xf[16];
} TaskRegs;

struct SH4Task {
    void *fiber;
    void (*entry)(SH4CPU *cpu);
    SH4CPU *cpu;
    bool started;
    bool finished;
    TaskRegs saved;
};

static void *g_scheduler_fiber = NULL;
static SH4Task *g_current = NULL;
static bool g_have_fibers = false;

static void regs_save(TaskRegs *f, const SH4CPU *cpu) {
    memcpy(f->r, cpu->r, sizeof f->r);
    f->pr = cpu->pr; f->sr = cpu->sr; f->gbr = cpu->gbr;
    f->mach = cpu->mach; f->macl = cpu->macl;
    f->fpul = cpu->fpul; f->fpscr = cpu->fpscr;
    memcpy(f->fr, cpu->fr, sizeof f->fr);
    memcpy(f->xf, cpu->xf, sizeof f->xf);
}

static void regs_restore(const TaskRegs *f, SH4CPU *cpu) {
    memcpy(cpu->r, f->r, sizeof f->r);
    cpu->pr = f->pr; cpu->sr = f->sr; cpu->gbr = f->gbr;
    cpu->mach = f->mach; cpu->macl = f->macl;
    cpu->fpul = f->fpul; cpu->fpscr = f->fpscr;
    memcpy(cpu->fr, f->fr, sizeof f->fr);
    memcpy(cpu->xf, f->xf, sizeof f->xf);
}

bool sh4_task_init(void) {
#if HAVE_FIBERS
    if (g_scheduler_fiber)
        return true;
    /* ConvertThreadToFiber fails if the thread already is one, which is fine. */
    g_scheduler_fiber = ConvertThreadToFiber(NULL);
    if (!g_scheduler_fiber)
        g_scheduler_fiber = GetCurrentFiber();
    g_have_fibers = (g_scheduler_fiber != NULL);
#else
    g_have_fibers = false;
#endif
    printf("[TASK] cooperative tasks: %s\n",
           g_have_fibers ? "fibers" : "inline (yield is a no-op)");
    return g_have_fibers;
}

#if HAVE_FIBERS
static VOID CALLBACK task_trampoline(LPVOID param) {
    SH4Task *task = (SH4Task *)param;
    task->entry(task->cpu);
    task->finished = true;
    /* A task that returns must hand control back for good. */
    for (;;)
        SwitchToFiber(g_scheduler_fiber);
}
#endif

SH4Task *sh4_task_spawn(void (*entry)(SH4CPU *cpu), SH4CPU *cpu) {
    SH4Task *task = (SH4Task *)calloc(1, sizeof(SH4Task));
    if (!task)
        return NULL;
    task->entry = entry;
    task->cpu = cpu;
#if HAVE_FIBERS
    if (g_have_fibers) {
        task->fiber = CreateFiberEx(TASK_STACK_COMMIT, TASK_STACK_RESERVE, 0,
                                    task_trampoline, task);
        if (!task->fiber) {
            printf("[TASK] CreateFiberEx failed (%lu) - running inline\n",
                   (unsigned long)GetLastError());
        }
    }
#endif
    return task;
}

bool sh4_task_resume(SH4Task *task) {
    if (!task || task->finished)
        return false;

#if HAVE_FIBERS
    if (task->fiber) {
        TaskRegs scheduler;
        regs_save(&scheduler, task->cpu);
        if (task->started)
            regs_restore(&task->saved, task->cpu);

        SH4Task *prev = g_current;
        g_current = task;
        task->started = true;
        SwitchToFiber(task->fiber);
        g_current = prev;

        /* Back from the task: it either yielded or returned. Either way the
         * scheduler's own view of the registers is what should be live here. */
        regs_restore(&scheduler, task->cpu);
        return !task->finished;
    }
#endif

    /* No fibers: run it straight through. A yield inside just returns. */
    SH4Task *prev = g_current;
    g_current = task;
    task->started = true;
    task->entry(task->cpu);
    task->finished = true;
    g_current = prev;
    return false;
}

void sh4_task_yield(void) {
#if HAVE_FIBERS
    SH4Task *task = g_current;
    if (task && task->fiber && g_scheduler_fiber) {
        regs_save(&task->saved, task->cpu);
        SwitchToFiber(g_scheduler_fiber);
        /* Resumed: sh4_task_resume has already put our registers back. */
        return;
    }
#endif
    /* Nothing to yield to. */
}

bool sh4_task_finished(const SH4Task *task) {
    return !task || task->finished;
}

SH4Task *sh4_task_current(void) {
    return g_current;
}

void sh4_task_destroy(SH4Task *task) {
    if (!task)
        return;
#if HAVE_FIBERS
    if (task->fiber)
        DeleteFiber(task->fiber);
#endif
    free(task);
}

void sh4_task_shutdown(void) {
#if HAVE_FIBERS
    if (g_scheduler_fiber) {
        ConvertFiberToThread();
        g_scheduler_fiber = NULL;
    }
#endif
    g_have_fibers = false;
    g_current = NULL;
}
