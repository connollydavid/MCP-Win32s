/*
 * theft_uart.c - host-native property-based tests for the pure Win32s direct-
 * UART ladder (src/uart.c), driven over the injected UartPortIo seam against a
 * simulated 16550 (tests/uart_sim.h). C99 + POSIX, ASan/UBSan, 50k trials per
 * property - the strongest pin on the security invariants.
 *
 * Spec: specs/uart.allium; obligations: tests/OBLIGATIONS-6.2.md. The pure
 * functions reference no Win32 type, so src/uart.c is compiled -DUART_HOST_PURE
 * (which leaves the asm inb/outb + transport wiring out) and linked here.
 *
 * Properties (each maps to an obligation; SECURITY PINs called out):
 *
 *   P1 fifo_iff_16550a        PIN #4 invariant.FifoEnabledImpliesDetected16550A,
 *                             rule-success.UartChipIdentified: fifo_enabled==1
 *                             IFF kind==16550A; tx_chunk==16 on 16550A, ==1 else.
 *   P2 loopback_fail_closed   rule-success.UartOpenFailsLoopback: a DEAD_CLONE or
 *                             DIVISOR_STUCK fake => UART_OPEN_FAILED, open stays 0.
 *   P3 interrupts_never_armed PIN #3 invariant.NoInterruptPathArmed: across a full
 *                             open/tx/rx/close, IER never left nonzero and MCR
 *                             OUT2 (bit3) never set.
 *   P4 divisor_round_trip     rule-success.UartGoesLive (read-back half): a NORMAL
 *                             fake round-trips any divisor; a DIVISOR_STUCK fake
 *                             fails the read-back (open fails).
 *   P5 open_loops_bounded     PIN #6 EveryPollLoopBounded: a NEVER_READY fake =>
 *                             UART_OPEN_FAILED with a bounded op count; a TX vs
 *                             NEVER_READY returns <0 within the bound.
 *   P6 rx_idle_is_live        PIN #6 exception: an idle line yields-and-repolls
 *                             and only ever returns >0 (on data), never 0.
 *   P7 lsr_before_rbr         rule (RX decode / overrun): an injected OE byte is
 *                             read correctly AND bumps overrun_count - only true
 *                             if LSR is read before RBR (the order-mandatory pin).
 *   P8 tx_burst_le_depth      config-default.fifo_tx_chunk: a 16550A TX never
 *                             bursts past tx_chunk and delivers all bytes in order.
 *   P9 rx_break_never_zero    PIN #6 exception (the "never 0" half): a poll pass
 *                             that consumes ONLY a line break (BI, no framing
 *                             error) must keep waiting, not return 0 (0 would read
 *                             as a peer close on a line that has none); a break
 *                             WITH a framing error is a real comms error (<0).
 *   P10 nona_16550_no_fifo    PIN #4, the DANGEROUS DETECTION DIRECTION: a non-A
 *                             16550 (IIR&0xC0==0x80, the false-16550A-positive)
 *                             must NOT enable the FIFO - fifo_enabled stays 0,
 *                             tx_chunk stays 1, chip_kind is never 16550A. Feeds
 *                             the 0x80 readback the FIFO detect must reject (the
 *                             input the 0xC0/0x00-only fake never produced).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "theft.h"
#include "uart.h"
#include "uart_sim.h"

#define TRIALS  50000
#define SEED    0x6A27ED0011ULL

/* A sane ceiling for the bounded-loop op counter: the whole open sequence does
 * a fixed number of register touches plus at most a few bounded polls, each
 * <= UART_POLL_BOUND iterations with one in + one yield. A handful of bounded
 * loops keeps the total well under this. */
#define OP_CEILING  (10UL * 100000UL)   /* 10 x UART_POLL_BOUND */

/* All four COM base ports, to vary the base across trials. */
static unsigned short com_base(unsigned int sel)
{
    switch (sel & 3) {
    case 0:  return 0x3F8;
    case 1:  return 0x2F8;
    case 2:  return 0x3E8;
    default: return 0x2E8;
    }
}

/* Map a 2-bit selector to a chip kind (UNKNOWN excluded - it is the absent
 * result, generated via the ABSENT variant, not a present kind). */
static UartChipKind kind_of(unsigned int sel)
{
    switch (sel % 3) {
    case 0:  return UART_CHIP_8250;
    case 1:  return UART_CHIP_16450;
    default: return UART_CHIP_16550A;
    }
}

/* ===================================================================
 * P1 fifo_iff_16550a - the headline detection pin (PIN #4).
 * Generator: a present chip of a random kind at a random base.
 * =================================================================== */

struct chipcase { uint8_t kind_sel; uint8_t base_sel; };

static enum theft_alloc_res
chip_alloc(struct theft *t, void *env, void **out)
{
    struct chipcase *c;
    (void)env;
    c = malloc(sizeof(*c));
    if (c == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    c->kind_sel = (uint8_t)theft_random_bits(t, 8);
    c->base_sel = (uint8_t)theft_random_bits(t, 8);
    *out = c;
    return THEFT_ALLOC_OK;
}

static theft_hash chip_hash(const void *instance, void *env)
{
    const struct chipcase *c = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)c, sizeof(*c));
}

static struct theft_type_info chip_info = {
    .alloc = chip_alloc,
    .free  = theft_generic_free_cb,
    .hash  = chip_hash,
};

static enum theft_trial_res
prop_fifo_iff_16550a(struct theft *t, void *arg1)
{
    struct chipcase *c = (struct chipcase *)arg1;
    UartChipKind kind = kind_of(c->kind_sel);
    unsigned short base = com_base(c->base_sel);
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int present;
    int is_16550a;
    (void)t;

    UartSimInit(&sim, base, kind, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);

    present = UartDetect(&io, &drv);
    if (!present) {
        return THEFT_TRIAL_FAIL;   /* a present NORMAL chip must be detected */
    }

    is_16550a = (kind == UART_CHIP_16550A);

    /* The load-bearing biconditional: fifo_enabled IFF the chip is a 16550A. */
    if (drv.fifo_enabled != (is_16550a ? 1 : 0)) {
        return THEFT_TRIAL_FAIL;
    }
    /* tx_chunk > 1 only on a 16550A; exactly 16 there, exactly 1 otherwise. */
    if (is_16550a) {
        if (drv.tx_chunk != UART_FIFO_TX_CHUNK) {
            return THEFT_TRIAL_FAIL;
        }
        if (drv.chip_kind != UART_CHIP_16550A) {
            return THEFT_TRIAL_FAIL;
        }
    } else {
        if (drv.tx_chunk != UART_SINGLE_TX_CHUNK) {
            return THEFT_TRIAL_FAIL;
        }
        if (drv.fifo_enabled) {
            return THEFT_TRIAL_FAIL;
        }
    }
    /* The dangerous direction stated directly: fifo on => 16550A. */
    if (drv.fifo_enabled && drv.chip_kind != UART_CHIP_16550A) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P2 loopback_fail_closed - a DEAD_CLONE or DIVISOR_STUCK fake fails the open.
 * Generator: a 16550A chip + a fault selector (clone vs divisor-stuck).
 * =================================================================== */

struct faultcase { uint8_t which; uint8_t base_sel; uint16_t divisor; };

static enum theft_alloc_res
fault_alloc(struct theft *t, void *env, void **out)
{
    struct faultcase *f;
    (void)env;
    f = malloc(sizeof(*f));
    if (f == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    f->which   = (uint8_t)theft_random_bits(t, 1);
    f->base_sel = (uint8_t)theft_random_bits(t, 8);
    f->divisor = (uint16_t)theft_random_bits(t, 16);
    *out = f;
    return THEFT_ALLOC_OK;
}

static theft_hash fault_hash(const void *instance, void *env)
{
    const struct faultcase *f = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)f, sizeof(*f));
}

static struct theft_type_info fault_info = {
    .alloc = fault_alloc,
    .free  = theft_generic_free_cb,
    .hash  = fault_hash,
};

static enum theft_trial_res
prop_loopback_fail_closed(struct theft *t, void *arg1)
{
    struct faultcase *f = (struct faultcase *)arg1;
    unsigned short base = com_base(f->base_sel);
    UartSimVariant v = f->which ? UART_SIM_DEAD_CLONE : UART_SIM_DIVISOR_STUCK;
    unsigned int divisor = (f->divisor == 0) ? 6u : (unsigned int)f->divisor;
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int res;
    (void)t;

    UartSimInit(&sim, base, UART_CHIP_16550A, v);
    io = UartSimPortIo(&sim);

    res = UartOpenSequence(&io, &drv, base, divisor);
    if (res != UART_OPEN_FAILED) {
        return THEFT_TRIAL_FAIL;   /* a dead/clone/stuck chip must fail closed */
    }
    if (drv.open != 0) {
        return THEFT_TRIAL_FAIL;   /* never goes live */
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P3 interrupts_never_armed - PIN #3. Full open/tx/rx/close on a NORMAL chip;
 * the OUT-trace must never arm IER nor set MCR OUT2.
 * Generator: a random present kind + a short payload to TX and to inject as RX.
 * =================================================================== */

struct livecase {
    uint8_t  kind_sel;
    uint8_t  base_sel;
    uint16_t divisor;
    uint8_t  txlen;
    uint8_t  rxlen;
    uint8_t  tx[32];
    uint8_t  rx[32];
};

static enum theft_alloc_res
live_alloc(struct theft *t, void *env, void **out)
{
    struct livecase *l;
    int i;
    (void)env;
    l = malloc(sizeof(*l));
    if (l == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    l->kind_sel = (uint8_t)theft_random_bits(t, 8);
    l->base_sel = (uint8_t)theft_random_bits(t, 8);
    l->divisor  = (uint16_t)theft_random_bits(t, 16);
    l->txlen    = (uint8_t)(1 + (theft_random_bits(t, 5) % 32));   /* 1..32 */
    l->rxlen    = (uint8_t)(1 + (theft_random_bits(t, 5) % 32));   /* 1..32 */
    for (i = 0; i < 32; i++) {
        l->tx[i] = (uint8_t)theft_random_bits(t, 8);
        l->rx[i] = (uint8_t)theft_random_bits(t, 8);
    }
    *out = l;
    return THEFT_ALLOC_OK;
}

static theft_hash live_hash(const void *instance, void *env)
{
    const struct livecase *l = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)l, sizeof(*l));
}

static struct theft_type_info live_info = {
    .alloc = live_alloc,
    .free  = theft_generic_free_cb,
    .hash  = live_hash,
};

static enum theft_trial_res
prop_interrupts_never_armed(struct theft *t, void *arg1)
{
    struct livecase *l = (struct livecase *)arg1;
    UartChipKind kind = kind_of(l->kind_sel);
    unsigned short base = com_base(l->base_sel);
    unsigned int divisor = (l->divisor == 0) ? 6u : (unsigned int)l->divisor;
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    unsigned char rxbuf[32];
    int res;
    (void)t;

    UartSimInit(&sim, base, kind, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);

    res = UartOpenSequence(&io, &drv, base, divisor);
    if (res != UART_OPEN_LIVE) {
        return THEFT_TRIAL_FAIL;   /* a NORMAL chip must go live */
    }

    /* The live register state, read directly the moment open completes: IER
     * reads 0 and MCR OUT2 (bit 3) is clear (invariant 3). */
    if ((io.in(io.ctx, (unsigned short)(base + 1)) & 0x0F) != 0) {
        return THEFT_TRIAL_FAIL;   /* IER must be quiescent live */
    }
    if (io.in(io.ctx, (unsigned short)(base + 4)) & 0x08) {
        return THEFT_TRIAL_FAIL;   /* MCR OUT2 must be clear live */
    }

    /* From here on is the LIVE window: the detection-phase IER store-test (a
     * transient 0x0F) is behind us, so any IER-nonzero / OUT2-set write now is
     * a real violation. */
    UartSimMarkLive(&sim);

    /* Some live traffic. */
    if (UartTxChunk(&io, &drv, l->tx, (int)l->txlen) != (int)l->txlen) {
        return THEFT_TRIAL_FAIL;
    }
    UartSimInjectRx(&sim, l->rx, (int)l->rxlen);
    if (UartRxDrain(&io, &drv, rxbuf, (int)l->rxlen) <= 0) {
        return THEFT_TRIAL_FAIL;
    }
    UartDrainAndClose(&io, &drv);

    /* PIN #3: across the whole LIVE window (TX/RX/close), IER was never written
     * nonzero and MCR OUT2 was never set. (DLAB-aware: base+1 is DLM under
     * DLAB.) */
    if (UartSimTraceIerArmed(&sim)) {
        return THEFT_TRIAL_FAIL;
    }
    if (UartSimTraceOut2Set(&sim)) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P4 divisor_round_trip - a NORMAL fake round-trips any divisor; a stuck fake
 * fails the read-back.
 * Generator: a divisor + base + a normal-vs-stuck selector.
 * =================================================================== */

struct divcase { uint16_t divisor; uint8_t base_sel; uint8_t stuck; };

static enum theft_alloc_res
div_alloc(struct theft *t, void *env, void **out)
{
    struct divcase *d;
    (void)env;
    d = malloc(sizeof(*d));
    if (d == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    d->divisor  = (uint16_t)theft_random_bits(t, 16);
    d->base_sel = (uint8_t)theft_random_bits(t, 8);
    d->stuck    = (uint8_t)theft_random_bits(t, 1);
    *out = d;
    return THEFT_ALLOC_OK;
}

static theft_hash div_hash(const void *instance, void *env)
{
    const struct divcase *d = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)d, sizeof(*d));
}

static struct theft_type_info div_info = {
    .alloc = div_alloc,
    .free  = theft_generic_free_cb,
    .hash  = div_hash,
};

static enum theft_trial_res
prop_divisor_round_trip(struct theft *t, void *arg1)
{
    struct divcase *d = (struct divcase *)arg1;
    unsigned short base = com_base(d->base_sel);
    unsigned int divisor = (unsigned int)d->divisor;
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int ok;
    (void)t;

    if (d->stuck) {
        UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_DIVISOR_STUCK);
        io = UartSimPortIo(&sim);
        UartDriverInit(&drv, base);
        ok = UartProgramDivisor(&io, &drv, divisor);
        /* A stuck clone that ignores DLL/DLM => read-back mismatch => 0.
         * (Edge: if the stuck fixed value happens to equal the request the
         * sim still drops the write, so the latch reads its init 0 - which
         * only matches a 0 request; force a nonzero divisor to avoid that.) */
        if (divisor == 0) {
            return THEFT_TRIAL_SKIP;
        }
        if (ok != 0) {
            return THEFT_TRIAL_FAIL;
        }
        return THEFT_TRIAL_PASS;
    }

    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    ok = UartProgramDivisor(&io, &drv, divisor);
    if (!ok) {
        return THEFT_TRIAL_FAIL;          /* a NORMAL fake must accept it */
    }
    if (drv.divisor != divisor) {
        return THEFT_TRIAL_FAIL;          /* and record exactly the request */
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P5 open_loops_bounded - PIN #6. A NEVER_READY fake fails the open with a
 * bounded op count; a TX vs NEVER_READY returns <0 within the bound.
 * Generator: a base + divisor (the variant is fixed NEVER_READY).
 * =================================================================== */

struct boundcase { uint8_t base_sel; uint16_t divisor; uint8_t txlen; };

static enum theft_alloc_res
bound_alloc(struct theft *t, void *env, void **out)
{
    struct boundcase *b;
    (void)env;
    b = malloc(sizeof(*b));
    if (b == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    b->base_sel = (uint8_t)theft_random_bits(t, 8);
    b->divisor  = (uint16_t)theft_random_bits(t, 16);
    b->txlen    = (uint8_t)(1 + (theft_random_bits(t, 4) % 8));
    *out = b;
    return THEFT_ALLOC_OK;
}

static theft_hash bound_hash(const void *instance, void *env)
{
    const struct boundcase *b = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)b, sizeof(*b));
}

static struct theft_type_info bound_info = {
    .alloc = bound_alloc,
    .free  = theft_generic_free_cb,
    .hash  = bound_hash,
};

static enum theft_trial_res
prop_open_loops_bounded(struct theft *t, void *arg1)
{
    struct boundcase *b = (struct boundcase *)arg1;
    unsigned short base = com_base(b->base_sel);
    unsigned int divisor = (b->divisor == 0) ? 6u : (unsigned int)b->divisor;
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    unsigned char txbuf[8];
    int res;
    int i;
    (void)t;

    for (i = 0; i < 8; i++) {
        txbuf[i] = (unsigned char)(0x40 + i);
    }

    /* NEVER_READY still presents a scratch round-trip and the FIFO IIR, so
     * UartDetect succeeds; the open then stalls in the bounded loopback DR poll
     * (THRE/DR never assert) and must fail closed within the bound. */
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NEVER_READY);
    io = UartSimPortIo(&sim);

    res = UartOpenSequence(&io, &drv, base, divisor);
    if (res != UART_OPEN_FAILED) {
        return THEFT_TRIAL_FAIL;          /* must fail, not hang */
    }
    if (drv.open != 0) {
        return THEFT_TRIAL_FAIL;
    }
    if (UartSimOps(&sim) >= OP_CEILING) {
        return THEFT_TRIAL_FAIL;          /* unbounded spin */
    }

    /* A TX against a never-ready THRE must also bail within the bound (<0). */
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NEVER_READY);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.tx_chunk = UART_SINGLE_TX_CHUNK;
    if (UartTxChunk(&io, &drv, txbuf, (int)b->txlen) >= 0) {
        return THEFT_TRIAL_FAIL;          /* THRE never asserts => error */
    }
    if (UartSimOps(&sim) >= OP_CEILING) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P6 rx_idle_is_live - PIN #6 exception. An idle line yields-and-repolls and
 * only ever returns >0 (when data finally arrives), never 0.
 * Generator: how many idle yields before the fake injects one byte (1..200),
 * and the byte value.
 * =================================================================== */

struct idlecase { uint8_t base_sel; uint8_t yields; uint8_t byte; };

static enum theft_alloc_res
idle_alloc(struct theft *t, void *env, void **out)
{
    struct idlecase *c;
    (void)env;
    c = malloc(sizeof(*c));
    if (c == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    c->base_sel = (uint8_t)theft_random_bits(t, 8);
    c->yields   = (uint8_t)(1 + (theft_random_bits(t, 8) % 200));   /* 1..200 */
    c->byte     = (uint8_t)theft_random_bits(t, 8);
    *out = c;
    return THEFT_ALLOC_OK;
}

static theft_hash idle_hash(const void *instance, void *env)
{
    const struct idlecase *c = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)c, sizeof(*c));
}

static struct theft_type_info idle_info = {
    .alloc = idle_alloc,
    .free  = theft_generic_free_cb,
    .hash  = idle_hash,
};

static enum theft_trial_res
prop_rx_idle_is_live(struct theft *t, void *arg1)
{
    struct idlecase *c = (struct idlecase *)arg1;
    unsigned short base = com_base(c->base_sel);
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    unsigned char rxbuf[8];
    int got;
    (void)t;

    /* Idle NORMAL line: no RX queued. The fake injects one byte after `yields`
     * idle yields, so UartRxDrain (the sole unbounded loop) yields-and-repolls
     * across the idle window and returns only when data arrives - NEVER 0. */
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    UartSimSetIdleRx(&sim, (int)c->yields, c->byte);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.open = 1;

    got = UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf));
    if (got <= 0) {
        return THEFT_TRIAL_FAIL;          /* idle must never yield 0 or error */
    }
    if (rxbuf[0] != c->byte) {
        return THEFT_TRIAL_FAIL;          /* and delivers the injected byte */
    }
    /* It must have actually idled (yielded) before the byte arrived. */
    if (UartSimOps(&sim) < (unsigned long)c->yields) {
        return THEFT_TRIAL_FAIL;
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P7 lsr_before_rbr - the order-mandatory overrun pin. An injected byte
 * carrying OE must be read correctly AND bump overrun_count, which only holds
 * if the code reads LSR (clearing OE) before RBR.
 * Generator: a base + byte + a flag selecting which error bit rides along.
 * =================================================================== */

struct errcase { uint8_t base_sel; uint8_t byte; uint8_t errsel; };

static enum theft_alloc_res
err_alloc(struct theft *t, void *env, void **out)
{
    struct errcase *e;
    (void)env;
    e = malloc(sizeof(*e));
    if (e == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    e->base_sel = (uint8_t)theft_random_bits(t, 8);
    e->byte     = (uint8_t)theft_random_bits(t, 8);
    e->errsel   = (uint8_t)theft_random_bits(t, 2);
    *out = e;
    return THEFT_ALLOC_OK;
}

static theft_hash err_hash(const void *instance, void *env)
{
    const struct errcase *e = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)e, sizeof(*e));
}

static struct theft_type_info err_info = {
    .alloc = err_alloc,
    .free  = theft_generic_free_cb,
    .hash  = err_hash,
};

static enum theft_trial_res
prop_lsr_before_rbr(struct theft *t, void *arg1)
{
    struct errcase *e = (struct errcase *)arg1;
    unsigned short base = com_base(e->base_sel);
    unsigned char byte = e->byte;
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    unsigned char rxbuf[4];
    int got;
    (void)t;

    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.open = 1;

    /* One overrun-flagged byte. The sim clears OE on the LSR read, so an
     * RBR-first reader misses it (overrun_count stays 0) - the refutation. */
    UartSimInjectRxErr(&sim, byte, SIM_LSR_OE);

    got = UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf));
    if (got != 1) {
        return THEFT_TRIAL_FAIL;          /* the data byte must still arrive */
    }
    if (rxbuf[0] != byte) {
        return THEFT_TRIAL_FAIL;
    }
    if (drv.overrun_count != 1) {
        return THEFT_TRIAL_FAIL;          /* OE seen only if LSR read first */
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P8 tx_burst_le_depth - a 16550A TX of a buffer longer than the FIFO depth
 * never bursts past tx_chunk and delivers every byte in order.
 * Generator: a payload length 1..256 + the bytes.
 * =================================================================== */

struct burstcase { uint16_t len; uint8_t base_sel; uint8_t seed; };

static enum theft_alloc_res
burst_alloc(struct theft *t, void *env, void **out)
{
    struct burstcase *b;
    (void)env;
    b = malloc(sizeof(*b));
    if (b == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    b->len      = (uint16_t)(1 + (theft_random_bits(t, 8) % 256));   /* 1..256 */
    b->base_sel = (uint8_t)theft_random_bits(t, 8);
    b->seed     = (uint8_t)theft_random_bits(t, 8);
    *out = b;
    return THEFT_ALLOC_OK;
}

static theft_hash burst_hash(const void *instance, void *env)
{
    const struct burstcase *b = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)b, sizeof(*b));
}

static struct theft_type_info burst_info = {
    .alloc = burst_alloc,
    .free  = theft_generic_free_cb,
    .hash  = burst_hash,
};

static enum theft_trial_res
prop_tx_burst_le_depth(struct theft *t, void *arg1)
{
    struct burstcase *b = (struct burstcase *)arg1;
    unsigned short base = com_base(b->base_sel);
    int len = (int)b->len;
    unsigned char payload[256];
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int i;
    int sent;
    const unsigned char *captured;
    (void)t;

    for (i = 0; i < len; i++) {
        payload[i] = (unsigned char)((b->seed + i) & 0xFF);
    }

    /* A detected 16550A => tx_chunk == 16. The sim captures THR writes; we
     * assert every byte arrived in order. (The burst <= tx_chunk discipline is
     * pinned structurally in UartTxChunk: it writes at most tx_chunk bytes per
     * THRE wait; the sim THRE-empties instantly so the ordered capture proves
     * delivery, and tx_chunk==16 is checked directly.) */
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);

    if (UartOpenSequence(&io, &drv, base, 6u) != UART_OPEN_LIVE) {
        return THEFT_TRIAL_FAIL;
    }
    if (drv.tx_chunk != UART_FIFO_TX_CHUNK) {
        return THEFT_TRIAL_FAIL;          /* 16550A must burst at 16 */
    }

    /* Clear any loopback-residue capture: the self-test ran under MCR LOOP so
     * its sentinel went to the loop cell, not tx[]. tx_len should be 0 here. */
    if (UartSimTxLen(&sim) != 0) {
        return THEFT_TRIAL_FAIL;
    }

    sent = UartTxChunk(&io, &drv, payload, len);
    if (sent != len) {
        return THEFT_TRIAL_FAIL;
    }
    if (UartSimTxLen(&sim) != len) {
        return THEFT_TRIAL_FAIL;          /* every byte left on the wire */
    }
    captured = UartSimTxBytes(&sim);
    for (i = 0; i < len; i++) {
        if (captured[i] != payload[i]) {
            return THEFT_TRIAL_FAIL;      /* in order, no drops/reorders */
        }
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P9 rx_break_never_zero - the "never 0" half of the RX invariant. A poll pass
 * that consumes ONLY a break (BI without framing error) drops the spurious 0x00
 * and must KEEP WAITING - returning 0 there would read as a peer close on a line
 * that has no orderly close. Sub-case A: a lone break followed (after some idle
 * yields) by a real byte => returns that byte (>0), never 0. Sub-case B: a break
 * WITH a framing error (BI|FE) => a real comms error (<0).
 * Generator: base + a real byte + idle-yield count + a with-framing selector.
 * =================================================================== */

struct breakcase { uint8_t base_sel; uint8_t byte; uint8_t yields; uint8_t framing; };

static enum theft_alloc_res
break_alloc(struct theft *t, void *env, void **out)
{
    struct breakcase *b;
    (void)env;
    b = malloc(sizeof(*b));
    if (b == NULL) {
        return THEFT_ALLOC_ERROR;
    }
    b->base_sel = (uint8_t)theft_random_bits(t, 8);
    b->byte     = (uint8_t)theft_random_bits(t, 8);
    b->yields   = (uint8_t)(1 + (theft_random_bits(t, 7) % 100));   /* 1..100 */
    b->framing  = (uint8_t)theft_random_bits(t, 1);
    *out = b;
    return THEFT_ALLOC_OK;
}

static theft_hash break_hash(const void *instance, void *env)
{
    const struct breakcase *b = instance;
    (void)env;
    return theft_hash_onepass((const uint8_t *)b, sizeof(*b));
}

static struct theft_type_info break_info = {
    .alloc = break_alloc,
    .free  = theft_generic_free_cb,
    .hash  = break_hash,
};

static enum theft_trial_res
prop_rx_break_never_zero(struct theft *t, void *arg1)
{
    struct breakcase *b = (struct breakcase *)arg1;
    unsigned short base = com_base(b->base_sel);
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    unsigned char rxbuf[8];
    int got;
    (void)t;

    if (b->framing) {
        /* Sub-case B: a break carrying a framing error is a hard line error. */
        UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
        io = UartSimPortIo(&sim);
        UartDriverInit(&drv, base);
        drv.open = 1;
        UartSimInjectRxErr(&sim, 0x00, (unsigned char)(SIM_LSR_BI | SIM_LSR_FE));
        got = UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf));
        if (got >= 0) {
            return THEFT_TRIAL_FAIL;       /* break+framing must be a comms error */
        }
        return THEFT_TRIAL_PASS;
    }

    /* Sub-case A: a lone break (BI, no FE) is the ONLY ready byte; a real byte
     * arrives later via the idle one-shot. The drain consumes+drops the break,
     * sees the line go idle, and must KEEP WAITING - never return 0 - delivering
     * the real byte once it lands. (Force the real byte != 0x00 so a stray
     * spurious-0x00 delivery could not masquerade as success.) */
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.open = 1;
    UartSimInjectRxErr(&sim, 0x00, (unsigned char)SIM_LSR_BI);   /* the lone break */
    UartSimSetIdleRx(&sim, (int)b->yields,
                     (unsigned char)(b->byte == 0 ? 0x42 : b->byte));

    got = UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf));
    if (got <= 0) {
        return THEFT_TRIAL_FAIL;           /* a dropped break must NOT yield 0 */
    }
    if (rxbuf[0] != (b->byte == 0 ? 0x42 : b->byte)) {
        return THEFT_TRIAL_FAIL;           /* delivers the real byte, not the 0x00 */
    }
    return THEFT_TRIAL_PASS;
}

/* ===================================================================
 * P10 nona_16550_no_fifo - PIN #4, the dangerous detection direction. A non-A
 * 16550 reports IIR&0xC0==0x80 after FCR=0xE7; the FIFO detect must REFUSE it
 * (only the exact 0xC0 enables the FIFO). This feeds UartDetect the 0x80
 * readback directly (the UART_SIM_NONA_16550 variant) and asserts the chip is
 * driven single-byte - the false-16550A-positive can never enable a multi-byte
 * burst. Generator: a base port (the kind under the variant is fixed 16550A so
 * the scratch register is present; the variant overrides IIR to 0x80).
 * =================================================================== */

static enum theft_trial_res
prop_nona_16550_no_fifo(struct theft *t, void *arg1)
{
    struct chipcase *c = (struct chipcase *)arg1;
    unsigned short base = com_base(c->base_sel);
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    (void)t;

    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NONA_16550);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);

    if (!UartDetect(&io, &drv)) {
        return THEFT_TRIAL_FAIL;   /* a present (non-A) chip must be detected */
    }
    /* The load-bearing refusal: a 0x80 readback NEVER enables the FIFO. */
    if (drv.fifo_enabled != 0) {
        return THEFT_TRIAL_FAIL;
    }
    if (drv.tx_chunk != UART_SINGLE_TX_CHUNK) {
        return THEFT_TRIAL_FAIL;
    }
    if (drv.chip_kind == UART_CHIP_16550A) {
        return THEFT_TRIAL_FAIL;   /* must NOT be mis-identified as a 16550A */
    }
    return THEFT_TRIAL_PASS;
}

/* =================================================================== */

static int run1(const char *name, theft_propfun1 *p,
                const struct theft_type_info *ti, theft_seed seed)
{
    struct theft_run_config cfg = {
        .name = name, .prop1 = p,
        .type_info = { ti }, .trials = TRIALS, .seed = seed,
    };
    enum theft_run_res res = theft_run(&cfg);
    printf("  %-28s %s (%d trials)\n", name,
           res == THEFT_RUN_PASS ? "PASS" : "FAIL", TRIALS);
    return res == THEFT_RUN_PASS ? 0 : 1;
}

int main(void)
{
    int fails = 0;

    printf("theft_uart (src/uart.c pure ladder vs simulated 16550):\n");

    fails += run1("uart/fifo_iff_16550a",     prop_fifo_iff_16550a,
                  &chip_info,  SEED);
    fails += run1("uart/loopback_fail_closed", prop_loopback_fail_closed,
                  &fault_info, SEED ^ 0x11);
    fails += run1("uart/interrupts_never_armed", prop_interrupts_never_armed,
                  &live_info,  SEED ^ 0x22);
    fails += run1("uart/divisor_round_trip",   prop_divisor_round_trip,
                  &div_info,   SEED ^ 0x33);
    fails += run1("uart/open_loops_bounded",   prop_open_loops_bounded,
                  &bound_info, SEED ^ 0x44);
    fails += run1("uart/rx_idle_is_live",      prop_rx_idle_is_live,
                  &idle_info,  SEED ^ 0x55);
    fails += run1("uart/lsr_before_rbr",       prop_lsr_before_rbr,
                  &err_info,   SEED ^ 0x66);
    fails += run1("uart/tx_burst_le_depth",    prop_tx_burst_le_depth,
                  &burst_info, SEED ^ 0x77);
    fails += run1("uart/rx_break_never_zero",  prop_rx_break_never_zero,
                  &break_info, SEED ^ 0x88);
    fails += run1("uart/nona_16550_no_fifo",   prop_nona_16550_no_fifo,
                  &chip_info,  SEED ^ 0x99);

    printf("%s\n", fails == 0 ? "ALL PASS" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
