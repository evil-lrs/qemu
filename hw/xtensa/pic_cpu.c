/*
 * Copyright (c) 2011, Max Filippov, Open Source and Linux Lab.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Open Source and Linux Lab nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/atomic.h"

void check_interrupts(CPUXtensaState *env)
{
    CPUState *cs = env_cpu(env);
    int minlevel = xtensa_get_cintlevel(env);
    uint32_t int_set_enabled = env->sregs[INTSET] &
        (env->sregs[INTENABLE] | env->config->inttype_mask[INTTYPE_NMI]);
    int level;

    if (minlevel >= env->config->nmi_level) {
        minlevel = env->config->nmi_level - 1;
    }
    for (level = env->config->nlevel; level > minlevel; --level) {
        if (env->config->level_mask[level] & int_set_enabled) {
            env->pending_irq_level = level;
            cpu_interrupt(cs, CPU_INTERRUPT_HARD);
            qemu_log_mask(CPU_LOG_INT,
                    "%s level = %d, cintlevel = %d, "
                    "pc = %08x, a0 = %08x, ps = %08x, "
                    "intset = %08x, intenable = %08x, "
                    "ccount = %08x\n",
                    __func__, level, xtensa_get_cintlevel(env),
                    env->pc, env->regs[0], env->sregs[PS],
                    env->sregs[INTSET], env->sregs[INTENABLE],
                    env->sregs[CCOUNT]);
            return;
        }
    }
    env->pending_irq_level = 0;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
}

static void xtensa_set_irq(void *opaque, int irq, int active)
{
    CPUXtensaState *env = opaque;
    CPUState *cs = env_cpu(env);

    if (irq >= env->config->ninterrupt) {
        qemu_log("%s: bad IRQ %d\n", __func__, irq);
    } else {
        uint32_t irq_bit = 1 << irq;

        if (active) {
            qatomic_or(&env->sregs[INTSET], irq_bit);
        } else if (env->config->interrupt[irq].inttype == INTTYPE_LEVEL) {
            qatomic_and(&env->sregs[INTSET], ~irq_bit);
        }

        if (irq == 12 || irq == 26) {
            int minlevel = xtensa_get_cintlevel(env);
            bool enabled = env->sregs[INTENABLE] & irq_bit;
            bool deliverable = active && enabled &&
                               env->config->interrupt[irq].level > minlevel;
            qemu_log_mask(CPU_LOG_INT,
                          "XTENSA_IRQ: cpu=%d irq=%d active=%d level=%u "
                          "type=%d enabled=%d deliverable=%d cintlevel=%d "
                          "intset=0x%08x intenable=0x%08x ps=0x%08x "
                          "pc=0x%08x\n",
                          cs->cpu_index, irq, active,
                          env->config->interrupt[irq].level,
                          env->config->interrupt[irq].inttype, enabled,
                          deliverable, minlevel, env->sregs[INTSET],
                          env->sregs[INTENABLE], env->sregs[PS], env->pc);
        }

        check_interrupts(env);
    }
}

static void xtensa_ccompare_cb(void *opaque)
{
    XtensaCcompareTimer *ccompare = opaque;
    CPUXtensaState *env = ccompare->env;
    CPUState *cs = env_cpu(env);
    unsigned i = ccompare - env->ccompare;

    ccompare->cb_log_count++;
    if (ccompare->cb_log_count <= 8 ||
        (ccompare->cb_log_count % 1000) == 0) {
        qemu_log_mask(CPU_LOG_INT,
                      "XTENSA_CCOMPARE: cpu=%d timer=%u irq=%u fire count=%u "
                      "ccount=0x%08x ccompare=0x%08x intset=0x%08x "
                      "intenable=0x%08x pc=0x%08x\n",
                      cs->cpu_index, i, env->config->timerint[i],
                      ccompare->cb_log_count, env->sregs[CCOUNT],
                      env->sregs[CCOMPARE + i], env->sregs[INTSET],
                      env->sregs[INTENABLE], env->pc);
    }
    qemu_set_irq(env->irq_inputs[env->config->timerint[i]], 1);
}

static void xtensa_set_runstall(void *opaque, int irq, int active)
{
    CPUXtensaState *env = opaque;
    xtensa_runstall(env, active);
}

void xtensa_irq_init(CPUXtensaState *env)
{
    unsigned i;

    env->irq_inputs = qemu_allocate_irqs(xtensa_set_irq, env,
                                         env->config->ninterrupt);
    if (xtensa_option_enabled(env->config, XTENSA_OPTION_TIMER_INTERRUPT)) {
        env->time_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        env->ccount_base = env->sregs[CCOUNT];
        for (i = 0; i < env->config->nccompare; ++i) {
            env->ccompare[i].env = env;
            env->ccompare[i].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                    xtensa_ccompare_cb, env->ccompare + i);
        }
    }
    for (i = 0; i < env->config->nextint; ++i) {
        unsigned irq = env->config->extint[i];

        env->ext_irq_inputs[i] = env->irq_inputs[irq];
    }
    env->runstall_irq = qemu_allocate_irq(xtensa_set_runstall, env, 0);
}

qemu_irq *xtensa_get_extints(CPUXtensaState *env)
{
    return env->ext_irq_inputs;
}

qemu_irq xtensa_get_runstall(CPUXtensaState *env)
{
    return env->runstall_irq;
}
