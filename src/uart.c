/*
 * uart.c - Win32s direct-UART serial route: the PURE detection ladder and
 * driving logic (task #37). Spec: specs/uart.allium; the frozen
 * contract is src/uart.h (each function's doc comment is the per-function spec).
 *
 * Everything here is OS-independent: it touches the 16550 ONLY through the
 * injected UartPortIo seam (in/out/yield), so the asm IN/OUT never enters the
 * host binary and the ladder is host-tested against a simulated 16550 (theft).
 * The real backend - the x86 inb/outb seam, the uart_read/uart_write/uart_close
 * vtable, the tier gate (UartTierWantsDirect) and the Transport wiring - lives
 * behind #ifndef UART_HOST_PURE (the integration session owns that block; it is
 * intentionally empty here).
 *
 * The register semantics below are a TRANSCRIPTION of the throwaway spike that
 * ran on real Win32s hardware (tools/phase6-qemu/uart-probe.c), not a datasheet
 * alone: the scratch-register AA/55 round-trip as the presence/8250-split probe,
 * FCR=0xE7 -> IIR top-bits for the FIFO detect, 19200 8N1 = divisor 6, THRE/DR
 * polling on LSR, and the MCR bit layout are all taken from that spike. The one
 * deliberate divergence (uart.h invariant 3): the spike set MCR OUT2 (0x0B); the
 * shipped backend keeps OUT2 CLEAR (MCR=0x03) so no UART interrupt can reach the
 * 8259 PIC.
 *
 * Hard constraints (CLAUDE.md): C89 only - declarations at block top, slash-star
 * comments, no floating point, no long long, i386. NO windows.h type here.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#include <stddef.h>   /* NULL - the pure section references NO windows.h type */
#include "uart.h"

/* ----------------------------------------------------------------------
 * 16550 register file (offsets from drv->base_port) and bit masks, transcribed
 * from the spike (R_RBR_THR=+0, R_IER_DLM=+1, R_IIR_FCR=+2, R_LCR=+3,
 * R_MCR=+4, R_LSR=+5, R_MSR=+6, R_SCR=+7).
 * ---------------------------------------------------------------------- */

#define UART_REG_RBR   0   /* read: RX buffer  / write: TX holding (DLAB=0) */
#define UART_REG_THR   0   /* alias of +0 for TX-side clarity */
#define UART_REG_DLL   0   /* divisor latch low  (DLAB=1)                   */
#define UART_REG_IER   1   /* interrupt enable   (DLAB=0)                   */
#define UART_REG_DLM   1   /* divisor latch high (DLAB=1)                   */
#define UART_REG_IIR   2   /* read: interrupt ident                         */
#define UART_REG_FCR   2   /* write: FIFO control                           */
#define UART_REG_LCR   3   /* line control (bit7 = DLAB)                    */
#define UART_REG_MCR   4   /* modem control (DTR/RTS/OUT1/OUT2/LOOP)        */
#define UART_REG_LSR   5   /* line status (DR/OE/PE/FE/BI/THRE/TEMT)        */
#define UART_REG_MSR   6   /* modem status (loopback reflects MCR here)     */
#define UART_REG_SCR   7   /* scratch (absent on the original 8250)         */

/* LCR */
#define UART_LCR_DLAB  0x80   /* divisor-latch access bit                   */
#define UART_LCR_8N1   0x03   /* 8 data bits, no parity, 1 stop             */

/* LSR */
#define UART_LSR_DR    0x01   /* data ready                                 */
#define UART_LSR_OE    0x02   /* overrun error                              */
#define UART_LSR_PE    0x04   /* parity error                               */
#define UART_LSR_FE    0x08   /* framing error                              */
#define UART_LSR_BI    0x10   /* break interrupt (spurious 0x00 follows)    */
#define UART_LSR_THRE  0x20   /* TX holding (or FIFO) empty                 */
#define UART_LSR_TEMT  0x40   /* transmitter (shift register) empty         */

/* MCR */
#define UART_MCR_DTR   0x01
#define UART_MCR_RTS   0x02
#define UART_MCR_OUT1  0x04
#define UART_MCR_OUT2  0x08
#define UART_MCR_LOOP  0x10

/* MCR used live: DTR|RTS, OUT2 CLEAR (uart.h invariant 3). */
#define UART_MCR_LIVE  (UART_MCR_DTR | UART_MCR_RTS)
/* MCR used for the internal loopback self-test: LOOP + DTR|RTS|OUT1, OUT2 still
 * CLEAR. (OUT1/OUT2/RTS/DTR reflect into MSR RI/DCD/CTS/DSR under loopback.) */
#define UART_MCR_TEST  (UART_MCR_LOOP | UART_MCR_DTR | UART_MCR_RTS | UART_MCR_OUT1)

/* MSR bits the loopback wiring reflects MCR into (DTR->DSR, RTS->CTS,
 * OUT1->RI, OUT2->DCD). The test asserts CTS reflects RTS. */
#define UART_MSR_CTS   0x10   /* reflects MCR RTS under loopback            */
#define UART_MSR_DSR   0x20   /* reflects MCR DTR under loopback            */

/* IIR FIFO-detect: write FCR=0xE7 (FIFO enable+clear+14-byte trigger+64-byte
 * probe bit), read IIR top two bits. 0xC0 => a working 16550A FIFO; 0x80 =>
 * the broken non-A 16550 (treat as no-FIFO); 0x00 => no FIFO (8250/16450). */
#define UART_IIR_FIFO_MASK   0xC0
#define UART_IIR_FIFO_16550A 0xC0   /* the ONE load-bearing value (invariant 4) */
#define UART_IIR_FIFO_NONA   0x80

/* FCR writes: 0xE7 is the detect probe; 0xC7 enables the FIFO live (enable +
 * clear RX/TX + 14-byte RX trigger); 0x00 disables it. */
#define UART_FCR_DETECT  0xE7
#define UART_FCR_ENABLE  0xC7
#define UART_FCR_OFF     0x00

/* Scratch-register presence probe patterns (the spike used 0xAA / 0x55). */
#define UART_SCRATCH_A   0xAA
#define UART_SCRATCH_B   0x55

/* Loopback sentinel byte (the spike's 0xAE). */
#define UART_LOOP_SENTINEL 0xAE

/* A floating bus reads all-ones. */
#define UART_FLOATING    0xFF

/* Hard iteration ceiling for EVERY bounded poll loop (open-phase + TX). On
 * expiry the loop fails closed. RX (UartRxDrain) is the sole exception. */
#define UART_POLL_BOUND  100000

/* ----------------------------------------------------------------------
 * UartDriverInit - zero a driver and set its base port (single-byte defaults).
 * ---------------------------------------------------------------------- */
void UartDriverInit(UartDriver *drv, unsigned short base_port)
{
    drv->base_port     = base_port;
    drv->chip_kind     = UART_CHIP_UNKNOWN;
    drv->fifo_enabled  = 0;
    drv->tx_chunk      = UART_SINGLE_TX_CHUNK;
    drv->divisor       = 0;
    drv->overrun_count = 0;
    drv->open          = 0;
}

/* ----------------------------------------------------------------------
 * UartDetect - the presence + identification ladder (uart.h step order).
 * Returns 1 present (and sets chip_kind/fifo_enabled/tx_chunk), 0 absent
 * (chip_kind left UNKNOWN).
 *
 * (1) PRESENCE - the IER store-test (uart.h): write 0x00 then 0x0F to IER
 *     (base+1, DLAB clear), read back masking the low nibble. A real UART has
 *     a writable IER low nibble (upper bits read 0), so 0x0F round-trips; a
 *     floating/absent bus reads 0xFF (low nibble 0xF for the 0x0F write but
 *     0xF != 0x0 for the 0x00 write) - the 0x00 write is the discriminator.
 *     A mismatch on either => absent, chip_kind UNKNOWN.
 * (2) FIFO probe (FCR=0xE7 -> IIR): (IIR & 0xC0)==0xC0 => 16550A with the FIFO
 *     enabled; 0x80 (broken non-A) or 0x00 => single-byte. THE ONE LOAD-BEARING
 *     RULE: fifo_enabled iff (IIR & 0xC0)==0xC0 (invariant 4).
 * (3) Scratch test (base+7) splits 8250 vs 16450 (a 16450/16550 round-trips
 *     0xAA/0x55; the 8250 has no scratch) but drives NO behavioural branch.
 * ---------------------------------------------------------------------- */
int UartDetect(const UartPortIo *io, UartDriver *drv)
{
    unsigned short base;
    unsigned char  ier_zero;
    unsigned char  ier_set;
    unsigned char  iir;
    unsigned char  s1;
    unsigned char  s2;
    int            present;
    int            fifo_is_16550a;
    int            scratch_ok;

    base = drv->base_port;

    /* (1) Presence: the IER store-test. Make sure DLAB is clear so base+1 is
     * IER, not DLM. Write 0x00 then 0x0F, masking the low nibble on readback.
     * A floating bus reads 0xFF (low nibble 0xF) for the 0x00 write -> absent. */
    io->out(io->ctx, (unsigned short)(base + UART_REG_LCR), UART_LCR_8N1);
    io->out(io->ctx, (unsigned short)(base + UART_REG_IER), 0x00);
    ier_zero = (unsigned char)(io->in(io->ctx,
                   (unsigned short)(base + UART_REG_IER)) & 0x0F);
    io->out(io->ctx, (unsigned short)(base + UART_REG_IER), 0x0F);
    ier_set = (unsigned char)(io->in(io->ctx,
                   (unsigned short)(base + UART_REG_IER)) & 0x0F);
    present = (ier_zero == 0x00 && ier_set == 0x0F);
    /* Restore IER quiescent. */
    io->out(io->ctx, (unsigned short)(base + UART_REG_IER), 0x00);

    if (!present) {
        drv->chip_kind    = UART_CHIP_UNKNOWN;
        drv->fifo_enabled = 0;
        drv->tx_chunk     = UART_SINGLE_TX_CHUNK;
        return 0;
    }

    /* (2) FIFO probe: FCR=0xE7, then read IIR top bits. The sole driver of
     * fifo_enabled (invariant 4): only a positively-detected 16550A FIFO. */
    io->out(io->ctx, (unsigned short)(base + UART_REG_FCR), UART_FCR_DETECT);
    iir = io->in(io->ctx, (unsigned short)(base + UART_REG_IIR));
    fifo_is_16550a = ((iir & UART_IIR_FIFO_MASK) == UART_IIR_FIFO_16550A);

    /* (3) Scratch split (8250 vs 16450) - names the chip, drives no branch. */
    io->out(io->ctx, (unsigned short)(base + UART_REG_SCR), UART_SCRATCH_A);
    s1 = io->in(io->ctx, (unsigned short)(base + UART_REG_SCR));
    io->out(io->ctx, (unsigned short)(base + UART_REG_SCR), UART_SCRATCH_B);
    s2 = io->in(io->ctx, (unsigned short)(base + UART_REG_SCR));
    scratch_ok = (s1 == UART_SCRATCH_A && s2 == UART_SCRATCH_B);

    if (fifo_is_16550a) {
        drv->chip_kind    = UART_CHIP_16550A;
        drv->fifo_enabled = 1;
        drv->tx_chunk     = UART_FIFO_TX_CHUNK;
    } else {
        /* A broken non-A 16550 (0x80) or no FIFO at all (0x00) => single-byte.
         * The scratch split distinguishes 16450 (round-trips) from 8250 (no
         * scratch); chip_kind drives no behaviour either way. */
        drv->chip_kind    = scratch_ok ? UART_CHIP_16450 : UART_CHIP_8250;
        drv->fifo_enabled = 0;
        drv->tx_chunk     = UART_SINGLE_TX_CHUNK;
    }
    return 1;
}

/* ----------------------------------------------------------------------
 * UartProgramDivisor - program the baud divisor and READ IT BACK. Returns 1 iff
 * the readback matches (sets drv->divisor); 0 if a clone ignored the write.
 * ---------------------------------------------------------------------- */
int UartProgramDivisor(const UartPortIo *io, UartDriver *drv,
                       unsigned int divisor)
{
    unsigned short base;
    unsigned char  want_lo;
    unsigned char  want_hi;
    unsigned char  got_lo;
    unsigned char  got_hi;

    base    = drv->base_port;
    want_lo = (unsigned char)(divisor & 0xFF);
    want_hi = (unsigned char)((divisor >> 8) & 0xFF);

    /* DLAB=1, write DLL/DLM, then LCR=8N1 (DLAB clear). */
    io->out(io->ctx, (unsigned short)(base + UART_REG_LCR), UART_LCR_DLAB);
    io->out(io->ctx, (unsigned short)(base + UART_REG_DLL), want_lo);
    io->out(io->ctx, (unsigned short)(base + UART_REG_DLM), want_hi);
    io->out(io->ctx, (unsigned short)(base + UART_REG_LCR), UART_LCR_8N1);

    /* Re-assert DLAB and read the latch back, then clear DLAB. */
    io->out(io->ctx, (unsigned short)(base + UART_REG_LCR), UART_LCR_DLAB);
    got_lo = io->in(io->ctx, (unsigned short)(base + UART_REG_DLL));
    got_hi = io->in(io->ctx, (unsigned short)(base + UART_REG_DLM));
    io->out(io->ctx, (unsigned short)(base + UART_REG_LCR), UART_LCR_8N1);

    if (got_lo != want_lo || got_hi != want_hi) {
        return 0;   /* a clone ignored the divisor write -> fail closed */
    }
    drv->divisor = divisor;
    return 1;
}

/* ----------------------------------------------------------------------
 * UartLoopbackSelfTest - internal-loopback echo test. Returns 1 on success, 0 on failure
 * (or DR-poll bound expiry). Restores MCR with OUT2 CLEAR afterward.
 * ---------------------------------------------------------------------- */
int UartLoopbackSelfTest(const UartPortIo *io, UartDriver *drv)
{
    unsigned short base;
    unsigned char  lsr;
    unsigned char  rbr;
    unsigned char  msr;
    int            guard;
    int            data_ready;

    base = drv->base_port;

    /* Enter internal loopback (LOOP|DTR|RTS|OUT1, OUT2 CLEAR). */
    io->out(io->ctx, (unsigned short)(base + UART_REG_MCR), UART_MCR_TEST);

    /* Send the sentinel and poll DR (BOUNDED). */
    io->out(io->ctx, (unsigned short)(base + UART_REG_THR), UART_LOOP_SENTINEL);
    data_ready = 0;
    for (guard = 0; guard < UART_POLL_BOUND; guard++) {
        lsr = io->in(io->ctx, (unsigned short)(base + UART_REG_LSR));
        if (lsr & UART_LSR_DR) {
            data_ready = 1;
            break;
        }
        io->yield(io->ctx);
    }

    if (!data_ready) {
        /* Never echoed: dead / clone chip. Restore quiescent MCR, fail closed. */
        io->out(io->ctx, (unsigned short)(base + UART_REG_MCR), UART_MCR_LIVE);
        return 0;
    }

    /* The echoed byte must be the sentinel. */
    rbr = io->in(io->ctx, (unsigned short)(base + UART_REG_RBR));

    /* Confirm the MCR modem bits reflect into MSR (loopback wiring): RTS->CTS,
     * DTR->DSR. A chip that decodes registers but does not truly loop fails. */
    msr = io->in(io->ctx, (unsigned short)(base + UART_REG_MSR));

    /* Restore MCR leaving OUT2 CLEAR (invariant 3) regardless of outcome. */
    io->out(io->ctx, (unsigned short)(base + UART_REG_MCR), UART_MCR_LIVE);

    if (rbr != UART_LOOP_SENTINEL) {
        return 0;
    }
    if ((msr & (UART_MSR_CTS | UART_MSR_DSR)) != (UART_MSR_CTS | UART_MSR_DSR)) {
        return 0;   /* modem-control did not loop back -> clone, fail closed */
    }
    return 1;
}

/* ----------------------------------------------------------------------
 * UartClearStale - read LSR, RBR, IIR, MSR once each and discard, draining any
 * stale status / data left by a previous session. (Local helper; not exported.)
 * ---------------------------------------------------------------------- */
static void UartClearStale(const UartPortIo *io, UartDriver *drv)
{
    unsigned short base;
    base = drv->base_port;
    (void)io->in(io->ctx, (unsigned short)(base + UART_REG_LSR));
    (void)io->in(io->ctx, (unsigned short)(base + UART_REG_RBR));
    (void)io->in(io->ctx, (unsigned short)(base + UART_REG_IIR));
    (void)io->in(io->ctx, (unsigned short)(base + UART_REG_MSR));
}

/* ----------------------------------------------------------------------
 * UartOpenSequence - the whole open ladder. Returns UART_OPEN_LIVE or
 * UART_OPEN_FAILED. Leaves IER=0 and MCR OUT2 clear on success (invariant 3);
 * every loop here is hard-bounded and fail-closed (invariant 6).
 * ---------------------------------------------------------------------- */
int UartOpenSequence(const UartPortIo *io, UartDriver *drv,
                     unsigned short base_port, unsigned int divisor)
{
    unsigned char fcr;

    UartDriverInit(drv, base_port);

    /* Clear any stale status/data before probing. */
    UartClearStale(io, drv);

    /* Presence + identification. */
    if (!UartDetect(io, drv)) {
        return UART_OPEN_FAILED;
    }

    /* Program + read-back-verify the divisor. */
    if (!UartProgramDivisor(io, drv, divisor)) {
        return UART_OPEN_FAILED;
    }

    /* FIFO on only for a detected 16550A (invariant 4); else explicitly off. */
    fcr = (drv->chip_kind == UART_CHIP_16550A) ? UART_FCR_ENABLE : UART_FCR_OFF;
    io->out(io->ctx, (unsigned short)(base_port + UART_REG_FCR), fcr);

    /* MCR = DTR|RTS (OUT2 CLEAR) and IER = 0 (no interrupt path armed). */
    io->out(io->ctx, (unsigned short)(base_port + UART_REG_MCR), UART_MCR_LIVE);
    io->out(io->ctx, (unsigned short)(base_port + UART_REG_IER), 0x00);

    /* Fail-closed loopback self-test. */
    if (!UartLoopbackSelfTest(io, drv)) {
        return UART_OPEN_FAILED;
    }

    /* Final stale-clear and go live. */
    UartClearStale(io, drv);
    drv->open = 1;
    return UART_OPEN_LIVE;
}

/* ----------------------------------------------------------------------
 * UartTxChunk - bounded polled TX. Polls LSR THRE (BOUNDED with yields) before
 * each burst of up to drv->tx_chunk bytes. Returns bytes written (== len) or
 * <0 if a THRE wait expired.
 * ---------------------------------------------------------------------- */
int UartTxChunk(const UartPortIo *io, UartDriver *drv,
                const unsigned char *buf, int len)
{
    unsigned short base;
    int            sent;
    int            chunk;
    int            i;
    int            guard;
    int            thre_ready;

    base  = drv->base_port;
    chunk = drv->tx_chunk;
    if (chunk < 1) {
        chunk = UART_SINGLE_TX_CHUNK;
    }

    sent = 0;
    while (sent < len) {
        int burst;

        /* Poll THRE (BOUNDED). THRE means TX holding (or, on a 16550A FIFO,
         * the whole FIFO) is empty, so a <=chunk burst is safe. */
        thre_ready = 0;
        for (guard = 0; guard < UART_POLL_BOUND; guard++) {
            if (io->in(io->ctx, (unsigned short)(base + UART_REG_LSR))
                & UART_LSR_THRE) {
                thre_ready = 1;
                break;
            }
            io->yield(io->ctx);
        }
        if (!thre_ready) {
            return -1;   /* THRE never asserted -> comms error, fail closed */
        }

        burst = len - sent;
        if (burst > chunk) {
            burst = chunk;
        }
        for (i = 0; i < burst; i++) {
            io->out(io->ctx, (unsigned short)(base + UART_REG_THR),
                    buf[sent + i]);
        }
        sent += burst;
    }
    return sent;
}

/* ----------------------------------------------------------------------
 * UartRxDrain - steady-state polled RX (THE SOLE unbounded loop). Yields-and-
 * repolls forever on an idle line (NEVER returns 0). Reads LSR ONCE per byte
 * and decodes OE/PE/FE/BI from that snapshot BEFORE reading RBR (reading LSR
 * clears the error bits, so the order is mandatory). Returns bytes read (>0)
 * or <0 on a real comms error.
 * ---------------------------------------------------------------------- */
int UartRxDrain(const UartPortIo *io, UartDriver *drv,
                unsigned char *buf, int len)
{
    unsigned short base;
    int            got;
    unsigned char  lsr;
    unsigned char  byte;

    base = drv->base_port;
    got  = 0;

    /*
     * The outer loop guarantees a return of >0 (real data) or <0 (a comms
     * error) - NEVER 0. An idle serial line is a live session (invariant 6's
     * exception): block-yield until data-ready. A poll pass that consumes ONLY
     * a line break (a spurious 0x00 dropped below) is neither data nor a fatal
     * fault, so it loops back to the wait rather than returning 0 - which the
     * transport contract reserves for a peer close a serial line cannot have.
     */
    for (;;) {
        /* Block-yielding until DR asserts. */
        for (;;) {
            lsr = io->in(io->ctx, (unsigned short)(base + UART_REG_LSR));
            if (lsr & UART_LSR_DR) {
                break;
            }
            io->yield(io->ctx);
        }

        /* Drain while DR stays set, up to len. The LSR snapshot read at the top
         * of each iteration is decoded BEFORE the RBR read (reading LSR clears
         * the error bits, so the order is mandatory). */
        while (got < len) {
            if (!(lsr & UART_LSR_DR)) {
                break;   /* FIFO emptied */
            }

            /* Decode errors from the snapshot BEFORE touching RBR. */
            if (lsr & UART_LSR_OE) {
                drv->overrun_count++;
            }
            if (lsr & UART_LSR_BI) {
                /* Break: the accompanying RBR byte is a spurious 0x00. Break +
                 * framing error is a hard line error; a lone break drops the
                 * spurious byte and the wait resumes below (not data, not fatal). */
                byte = io->in(io->ctx, (unsigned short)(base + UART_REG_RBR));
                (void)byte;
                if (lsr & UART_LSR_FE) {
                    return -1;   /* break + framing = a real comms error */
                }
            } else {
                byte = io->in(io->ctx, (unsigned short)(base + UART_REG_RBR));
                buf[got] = byte;
                got++;
            }

            /* Re-snapshot LSR for the next iteration's DR test + error decode. */
            lsr = io->in(io->ctx, (unsigned short)(base + UART_REG_LSR));
        }

        if (got > 0) {
            return got;   /* delivered real data */
        }
        /* Only break-drops this pass: still a live session - keep waiting. */
    }
}

/* ----------------------------------------------------------------------
 * UartDrainAndClose - drain the shift register on TEMT (BOUNDED), then quiesce
 * (IER=0, MCR=0: DTR/RTS/OUT2 all clear) and mark closed.
 * ---------------------------------------------------------------------- */
void UartDrainAndClose(const UartPortIo *io, UartDriver *drv)
{
    unsigned short base;
    int            guard;

    base = drv->base_port;

    /* Wait for the transmitter (shift register) to empty (BOUNDED). */
    for (guard = 0; guard < UART_POLL_BOUND; guard++) {
        if (io->in(io->ctx, (unsigned short)(base + UART_REG_LSR))
            & UART_LSR_TEMT) {
            break;
        }
        io->yield(io->ctx);
    }

    io->out(io->ctx, (unsigned short)(base + UART_REG_IER), 0x00);
    io->out(io->ctx, (unsigned short)(base + UART_REG_MCR), 0x00);
    drv->open = 0;
}

#ifndef UART_HOST_PURE
/*
 * The real backend: the x86 IN/OUT port-I/O seam, the cooperative yield, the
 * uart_read/uart_write/uart_close Transport vtable, the tier gate
 * (UartTierWantsDirect) and UartBackendOpenDirect. Compiled target-only - it
 * needs <windows.h>/feat.h/transport.h and the i386-only asm, none of which
 * enter the host-pure build above. (UartLastRouteForTest, the route-dispatch
 * probe, lives in serial.c where the dispatch branch is.)
 */
#include <windows.h>
#include "transport.h"
#include "feat.h"

/* ----------------------------------------------------------------------
 * The bare ring-3 port-I/O seam (SECURITY PIN #2, BarePortIoNoEscalation): a
 * single IN / a single OUT and nothing else - no VxD, IOPL change, call gate,
 * IRQ hook or ring transition. Double-guarded: the #ifndef UART_HOST_PURE wall
 * above is the primary exclusion (the host is x86_64 with no UART); this inner
 * i386 guard is belt-and-braces. MinGW uses GCC extended asm (the spike's exact
 * forms); VC6 uses __asm{}. Any other compiler is a hard error.
 * ---------------------------------------------------------------------- */
#if defined(__i386__) || defined(_M_IX86)

#if defined(__GNUC__)
static unsigned char uart_real_in(void *ctx, unsigned short port)
{
    unsigned char v;
    (void)ctx;
    __asm__ __volatile__ ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static void uart_real_out(void *ctx, unsigned short port, unsigned char val)
{
    (void)ctx;
    __asm__ __volatile__ ("outb %0, %1" : : "a"(val), "Nd"(port));
}
#elif defined(_MSC_VER)
static unsigned char uart_real_in(void *ctx, unsigned short port)
{
    unsigned char v;
    (void)ctx;
    __asm {
        mov dx, port
        in  al, dx
        mov v, al
    }
    return v;
}
static void uart_real_out(void *ctx, unsigned short port, unsigned char val)
{
    (void)ctx;
    __asm {
        mov dx, port
        mov al, val
        out dx, al
    }
}
#else
#error "direct-UART port I/O needs GCC extended asm or MSVC __asm (i386)"
#endif

#else
#error "the Win32s direct-UART backend is i386-only (bare IN/OUT port I/O)"
#endif

/*
 * Cooperative yield (the pump in SECURITY PIN #6's bounded loops + the idle RX
 * wait): Sleep(0) relinquishes the rest of the timeslice without a fixed delay,
 * so the bounded open-phase polls still fail closed fast while steady-state RX
 * does not peg the cooperatively-scheduled system. Sleep is on the Win32s import
 * floor (kernel32). The seam abstracts this: swapping to a PeekMessage pump is a
 * one-line change if the guest proves a console app must drain its queue (the
 * open hardware question recorded in the plan).
 */
static void uart_real_yield(void *ctx)
{
    (void)ctx;
    Sleep(0);
}

/* The single live-session driver state + its real-seam UartPortIo. No heap - the
 * device is single-client-sequential and single-threaded. */
static UartDriver g_uart;
static UartPortIo g_uart_io;

static void uart_seam_init(void)
{
    g_uart_io.in    = uart_real_in;
    g_uart_io.out   = uart_real_out;
    g_uart_io.yield = uart_real_yield;
    g_uart_io.ctx   = NULL;
}

/* ----------------------------------------------------------------------
 * The Transport vtable closures over the static driver. read returns >0 / <0
 * (UartRxDrain NEVER returns 0 - a serial line has no orderly close); write
 * returns bytes / <0; close quiesces the chip. There is no OS handle.
 * ---------------------------------------------------------------------- */
static int uart_read(Transport *t, void *buf, int len)
{
    return UartRxDrain(&g_uart_io, (UartDriver *)t->io.ptr,
                       (unsigned char *)buf, len);
}
static int uart_write(Transport *t, const void *buf, int len)
{
    return UartTxChunk(&g_uart_io, (UartDriver *)t->io.ptr,
                       (const unsigned char *)buf, len);
}
static void uart_close(Transport *t)
{
    UartDrainAndClose(&g_uart_io, (UartDriver *)t->io.ptr);
}

/* ----------------------------------------------------------------------
 * Map "COM1".."COM4" to its ISA base port; 0 if unrecognised. Case-insensitive
 * on the COM prefix.
 * ---------------------------------------------------------------------- */
static unsigned short uart_base_for_port(const char *port)
{
    if (port == NULL) {
        return 0;
    }
    if ((port[0] == 'C' || port[0] == 'c') &&
        (port[1] == 'O' || port[1] == 'o') &&
        (port[2] == 'M' || port[2] == 'm')) {
        switch (port[3]) {
        case '1': return 0x3F8;
        case '2': return 0x2F8;
        case '3': return 0x3E8;
        case '4': return 0x2E8;
        default:  break;
        }
    }
    return 0;
}

/* ----------------------------------------------------------------------
 * The baud divisor (115200 / baud; the 1.8432 MHz / 16 reference clock). Every
 * standard rate from 300..115200 is an exact divisor of 115200; a 0 or
 * non-standard baud falls back to the 19200 divisor (6), the Win32s ceiling the
 * /BAUD flag drives.
 * ---------------------------------------------------------------------- */
static unsigned int uart_divisor_for_baud(DWORD baud)
{
    if (baud == 0 || (115200UL % baud) != 0) {
        return 6;
    }
    if ((115200UL / baud) == 0 || (115200UL / baud) > 0xFFFFUL) {
        return 6;
    }
    return (unsigned int)(115200UL / baud);
}

/* ----------------------------------------------------------------------
 * UartTierWantsDirect - THE TIER GATE (SECURITY PIN #1). Returns
 * g_features.is_win32s; NO port I/O, so the dispatch test can flip the tier and
 * read the decision without faulting on NT.
 * ---------------------------------------------------------------------- */
int UartTierWantsDirect(void)
{
    return g_features.is_win32s;
}

/* ----------------------------------------------------------------------
 * UartBackendOpenDirect - open a direct-UART serial Transport. SECURITY: this is
 * the dangerous branch, reachable ONLY under UartTierWantsDirect() (the
 * SerialBackendOpen first-line gate). A failed open is TERMINAL - no COMM.DRV
 * degrade (invariant 5, UartOwnedExclusively): this route never opened COM1 via
 * the OS, so there is nothing to fall back to.
 * ---------------------------------------------------------------------- */
int UartBackendOpenDirect(const TransportConfig *cfg, Transport *out,
                          char *err, int errSize)
{
    unsigned short base;
    unsigned int   divisor;

    base = uart_base_for_port(cfg->port);
    if (base == 0) {
        if (err != NULL && errSize > 0) {
            lstrcpynA(err, "win32s direct-uart: unknown COM port", errSize);
        }
        return 0;
    }

    uart_seam_init();
    divisor = uart_divisor_for_baud(cfg->baudRate);

    if (UartOpenSequence(&g_uart_io, &g_uart, base, divisor) != UART_OPEN_LIVE) {
        if (err != NULL && errSize > 0) {
            lstrcpynA(err,
                "win32s direct-uart: no working UART at the COM port", errSize);
        }
        return 0;   /* terminal - no degrade to the OS comm path (invariant 5) */
    }

    out->name   = "serial";
    out->kind   = TRANSPORT_SERIAL;
    out->flags  = 0;
    out->read   = uart_read;
    out->write  = uart_write;
    out->close  = uart_close;
    out->accept = NULL;          /* point-to-point: no accept */
    out->io.ptr = &g_uart;
    return 1;
}

#ifdef TEST_BUILD
/* Clear the static driver + re-arm the seam so each on-target test starts from a
 * known state. */
void UartResetForTest(void)
{
    UartDriverInit(&g_uart, 0);
    uart_seam_init();
}
#endif

#endif /* UART_HOST_PURE */
