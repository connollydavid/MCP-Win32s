/*
 * uart_sim.h - a stateful simulated 16550 register machine for testing the pure
 * UART ladder (src/uart.c) over the injected UartPortIo seam. Header-only and
 * strict C89 so it can be #included by BOTH the host theft harness
 * (tests/host/theft_uart.c, C99) AND the on-target prop.h mirror
 * (tests/test_uart.c, C89) - each translation unit gets its own static copy.
 *
 * The register semantics are a TRANSCRIPTION of the throwaway spike that ran on
 * real Win32s hardware (tools/phase6-qemu/uart-probe.c), not a datasheet alone:
 *   - presence + the 8250/16450 split is the scratch (base+7) AA/55 round-trip;
 *   - the FIFO detect is FCR=0xE7 -> IIR top two bits (0xC0=16550A FIFO);
 *   - the DLAB latch (LCR bit7) routes base+0/+1 to DLL/DLM vs RBR-THR/IER;
 *   - LSR DR/THRE/TEMT drive the polled TX/RX; the LSR error bits OE/PE/FE/BI
 *     are cleared on an LSR read (so an RBR-before-LSR reader misses them);
 *   - MCR LOOP wires THR->RBR internally and reflects MCR modem bits into MSR.
 *
 * The fault variants exercise the fail-closed directions: ABSENT (floating bus),
 * DEAD_CLONE (decodes but never loops), DIVISOR_STUCK (ignores DLL/DLM writes),
 * NEVER_READY (THRE/DR never assert - the bounded-loop pin).
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef UART_SIM_H
#define UART_SIM_H

#include "uart.h"   /* UartPortIo, UartChipKind */

/* Sim register offsets (mirror src/uart.c; kept local so the header stands
 * alone). */
#define SIM_RBR  0
#define SIM_IER  1
#define SIM_IIR  2   /* read */
#define SIM_FCR  2   /* write */
#define SIM_LCR  3
#define SIM_MCR  4
#define SIM_LSR  5
#define SIM_MSR  6
#define SIM_SCR  7

/* LCR / LSR / MCR / MSR bits used by the sim. */
#define SIM_LCR_DLAB  0x80
#define SIM_LSR_DR    0x01
#define SIM_LSR_OE    0x02
#define SIM_LSR_PE    0x04
#define SIM_LSR_FE    0x08
#define SIM_LSR_BI    0x10
#define SIM_LSR_THRE  0x20
#define SIM_LSR_TEMT  0x40
#define SIM_MCR_DTR   0x01
#define SIM_MCR_RTS   0x02
#define SIM_MCR_OUT1  0x04
#define SIM_MCR_OUT2  0x08
#define SIM_MCR_LOOP  0x10
#define SIM_MSR_CTS   0x10   /* reflects MCR RTS under loopback */
#define SIM_MSR_DSR   0x20   /* reflects MCR DTR under loopback */
#define SIM_MSR_RI    0x40   /* reflects MCR OUT1 under loopback */
#define SIM_MSR_DCD   0x80   /* reflects MCR OUT2 under loopback */

/* Capacities. */
#define SIM_TX_CAP      512
#define SIM_RX_CAP      512
#define SIM_TRACE_CAP   2048

/* A floating bus reads all-ones. */
#define SIM_FLOATING    0xFF

typedef enum {
    UART_SIM_NORMAL = 0,    /* a healthy chip of the given kind                */
    UART_SIM_ABSENT,        /* floating bus: every read 0xFF, scratch dead     */
    UART_SIM_DEAD_CLONE,    /* decodes registers but loopback does NOT echo    */
    UART_SIM_DIVISOR_STUCK, /* ignores DLL/DLM writes (readback mismatches)    */
    UART_SIM_NEVER_READY    /* THRE and DR never assert (bounded-loop pin)     */
} UartSimVariant;

typedef struct UartSimTrace {
    unsigned short port;
    unsigned char  val;
} UartSimTrace;

typedef struct UartSim {
    unsigned short base_port;
    UartChipKind   kind;        /* what FCR=0xE7->IIR and the scratch split say */
    UartSimVariant variant;

    /* Register state. */
    int            dlab;        /* LCR bit7 latched                             */
    unsigned char  dll;
    unsigned char  dlm;
    unsigned char  ier;
    unsigned char  fcr;
    int            fifo_on;
    unsigned char  mcr;
    unsigned char  lsr_err;     /* sticky OE/PE/FE/BI, cleared on an LSR read    */
    unsigned char  scratch;
    int            scratch_loaded;

    /* TX capture (THR writes, live - not the loopback path). */
    unsigned char  tx[SIM_TX_CAP];
    int            tx_len;

    /* RX inject queue (the wire feeding RBR), each byte paired with the LSR
     * error bits that accompany it (OE/PE/FE/BI). */
    unsigned char  rx[SIM_RX_CAP];
    unsigned char  rx_err[SIM_RX_CAP];
    int            rx_len;
    int            rx_pos;

    /* Loopback echo holding cell: a THR write under MCR LOOP lands here and
     * asserts DR until read. */
    int            loop_pending;
    unsigned char  loop_byte;

    /* OUT-trace: every out(port,val), for the interrupt-never-armed property. */
    UartSimTrace   trace[SIM_TRACE_CAP];
    int            trace_len;

    /* Op counter (++ on every in/out/yield) for the bounded-loop property. */
    unsigned long  ops;

    /* Trace index marking where the live session begins (set by the harness
     * once UartOpenSequence returns live), so the interrupts-never-armed scan
     * can be restricted to the LIVE window - the transient IER store-test probe
     * during detection legitimately writes IER=0x0F, then clears it. */
    int            live_mark;

    /* Idle-RX modelling for rx_idle_is_live: after this many yields with an
     * empty RX queue, inject one byte so the harness cannot spin forever. 0
     * disables (stay idle until the queue is fed). */
    int            idle_yields_until_rx;
    unsigned char  idle_rx_byte;
    int            idle_yields_seen;
} UartSim;

/* ----------------------------------------------------------------------
 * Construction.
 * ---------------------------------------------------------------------- */
static void UartSimInit(UartSim *s, unsigned short base, UartChipKind kind,
                        UartSimVariant v)
{
    int i;
    s->base_port = base;
    s->kind      = kind;
    s->variant   = v;
    s->dlab      = 0;
    s->dll       = 0;
    s->dlm       = 0;
    s->ier       = 0;
    s->fcr       = 0;
    s->fifo_on   = 0;
    s->mcr       = 0;
    s->lsr_err   = 0;
    s->scratch   = 0;
    s->scratch_loaded = 0;
    s->tx_len    = 0;
    s->rx_len    = 0;
    s->rx_pos    = 0;
    s->loop_pending = 0;
    s->loop_byte = 0;
    s->trace_len = 0;
    s->ops       = 0;
    s->live_mark = 0;
    s->idle_yields_until_rx = 0;
    s->idle_rx_byte = 0;
    s->idle_yields_seen = 0;
    for (i = 0; i < SIM_TX_CAP; i++) {
        s->tx[i] = 0;
    }
    for (i = 0; i < SIM_RX_CAP; i++) {
        s->rx[i]     = 0;
        s->rx_err[i] = 0;
    }
}

/* Feed bytes into the RX path (no error flags). */
static void UartSimInjectRx(UartSim *s, const unsigned char *data, int n)
{
    int i;
    for (i = 0; i < n && s->rx_len < SIM_RX_CAP; i++) {
        s->rx[s->rx_len]     = data[i];
        s->rx_err[s->rx_len] = 0;
        s->rx_len++;
    }
}

/* Feed one RX byte carrying explicit LSR error bits (OE/PE/FE/BI) - used by the
 * lsr_before_rbr / overrun property. */
static void UartSimInjectRxErr(UartSim *s, unsigned char byte, unsigned char err)
{
    if (s->rx_len < SIM_RX_CAP) {
        s->rx[s->rx_len]     = byte;
        s->rx_err[s->rx_len] = err;
        s->rx_len++;
    }
}

/* ----------------------------------------------------------------------
 * Whether this variant/kind presents a working scratch register. A real 8250
 * has none; the 16450/16550 do; ABSENT presents nothing. (DEAD_CLONE /
 * DIVISOR_STUCK / NEVER_READY decode normally - their fault is elsewhere.)
 * ---------------------------------------------------------------------- */
static int UartSimHasScratch(const UartSim *s)
{
    if (s->variant == UART_SIM_ABSENT) {
        return 0;
    }
    if (s->kind == UART_CHIP_8250) {
        return 0;
    }
    return 1;
}

/* The IIR top-bit FIFO signature for FCR=0xE7 on this kind. */
static unsigned char UartSimFifoIir(const UartSim *s)
{
    if (s->kind == UART_CHIP_16550A) {
        return 0xC0;   /* working FIFO */
    }
    /* A non-A 16550 reports 0x80; 8250/16450 report 0x00. We have no separate
     * "non-A 16550" kind enum, so model it via DEAD-style? No: kind is the
     * source of truth. 16450/8250 => 0x00. */
    return 0x00;
}

/* ----------------------------------------------------------------------
 * The LSR value the chip presents right now (data-ready / THRE / TEMT / errors),
 * for the NORMAL and fault variants.
 * ---------------------------------------------------------------------- */
static unsigned char UartSimLsr(const UartSim *s)
{
    unsigned char lsr;

    if (s->variant == UART_SIM_ABSENT) {
        return SIM_FLOATING;
    }

    lsr = 0;

    /* THRE/TEMT: always empty (we capture instantly) EXCEPT NEVER_READY, where
     * they never assert so every bounded TX/TEMT poll runs to its guard. */
    if (s->variant != UART_SIM_NEVER_READY) {
        lsr |= SIM_LSR_THRE | SIM_LSR_TEMT;
    }

    /* DR: loopback echo pending, or a queued RX byte, makes data ready. For
     * NEVER_READY, DR never asserts. */
    if (s->variant != UART_SIM_NEVER_READY) {
        if (s->loop_pending) {
            lsr |= SIM_LSR_DR;
        } else if (s->rx_pos < s->rx_len) {
            lsr |= SIM_LSR_DR;
            /* Surface the queued byte's error bits in this LSR snapshot. */
            lsr |= (s->rx_err[s->rx_pos] &
                    (SIM_LSR_OE | SIM_LSR_PE | SIM_LSR_FE | SIM_LSR_BI));
        }
    }

    /* Sticky errors not yet cleared by an LSR read. */
    lsr |= s->lsr_err;
    return lsr;
}

/* ----------------------------------------------------------------------
 * Port read.
 * ---------------------------------------------------------------------- */
static unsigned char UartSimIn(void *ctx, unsigned short port)
{
    UartSim      *s = (UartSim *)ctx;
    unsigned short off;
    unsigned char  v;

    s->ops++;
    off = (unsigned short)(port - s->base_port);

    if (s->variant == UART_SIM_ABSENT) {
        return SIM_FLOATING;   /* floating bus: everything reads 0xFF */
    }

    switch (off) {
    case SIM_RBR:   /* RBR (DLAB=0) or DLL (DLAB=1) */
        if (s->dlab) {
            return s->dll;
        }
        /* Reading RBR: loopback echo first, then the injected RX queue. */
        if (s->loop_pending) {
            s->loop_pending = 0;
            return s->loop_byte;
        }
        if (s->rx_pos < s->rx_len) {
            v = s->rx[s->rx_pos];
            s->rx_pos++;
            return v;
        }
        return 0x00;

    case SIM_IER:   /* IER (DLAB=0) or DLM (DLAB=1) */
        return s->dlab ? s->dlm : s->ier;

    case SIM_IIR:
        /* After an FCR=0xE7 probe write, the top bits report the FIFO kind. */
        return UartSimFifoIir(s);

    case SIM_LCR:
        return (unsigned char)(s->dlab ? SIM_LCR_DLAB : 0x03);

    case SIM_MCR:
        return s->mcr;

    case SIM_LSR:
        v = UartSimLsr(s);
        /* Reading LSR clears the sticky error bits AND consumes the queued
         * byte's error flags (the order-mandatory pin: an RBR-first reader
         * never sees them). */
        s->lsr_err = 0;
        if (s->rx_pos < s->rx_len) {
            s->rx_err[s->rx_pos] = 0;
        }
        return v;

    case SIM_MSR:
        /* Under MCR LOOP, the modem-control bits reflect into MSR. */
        if (s->mcr & SIM_MCR_LOOP) {
            unsigned char msr = 0;
            if (s->mcr & SIM_MCR_RTS)  { msr |= SIM_MSR_CTS; }
            if (s->mcr & SIM_MCR_DTR)  { msr |= SIM_MSR_DSR; }
            if (s->mcr & SIM_MCR_OUT1) { msr |= SIM_MSR_RI;  }
            if (s->mcr & SIM_MCR_OUT2) { msr |= SIM_MSR_DCD; }
            return msr;
        }
        return 0x00;

    case SIM_SCR:
        if (!UartSimHasScratch(s)) {
            return SIM_FLOATING;   /* no scratch -> readback fails the round-trip */
        }
        return s->scratch_loaded ? s->scratch : 0x00;

    default:
        return 0x00;
    }
}

/* ----------------------------------------------------------------------
 * Port write.
 * ---------------------------------------------------------------------- */
static void UartSimOut(void *ctx, unsigned short port, unsigned char val)
{
    UartSim      *s = (UartSim *)ctx;
    unsigned short off;

    s->ops++;

    /* Record every write for the interrupt-never-armed trace. */
    if (s->trace_len < SIM_TRACE_CAP) {
        s->trace[s->trace_len].port = port;
        s->trace[s->trace_len].val  = val;
        s->trace_len++;
    }

    off = (unsigned short)(port - s->base_port);

    if (s->variant == UART_SIM_ABSENT) {
        return;   /* floating bus swallows writes */
    }

    switch (off) {
    case SIM_RBR:   /* THR (DLAB=0) or DLL (DLAB=1) */
        if (s->dlab) {
            if (s->variant != UART_SIM_DIVISOR_STUCK) {
                s->dll = val;   /* a stuck clone drops the divisor write */
            }
        } else {
            /* THR write. Under MCR LOOP it echoes to RBR (unless DEAD_CLONE,
             * which decodes but never loops); otherwise it leaves on the wire
             * and is captured. */
            if (s->mcr & SIM_MCR_LOOP) {
                if (s->variant != UART_SIM_DEAD_CLONE) {
                    s->loop_pending = 1;
                    s->loop_byte    = val;
                }
            } else if (s->tx_len < SIM_TX_CAP) {
                s->tx[s->tx_len] = val;
                s->tx_len++;
            }
        }
        break;

    case SIM_IER:   /* IER (DLAB=0) or DLM (DLAB=1) */
        if (s->dlab) {
            if (s->variant != UART_SIM_DIVISOR_STUCK) {
                s->dlm = val;
            }
        } else {
            /* Only the low nibble of IER is writable on a real UART; the upper
             * bits read back 0. This is what makes the IER store-test a valid
             * presence probe (0x0F round-trips its low nibble; 0xF0 does not). */
            s->ier = (unsigned char)(val & 0x0F);
        }
        break;

    case SIM_FCR:
        s->fcr     = val;
        s->fifo_on = (val & 0x01) ? 1 : 0;
        break;

    case SIM_LCR:
        s->dlab = (val & SIM_LCR_DLAB) ? 1 : 0;
        break;

    case SIM_MCR:
        s->mcr = val;
        break;

    case SIM_SCR:
        if (UartSimHasScratch(s)) {
            s->scratch        = val;
            s->scratch_loaded = 1;
        }
        break;

    default:
        break;
    }
}

/* ----------------------------------------------------------------------
 * Cooperative yield. Counts toward the op total; on an idle RX line it can
 * inject a byte after a configured number of yields so rx_idle_is_live can be
 * exercised without an unbounded harness loop.
 * ---------------------------------------------------------------------- */
static void UartSimYield(void *ctx)
{
    UartSim *s = (UartSim *)ctx;
    s->ops++;
    if (s->idle_yields_until_rx > 0 && s->rx_pos >= s->rx_len) {
        s->idle_yields_seen++;
        if (s->idle_yields_seen >= s->idle_yields_until_rx) {
            if (s->rx_len < SIM_RX_CAP) {
                s->rx[s->rx_len]     = s->idle_rx_byte;
                s->rx_err[s->rx_len] = 0;
                s->rx_len++;
            }
            s->idle_yields_until_rx = 0;   /* one-shot */
        }
    }
}

/* ----------------------------------------------------------------------
 * Build a UartPortIo seam whose ctx is this sim.
 * ---------------------------------------------------------------------- */
static UartPortIo UartSimPortIo(UartSim *s)
{
    UartPortIo io;
    io.in    = UartSimIn;
    io.out   = UartSimOut;
    io.yield = UartSimYield;
    io.ctx   = s;
    return io;
}

/* ----------------------------------------------------------------------
 * Accessors the properties need.
 * ---------------------------------------------------------------------- */

/* Configure the idle-RX one-shot: after `yields` idle yields, inject `byte`. */
static void UartSimSetIdleRx(UartSim *s, int yields, unsigned char byte)
{
    s->idle_yields_until_rx = yields;
    s->idle_rx_byte         = byte;
    s->idle_yields_seen     = 0;
}

/* Mark the current trace position as the start of the LIVE session, so the
 * interrupt-never-armed scans skip the detection-phase IER store-test probe
 * (which transiently writes 0x0F then clears it). Call right after
 * UartOpenSequence returns UART_OPEN_LIVE. */
static void UartSimMarkLive(UartSim *s)
{
    s->live_mark = s->trace_len;
}

/* Scan the OUT-trace from live_mark onward: did any LIVE write ever leave IER
 * (base+1, DLAB clear) nonzero? We track DLAB across the WHOLE trace (so the
 * latch state at live_mark is correct), because base+1 is DLM under DLAB. */
static int UartSimTraceIerArmed(const UartSim *s)
{
    int i;
    int dlab;
    dlab = 0;
    for (i = 0; i < s->trace_len; i++) {
        unsigned short off = (unsigned short)(s->trace[i].port - s->base_port);
        if (off == SIM_LCR) {
            dlab = (s->trace[i].val & SIM_LCR_DLAB) ? 1 : 0;
        } else if (off == SIM_IER && !dlab && i >= s->live_mark) {
            if (s->trace[i].val != 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* Scan the OUT-trace from live_mark onward: did any LIVE write set MCR OUT2
 * (bit 3)? (OUT2 must stay clear from open completion through close.) */
static int UartSimTraceOut2Set(const UartSim *s)
{
    int i;
    for (i = s->live_mark; i < s->trace_len; i++) {
        unsigned short off = (unsigned short)(s->trace[i].port - s->base_port);
        if (off == SIM_MCR && (s->trace[i].val & SIM_MCR_OUT2)) {
            return 1;
        }
    }
    return 0;
}

/* Captured live-TX bytes (THR writes outside loopback). */
static const unsigned char *UartSimTxBytes(const UartSim *s)
{
    return s->tx;
}
static int UartSimTxLen(const UartSim *s)
{
    return s->tx_len;
}
static unsigned long UartSimOps(const UartSim *s)
{
    return s->ops;
}

#endif /* UART_SIM_H */
