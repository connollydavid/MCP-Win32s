/*
 * test_uart.c - property-based tests for the pure Win32s direct-UART ladder
 * (src/uart.c) on the shipped C89/i386 path, driven over the injected UartPortIo
 * seam against a simulated 16550 (tests/uart_sim.h).
 *
 * Mirrors the nine theft host properties (tests/host/theft_uart.c, P1..P9) at
 * lower trial counts, so the 6.2 security pins (uart.allium invariants) are
 * proven on the actual C89/i386 build, not only natively:
 *   uart_fifo_iff_16550a      PIN #4: fifo_enabled IFF kind==16550A; tx_chunk 16
 *                             on 16550A else 1; fifo on => 16550A.
 *   uart_loopback_fail_closed rule UartOpenFailsLoopback: a DEAD_CLONE or
 *                             DIVISOR_STUCK fake => UART_OPEN_FAILED, open stays 0.
 *   uart_interrupts_never_armed PIN #3: across a full open/tx/rx/close, IER never
 *                             left nonzero and MCR OUT2 (bit 3) never set.
 *   uart_divisor_round_trip   a NORMAL fake round-trips any divisor; a
 *                             DIVISOR_STUCK fake fails the read-back.
 *   uart_open_loops_bounded   PIN #6: a NEVER_READY fake => UART_OPEN_FAILED with
 *                             a bounded op count; a TX vs NEVER_READY returns <0.
 *   uart_rx_idle_is_live      PIN #6 exception: an idle line yields-and-repolls
 *                             and returns only on data (>0), never 0.
 *   uart_lsr_before_rbr       the order-mandatory overrun pin: an injected OE byte
 *                             reads correctly AND bumps overrun_count (only true if
 *                             LSR is read before RBR).
 *   uart_tx_burst_le_depth    a 16550A TX delivers every byte in order; tx_chunk 16.
 *   uart_rx_break_never_zero  PIN #6 exception ("never 0" half): a lone break is
 *                             dropped and the drain keeps waiting (>0 on the real
 *                             byte, never 0); a break+framing error is <0.
 *
 * Uses prop.h (minimal C89 PBT framework). Pure - links only src/uart.c, compiled
 * -DUART_HOST_PURE (excludes the asm IN/OUT seam + the Transport wiring).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#define PROP_IMPLEMENTATION
#include "prop.h"
#include "uart.h"
#include "uart_sim.h"
#include <stdio.h>

/* A sane ceiling for the bounded-loop op counter (same as theft_uart.c:
 * 10 x UART_POLL_BOUND). A handful of bounded loops stays well under it. */
#define OP_CEILING  (10L * 100000L)

/* The four COM base ports, selected per trial to mirror theft's com_base. */
static unsigned short com_base(int sel)
{
    switch (sel & 3) {
    case 0:  return 0x3F8;
    case 1:  return 0x2F8;
    case 2:  return 0x3E8;
    default: return 0x2E8;
    }
}

/* Map a 3-way selector to a present chip kind (UNKNOWN excluded - it is the
 * absent result, not a present kind). */
static UartChipKind kind_of(int sel)
{
    switch (sel % 3) {
    case 0:  return UART_CHIP_8250;
    case 1:  return UART_CHIP_16450;
    default: return UART_CHIP_16550A;
    }
}

/* P1: fifo_enabled IFF the chip is a 16550A; tx_chunk 16 there, 1 otherwise. */
PROP_TEST(uart_fifo_iff_16550a) {
    UartChipKind kind = kind_of(PROP_CHOICE(3));
    unsigned short base = com_base(PROP_CHOICE(4));
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int is_16550a;

    UartSimInit(&sim, base, kind, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);

    PROP_CHECK(UartDetect(&io, &drv) != 0);   /* a present NORMAL chip detects */

    is_16550a = (kind == UART_CHIP_16550A);

    /* The load-bearing biconditional. */
    PROP_CHECK(drv.fifo_enabled == (is_16550a ? 1 : 0));
    if (is_16550a) {
        PROP_CHECK(drv.tx_chunk == UART_FIFO_TX_CHUNK);
        PROP_CHECK(drv.chip_kind == UART_CHIP_16550A);
    } else {
        PROP_CHECK(drv.tx_chunk == UART_SINGLE_TX_CHUNK);
        PROP_CHECK(!drv.fifo_enabled);
    }
    /* The dangerous direction stated directly: fifo on => 16550A. */
    PROP_CHECK(!(drv.fifo_enabled && drv.chip_kind != UART_CHIP_16550A));
}

/* P2: a DEAD_CLONE or DIVISOR_STUCK fake fails the open closed. */
PROP_TEST(uart_loopback_fail_closed) {
    int which = PROP_BOOL();
    unsigned short base = com_base(PROP_CHOICE(4));
    UartSimVariant v = which ? UART_SIM_DEAD_CLONE : UART_SIM_DIVISOR_STUCK;
    unsigned int divisor = (unsigned int)PROP_INT(1, 0xFFFF);
    UartSim sim;
    UartPortIo io;
    UartDriver drv;

    UartSimInit(&sim, base, UART_CHIP_16550A, v);
    io = UartSimPortIo(&sim);

    PROP_CHECK(UartOpenSequence(&io, &drv, base, divisor) == UART_OPEN_FAILED);
    PROP_CHECK(drv.open == 0);   /* never goes live */
}

/* P3 (PIN #3): a full open/tx/rx/close on a NORMAL chip never arms IER nor sets
 * MCR OUT2 across the LIVE window. */
PROP_TEST(uart_interrupts_never_armed) {
    UartChipKind kind = kind_of(PROP_CHOICE(3));
    unsigned short base = com_base(PROP_CHOICE(4));
    unsigned int divisor = (unsigned int)PROP_INT(1, 0xFFFF);
    int txlen = PROP_INT(1, 32);
    int rxlen = PROP_INT(1, 32);
    unsigned char tx[32];
    unsigned char rx[32];
    unsigned char rxbuf[32];
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int i;

    for (i = 0; i < 32; i++) {
        tx[i] = (unsigned char)PROP_INT(0, 255);
        rx[i] = (unsigned char)PROP_INT(0, 255);
    }

    UartSimInit(&sim, base, kind, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);

    PROP_CHECK(UartOpenSequence(&io, &drv, base, divisor) == UART_OPEN_LIVE);

    /* The live register state read directly at open: IER masks to 0, OUT2 clear. */
    PROP_CHECK((io.in(io.ctx, (unsigned short)(base + 1)) & 0x0F) == 0);
    PROP_CHECK((io.in(io.ctx, (unsigned short)(base + 4)) & 0x08) == 0);

    /* Mark the start of the LIVE window (skip the detection IER store-test). */
    UartSimMarkLive(&sim);

    PROP_CHECK(UartTxChunk(&io, &drv, tx, txlen) == txlen);
    UartSimInjectRx(&sim, rx, rxlen);
    PROP_CHECK(UartRxDrain(&io, &drv, rxbuf, rxlen) > 0);
    UartDrainAndClose(&io, &drv);

    /* PIN #3: across the whole LIVE window, IER never written nonzero and MCR
     * OUT2 never set. */
    PROP_CHECK(UartSimTraceIerArmed(&sim) == 0);
    PROP_CHECK(UartSimTraceOut2Set(&sim) == 0);
}

/* P4: a NORMAL fake round-trips any divisor; a DIVISOR_STUCK fake fails it. */
PROP_TEST(uart_divisor_round_trip) {
    unsigned short base = com_base(PROP_CHOICE(4));
    int stuck = PROP_BOOL();
    unsigned int divisor;
    UartSim sim;
    UartPortIo io;
    UartDriver drv;

    if (stuck) {
        /* Force a nonzero divisor: a stuck clone drops the write and the latch
         * reads its init 0, which would spuriously match a 0 request. */
        divisor = (unsigned int)PROP_INT(1, 0xFFFF);
        UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_DIVISOR_STUCK);
        io = UartSimPortIo(&sim);
        UartDriverInit(&drv, base);
        PROP_CHECK(UartProgramDivisor(&io, &drv, divisor) == 0);
        return;
    }

    divisor = (unsigned int)PROP_INT(0, 0xFFFF);
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    PROP_CHECK(UartProgramDivisor(&io, &drv, divisor) == 1);   /* accepted */
    PROP_CHECK(drv.divisor == divisor);                         /* exact request */
}

/* P5 (PIN #6): a NEVER_READY fake fails the open with a bounded op count; a TX
 * against a never-ready THRE returns <0 within the bound. */
PROP_TEST(uart_open_loops_bounded) {
    unsigned short base = com_base(PROP_CHOICE(4));
    unsigned int divisor = (unsigned int)PROP_INT(1, 0xFFFF);
    int txlen = PROP_INT(1, 8);
    unsigned char txbuf[8];
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int i;

    for (i = 0; i < 8; i++) {
        txbuf[i] = (unsigned char)(0x40 + i);
    }

    /* NEVER_READY presents a scratch round-trip + the FIFO IIR, so UartDetect
     * succeeds; the open then stalls in the bounded loopback DR poll and must
     * fail closed within the bound. */
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NEVER_READY);
    io = UartSimPortIo(&sim);

    PROP_CHECK(UartOpenSequence(&io, &drv, base, divisor) == UART_OPEN_FAILED);
    PROP_CHECK(drv.open == 0);
    PROP_CHECK((long)UartSimOps(&sim) < OP_CEILING);   /* not an unbounded spin */

    /* A TX against a never-ready THRE must also bail within the bound (<0). */
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NEVER_READY);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.tx_chunk = UART_SINGLE_TX_CHUNK;
    PROP_CHECK(UartTxChunk(&io, &drv, txbuf, txlen) < 0);
    PROP_CHECK((long)UartSimOps(&sim) < OP_CEILING);
}

/* P6 (PIN #6 exception): an idle line yields-and-repolls and returns only on
 * data (>0), never 0; the injected byte is delivered. */
PROP_TEST(uart_rx_idle_is_live) {
    unsigned short base = com_base(PROP_CHOICE(4));
    int yields = PROP_INT(1, 200);
    unsigned char byte = (unsigned char)PROP_INT(0, 255);
    unsigned char rxbuf[8];
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    int got;

    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    UartSimSetIdleRx(&sim, yields, byte);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.open = 1;

    got = UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf));
    PROP_CHECK(got > 0);                 /* idle must never yield 0 or error */
    PROP_CHECK(rxbuf[0] == byte);        /* delivers the injected byte */
    PROP_CHECK(UartSimOps(&sim) >= (unsigned long)yields);   /* it did idle */
}

/* P7: the order-mandatory overrun pin. An injected OE byte reads correctly AND
 * bumps overrun_count, which only holds if LSR is read before RBR. */
PROP_TEST(uart_lsr_before_rbr) {
    unsigned short base = com_base(PROP_CHOICE(4));
    unsigned char byte = (unsigned char)PROP_INT(0, 255);
    unsigned char rxbuf[4];
    UartSim sim;
    UartPortIo io;
    UartDriver drv;

    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.open = 1;

    /* One overrun-flagged byte. The sim clears OE on the LSR read, so an
     * RBR-first reader misses it (overrun_count stays 0). */
    UartSimInjectRxErr(&sim, byte, SIM_LSR_OE);

    PROP_CHECK(UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf)) == 1);
    PROP_CHECK(rxbuf[0] == byte);
    PROP_CHECK(drv.overrun_count == 1);   /* OE seen only if LSR read first */
}

/* P8: a 16550A TX delivers every byte in order; tx_chunk is 16. */
PROP_TEST(uart_tx_burst_le_depth) {
    unsigned short base = com_base(PROP_CHOICE(4));
    int len = PROP_INT(1, 256);
    int seed = PROP_INT(0, 255);
    unsigned char payload[256];
    UartSim sim;
    UartPortIo io;
    UartDriver drv;
    const unsigned char *captured;
    int i;

    for (i = 0; i < len; i++) {
        payload[i] = (unsigned char)((seed + i) & 0xFF);
    }

    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);

    PROP_CHECK(UartOpenSequence(&io, &drv, base, 6u) == UART_OPEN_LIVE);
    PROP_CHECK(drv.tx_chunk == UART_FIFO_TX_CHUNK);   /* 16550A bursts at 16 */
    /* The loopback self-test ran under MCR LOOP, so its sentinel went to the
     * loop cell, not tx[]: live TX capture starts empty. */
    PROP_CHECK(UartSimTxLen(&sim) == 0);

    PROP_CHECK(UartTxChunk(&io, &drv, payload, len) == len);
    PROP_CHECK(UartSimTxLen(&sim) == len);            /* every byte on the wire */
    captured = UartSimTxBytes(&sim);
    for (i = 0; i < len; i++) {
        PROP_CHECK(captured[i] == payload[i]);        /* in order, no drops */
    }
}

/* P9 (PIN #6 exception, "never 0" half): a lone break is dropped and the drain
 * keeps waiting (>0 on the real byte, never 0); a break+framing error is <0. */
PROP_TEST(uart_rx_break_never_zero) {
    unsigned short base = com_base(PROP_CHOICE(4));
    int framing = PROP_BOOL();
    int yields = PROP_INT(1, 100);
    unsigned char realbyte;
    unsigned char rxbuf[8];
    UartSim sim;
    UartPortIo io;
    UartDriver drv;

    if (framing) {
        /* Sub-case B: a break carrying a framing error is a hard line error. */
        UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
        io = UartSimPortIo(&sim);
        UartDriverInit(&drv, base);
        drv.open = 1;
        UartSimInjectRxErr(&sim, 0x00,
                           (unsigned char)(SIM_LSR_BI | SIM_LSR_FE));
        PROP_CHECK(UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf)) < 0);
        return;
    }

    /* Sub-case A: a lone break (BI, no FE) is the only ready byte; a real byte
     * arrives later via the idle one-shot. The drain consumes+drops the break,
     * sees the line go idle, keeps waiting, and delivers the real byte - never
     * returns 0. Force the real byte != 0x00 so a stray spurious-0x00 delivery
     * could not masquerade as success. */
    realbyte = (unsigned char)PROP_INT(1, 255);
    UartSimInit(&sim, base, UART_CHIP_16550A, UART_SIM_NORMAL);
    io = UartSimPortIo(&sim);
    UartDriverInit(&drv, base);
    drv.open = 1;
    UartSimInjectRxErr(&sim, 0x00, (unsigned char)SIM_LSR_BI);   /* lone break */
    UartSimSetIdleRx(&sim, yields, realbyte);

    PROP_CHECK(UartRxDrain(&io, &drv, rxbuf, (int)sizeof(rxbuf)) > 0);
    PROP_CHECK(rxbuf[0] == realbyte);   /* the real byte, not the dropped 0x00 */
}

int main(void)
{
    prop_seed(0);

    PROP_RUN(uart_fifo_iff_16550a,       1500);
    PROP_RUN(uart_loopback_fail_closed,  1500);
    PROP_RUN(uart_interrupts_never_armed, 1500);
    PROP_RUN(uart_divisor_round_trip,    1500);
    PROP_RUN(uart_open_loops_bounded,    1500);
    PROP_RUN(uart_rx_idle_is_live,       1500);
    PROP_RUN(uart_lsr_before_rbr,        1500);
    PROP_RUN(uart_tx_burst_le_depth,     1500);
    PROP_RUN(uart_rx_break_never_zero,   1500);

    return prop_summary();
}
