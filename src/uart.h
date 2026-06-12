/*
 * uart.h - Win32s direct-UART serial route (work-item 6.2 / task #37)
 *
 * Spec: uart.allium. The tier-aware serial backend's Win32s-only internal
 * route: where the Win32 communications API is an exported-but-stubbed no-op
 * (SetCommState -> err 120 on Win32s 1.25a), the device drives the 16550 UART
 * by BARE ring-3 port I/O (IN/OUT to base+0..base+7, COM1 base 0x3F8). The
 * transport KIND and wire NAME stay "serial" - this route is invisible above
 * the backend. Win9x/NT keep the CreateFileA + SetCommState path (serial.c).
 *
 * SECURITY BOUNDARY (uart.allium invariants; weed's gate-bypass dimension
 * audits these - do NOT weaken without the spec changing too):
 *   1 ServingViaUartImpliesWin32s   - the direct route is reached ONLY when
 *                                     g_features.is_win32s (bare IN/OUT #GPs at
 *                                     ring-3 IOPL-0 on NT). SerialBackendOpen
 *                                     MUST gate on UartTierWantsDirect() as its
 *                                     FIRST line, the sole selector; this holds
 *                                     even through TransportOpen's /AUTO serial
 *                                     fallback, which re-enters SerialBackendOpen.
 *   2 BarePortIoNoEscalation        - only IN/OUT. No VxD, IOPL change, call
 *                                     gate, IRQ hook, ring transition.
 *   3 NoInterruptPathArmed          - while live, IER=0 AND MCR OUT2 (bit 3)
 *                                     CLEAR, so no UART interrupt can reach the
 *                                     8259 PIC. (NB: stricter than the spike,
 *                                     which set OUT2=0x0B; the shipped backend
 *                                     uses MCR=0x03 = DTR|RTS, OUT2 clear.)
 *   4 FifoEnabledImpliesDetected16550A - FIFO/multi-byte burst ONLY on a chip
 *                                     positively detected as 16550A
 *                                     (IIR & 0xC0 == 0xC0). A 0x80 readback is
 *                                     the broken non-A 16550 -> treat as no-FIFO.
 *   5 UartOwnedExclusively          - COM1 is NEVER opened via the OS
 *                                     (CreateFileA/OpenComm) on this route, so
 *                                     COMM.DRV stays passive; a failed open is
 *                                     terminal (no degrade to the OS comm path).
 *   6 EveryPollLoopBounded          - every OPEN-phase / TX poll loop is
 *                                     HARD-BOUNDED and fail-closed; the SOLE
 *                                     exception is steady-state RX (UartRxDrain),
 *                                     which yields-and-repolls indefinitely (an
 *                                     idle serial line is a live session).
 *
 * The IN/OUT atom can never run under Wine/WSL (x86_64 host, no UART, bare IN
 * faults), so the detection-ladder + driving logic is PURE over an injected
 * port-I/O seam (UartPortIo) and host-tested against a simulated 16550 (theft).
 * The asm seam + the Transport wiring live behind #ifndef UART_HOST_PURE and
 * are verified live on the pinned Win32s guest (the #35 acceptance).
 *
 * Hard constraints (CLAUDE.md): C89 only, declarations at block top, slash-star
 * comments, no floating point, i386, ANSI APIs, no threads. The direct route
 * adds ZERO imports (bare asm; Sleep/PeekMessageA via kernel32/user32 already
 * imported) - asserted by the import-allowlist CI guard.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef UART_H
#define UART_H

/* The detected UART generation (spec: enum ChipKind). The 8250, 16450 and the
 * non-A 16550 are ALL driven single-byte; only a positively-detected 16550A
 * enables the FIFO. UART_CHIP_UNKNOWN is the absent/floating-bus result. */
typedef enum {
    UART_CHIP_UNKNOWN = 0,
    UART_CHIP_8250,
    UART_CHIP_16450,
    UART_CHIP_16550A
} UartChipKind;

/* TX burst sizes (spec: config.fifo_tx_chunk / single_byte_tx_chunk). The
 * 16550A's 16-byte FIFO is the conservative burst ceiling on a POSITIVELY
 * detected 16550A; every lesser chip is driven one byte at a time. */
#define UART_FIFO_TX_CHUNK    16
#define UART_SINGLE_TX_CHUNK  1

/* UartOpenSequence outcomes (1 = success, project convention). */
#define UART_OPEN_LIVE        1   /* detected, looped-back, divisor-verified, live */
#define UART_OPEN_FAILED      0   /* fail-closed: absent / loopback / divisor / clone */

/*
 * UartPortIo - the injected port-I/O seam. The real build wires `in`/`out` to
 * the x86 inb/outb asm (compiled only under #ifndef UART_HOST_PURE and only on
 * i386 targets); the host tests wire them to a simulated-16550 register machine.
 * `yield` is the cooperative pump called between bounded re-polls (Win 3.11 is
 * non-preemptive): the real build uses Sleep(0)/Sleep(1) (or, if the guest
 * proves it necessary, a PeekMessage pump - resolved on hardware);
 * the host build uses a no-op (the bound is asserted by an op counter, not time).
 * `ctx` is passed back to in/out/yield (the simulated chip in tests; unused real).
 */
typedef struct UartPortIo {
    unsigned char (*in)(void *ctx, unsigned short port);
    void          (*out)(void *ctx, unsigned short port, unsigned char val);
    void          (*yield)(void *ctx);
    void          *ctx;
} UartPortIo;

/*
 * UartDriver - the single live-session driver state. One file-static instance
 * (no heap; the device is single-client-sequential and single-threaded), keyed
 * by Transport.io.ptr. `fifo_enabled`/`tx_chunk` are DERIVED at detection and
 * bound by invariant 4 (true / >1 only for a detected 16550A).
 */
typedef struct UartDriver {
    unsigned short base_port;      /* 0x3F8 COM1, 0x2F8 COM2, 0x3E8 COM3, 0x2E8 COM4 */
    UartChipKind   chip_kind;
    int            fifo_enabled;
    int            tx_chunk;       /* UART_SINGLE_TX_CHUNK or UART_FIFO_TX_CHUNK */
    unsigned int   divisor;        /* programmed + read-back-verified baud divisor */
    unsigned long  overrun_count;  /* LSR OE seen during RX (diagnostic) */
    int            open;           /* 1 once live; 0 before open / after close */
} UartDriver;

/* ----------------------------------------------------------------------
 * Pure logic - OS-independent, NO windows.h type. Compiled into the theft
 * host harness AND the real build; driven through UartPortIo so the asm never
 * enters the host binary. Every function below is total and (except RX) bounded.
 * ---------------------------------------------------------------------- */

/*
 * UartDriverInit - Zero a driver and set its base port. Called at the top of
 * every open so a re-open never inherits stale chip_kind/fifo/overrun state.
 */
void UartDriverInit(UartDriver *drv, unsigned short base_port);

/*
 * UartDetect - The presence + identification ladder (spec rules UartPortAbsent,
 * UartChipIdentified). (1) IER store-test: write 0x00/0x0F to base+1, read back
 * masked; a floating bus (0xFF) or mismatch => returns 0 (absent), drv->chip_kind
 * left UART_CHIP_UNKNOWN. (2) FIFO probe: write FCR=0xE7 to base+2, read IIR;
 * IIR&0xC0==0xC0 => UART_CHIP_16550A with fifo_enabled=1, tx_chunk=UART_FIFO_TX_CHUNK;
 * ==0x80 (broken non-A) or ==0x00 => fifo_enabled=0, tx_chunk=UART_SINGLE_TX_CHUNK.
 * (3) scratch test (base+7, two patterns w/ an intervening foreign read) splits
 * 8250 vs 16450 but drives NO behavioural branch. Returns 1 present, 0 absent.
 * THE ONE LOAD-BEARING RULE: fifo_enabled iff IIR&0xC0==0xC0 (invariant 4).
 */
int UartDetect(const UartPortIo *io, UartDriver *drv);

/*
 * UartProgramDivisor - Program the baud divisor and READ IT BACK (spec: the
 * read-back-verify half of UartGoesLive). Set LCR=0x80 (DLAB), write DLL/DLM,
 * set LCR=0x03 (8N1, DLAB clear); then re-assert DLAB, read DLL/DLM, compare,
 * clear DLAB. Returns 1 iff the readback matches (a clone that ignored the
 * write fails -> open fails). Sets drv->divisor on success.
 */
int UartProgramDivisor(const UartPortIo *io, UartDriver *drv, unsigned int divisor);

/*
 * UartLoopbackSelfTest - Fail-closed loopback (spec: UartOpenFailsLoopback).
 * Set MCR LOOP (bit4); send 0xAE to THR; poll LSR DR (BOUNDED) and read RBR ==
 * 0xAE; confirm MCR->MSR reflects (LOOP|OUT2|RTS gives MSR DCD/CTS). A dead /
 * clone chip that decodes registers but does not truly loop fails here. Restore
 * MCR (OUT2 left CLEAR) afterward. Returns 1 on success, 0 on failure (or the DR poll bound
 * expiring -> fail-closed).
 */
int UartLoopbackSelfTest(const UartPortIo *io, UartDriver *drv);

/*
 * UartOpenSequence - The whole open ladder, orchestrating the above. Steps:
 * UartDriverInit(base) -> clear stale (read LSR,RBR,IIR,MSR) -> UartDetect
 * (0 => UART_OPEN_FAILED) -> UartProgramDivisor (0 => FAILED) -> set FCR
 * (0xC7 on a 16550A else 0x00) -> MCR=0x03 (DTR|RTS, OUT2 CLEAR) -> IER=0x00 ->
 * UartLoopbackSelfTest (0 => FAILED) -> final clear stale -> drv->open=1.
 * Returns UART_OPEN_LIVE or UART_OPEN_FAILED. ALL loops here are hard-bounded
 * and fail-closed (invariant 6); leaves IER=0 and OUT2 clear on success
 * (invariant 3).
 */
int UartOpenSequence(const UartPortIo *io, UartDriver *drv,
                     unsigned short base_port, unsigned int divisor);

/*
 * UartTxChunk - Bounded polled TX (spec: the live TX discipline). Poll LSR THRE
 * (0x20, BOUNDED with yields) then write up to drv->tx_chunk bytes to THR;
 * repeat until `len` is sent. tx_chunk is 1 except on a detected 16550A (where
 * THRE means "FIFO empty", so a <=16-byte burst is safe - invariant 4). Returns
 * bytes written (== len) or <0 if a THRE wait expired (a comms error).
 */
int UartTxChunk(const UartPortIo *io, UartDriver *drv,
                const unsigned char *buf, int len);

/*
 * UartRxDrain - Steady-state polled RX (spec: the live RX discipline; the SOLE
 * unbounded loop, invariant 6's exception). Poll LSR DR (0x01), yielding to the
 * cooperative scheduler between re-polls; an idle line blocks-yielding FOREVER
 * (never returns 0 - a serial line has no orderly close). Read LSR ONCE, decode
 * OE/PE/FE/BI from that snapshot (OE -> overrun_count++, BI -> drop the spurious
 * 0x00), THEN read RBR (reading LSR clears the error bits, so the order is
 * mandatory); drain the FIFO while DR stays set, up to `len`. Returns the number
 * of bytes read (>0), or <0 on a real comms error (NEVER 0).
 */
int UartRxDrain(const UartPortIo *io, UartDriver *drv,
                unsigned char *buf, int len);

/*
 * UartDrainAndClose - End a live session (spec: UartCloses). Drain TX on LSR
 * TEMT (0x40, BOUNDED) so the last bytes leave the shift register, then IER=0
 * and MCR=0 (DTR/RTS/OUT2 all clear) to leave the port quiescent. There is no
 * OS handle to close.
 */
void UartDrainAndClose(const UartPortIo *io, UartDriver *drv);

/* ----------------------------------------------------------------------
 * Real backend - the asm seam + the Transport wiring + the tier gate. Excluded
 * from the host-pure build (it needs windows.h / feat.h / transport.h).
 * ---------------------------------------------------------------------- */

#ifndef UART_HOST_PURE

#include "transport.h"

/*
 * UartTierWantsDirect - THE TIER GATE (invariant 1, SECURITY PIN #1). Returns 1
 * iff g_features.is_win32s (the direct route's sole precondition); 0 on Win9x/NT
 * (which use the OS serial path). Reads g_features, performs NO port I/O, so the
 * dispatch test can flip g_features and assert the decision without faulting on
 * NT. SerialBackendOpen MUST call this as its FIRST line and dispatch to
 * UartBackendOpenDirect only when it returns 1.
 */
int UartTierWantsDirect(void);

/*
 * UartBackendOpenDirect - Open a direct-UART serial Transport (spec: the live
 * route). Maps cfg->port ("COM1".."COM4") to a base port, runs UartOpenSequence
 * over the real inb/outb seam at cfg->baudRate's divisor, and on UART_OPEN_LIVE
 * fills `out` with name="serial", kind=TRANSPORT_SERIAL, io.ptr=&the static
 * UartDriver, and the uart_read/uart_write/uart_close vtable (UartRxDrain /
 * UartTxChunk / UartDrainAndClose). Returns 1 on success; 0 with `err` on a
 * fail-closed open (terminal - no COMM.DRV degrade, invariant 5). Called ONLY
 * from SerialBackendOpen under UartTierWantsDirect().
 */
int UartBackendOpenDirect(const TransportConfig *cfg, Transport *out,
                          char *err, int errSize);

#endif /* UART_HOST_PURE */

#ifdef TEST_BUILD
/*
 * UartResetForTest - Clear the file-static driver so each on-target test starts
 * from a known state. Test-only.
 */
void UartResetForTest(void);

/*
 * UartLastRouteForTest - The route SerialBackendOpen last selected, so the
 * dispatch-gate test (test_serial.c) can pin the tier decision (SECURITY PIN #1)
 * WITHOUT driving a real port: -1 none yet, 0 the OS serial (CreateFileA) route,
 * 1 the Win32s direct-UART route. Under TEST_BUILD the direct branch RECORDS the
 * decision and returns without any port I/O (a ring-3 IN #GPs on the CI/NT host);
 * the live open is the on-target #35 hardware acceptance. Defined in serial.c
 * (where the dispatch branch lives). Test-only.
 */
#define UART_ROUTE_NONE        (-1)
#define UART_ROUTE_OS_SERIAL   0
#define UART_ROUTE_DIRECT_UART 1
int UartLastRouteForTest(void);
#endif

#endif /* UART_H */
