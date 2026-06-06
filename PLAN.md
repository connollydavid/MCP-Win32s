# Implementation Plan: MCP-Win32s

## Phase 1: Foundation — **Complete**
- Test framework, JSON parser, serial init, main loop, CI
- 59 tests passing

## Phase 2: File Operations + Base64 — **Complete**
- `src/base64.c/.h` — base64 encode/decode (integer-only)
- `src/file_ops.c/.h` — file read/write/list/delete (ANSI APIs)
- `src/mcp-w32s.c` — dispatch: echo, read, write, list, delete, exec (stub)
- `tests/test_base64.c` — 14 tests
- `tests/test_file_ops.c` — 10 tests
- `tests/prop.h` — C89 property-based testing framework
- `tests/test_pbt_base64.c` — 4 PBT (4000 trials)
- `specs/mcp-protocol.allium` / `specs/file-ops.allium` — Allium specs
- 87 tests passing, all builds clean, binary FPU/486-free

## Phase 3: Network & Transport (serial + TCP/Winsock) — In Progress

**Goal.** Make the network a first-class peer of the serial port. Replace the `HANDLE`-hardwired protocol I/O with a transport-agnostic byte-pipe interface backed by pluggable, runtime-registered backends; refactor serial onto it; add a TCP backend over Winsock 1.1; add a mock backend that makes response bytes assertable in tests. The same seam admits future backends — UDP / HTTP-3 (QUIC), then exotic message/RDMA transports (e.g. ibverbs-over-Thunderbolt) — without touching the protocol core. Phase 3 is fully self-contained: abstraction + registry + serial refactor + TCP backend + runtime detection + mock backend + specs + tests + CI, all in scope here.

**Why this is its own phase, ahead of command execution.** Today the protocol I/O is hard-wired to a Win32 `HANDLE`: `MainLoop`, `SendReady`, `ProcessCommand`, and `ProcessBuffer`'s handler all call `ReadFile`/`WriteFile` directly (`src/mcp-w32s.c:84,197,213`; handler typedef at `:51`). That works for serial because a COM port *is* a file handle — but a Winsock `SOCKET` is **not** a Win32 file handle on Win32s/Win9x, so `ReadFile`/`WriteFile` cannot drive it (README §449 says exactly this). Phase 4 (command execution) emits the ready message and exec stdout/stderr over the transport, so this abstraction must exist first — otherwise exec ships serial-only and is rewritten later.

### Pre-decisions (non-negotiable)

1. **vtable interface, not tagged dispatch.** A backend is a struct of function pointers; the core knows only the interface. This is what makes the layer agnostic and future-proof.
2. **Network backends are runtime-probed (`LoadLibraryA`/`GetProcAddress`), never statically imported.** `wsock32.dll` is absent on bare Win32s; a static import would stop the binary loading there. Same philosophy as Phase 4's `feat.c`.
3. **TCP server is single-client-sequential.** `listen(s, 1)` → `accept` one client → serve until disconnect → accept the next. Matches the single-threaded, one-exec-at-a-time model. Blocking sockets; no `select` loop.
4. **Framing stays above the transport.** Newline-JSON (`LineBuffer`) lives in the core; any reliable ordered byte backend works unchanged. A message-oriented backend sets a `flags` bit to bypass `LineBuffer`.
5. **Transport config moves to the transport module.** `TransportConfig`, `TRANSPORT_*`, and `ParseCommandLine` move from `serial.{c,h}` to `transport.{c,h}` — they are transport-level, not serial-level.
6. **Serial is the always-available baseline + auto-detect fallback.** Explicit `/SERIAL`/`/TCP` are honored exactly; default (no flag) stays serial COM1 (preserves current behavior + tests). Auto-detect, if requested, probes TCP then falls back to serial (README chain: TCP > serial).
7. **`htons`/`htonl` implemented by hand** (`((x&0xff)<<8)|((x>>8)&0xff)`) — avoids importing the symbols and avoids the banned `bswap` (486+) instruction. Integer-only, i386-safe.
8. **The mock backend is the test seam.** Response-byte assertions (impossible today) become possible; unit tests never open a real port or socket.

### Critical Winsock 1.1 / Win32s quirks to design around

| # | Quirk | Mitigation |
|---|-------|-----------|
| T1 | A `SOCKET` is **not** a Win32 file handle on Win32s/Win9x | TCP backend uses `recv`/`send`, never `ReadFile`/`WriteFile`; the vtable hides the difference |
| T2 | `wsock32.dll` absent on bare Win32s (needs TCP/IP-32 add-on on WfW 3.11) | `LoadLibraryA("wsock32.dll")` + `GetProcAddress`; probe fail ⇒ backend unavailable, fall back to serial |
| T3 | Winsock must be initialized/negotiated | `WSAStartup(MAKEWORD(1,1), &wsaData)`; verify `wsaData.wVersion == 0x0101`; one `WSACleanup` at shutdown |
| T4 | `SOCKET` is `unsigned int`; failure sentinel differs | Check `== INVALID_SOCKET` (not `INVALID_HANDLE_VALUE`); API errors are `SOCKET_ERROR` (-1) |
| T5 | Socket teardown differs from handles | `closesocket()` per socket (not `CloseHandle`); pair the single `WSAStartup` with one `WSACleanup` |
| T6 | Network byte order needed for `bind`/port | Manual `htons`/`htonl` (shift, not `bswap` — 486+ banned) |
| T7 | `recv` returns 0 on orderly close, <0 on error | Treat 0 as peer-closed (advance accept loop); <0 → check `WSAGetLastError`, close conn |
| T8 | `send` may move fewer bytes than requested | `TransportWriteAll` loops until all bytes sent or hard error |
| T9 | Socket errors don't use `GetLastError` | Use `WSAGetLastError()` for socket diagnostics in `errMsg` |
| T10 | **Winsock 1.1 only** — no `ws2_32` | Resolve from `wsock32.dll`; never link/`LoadLibrary` `ws2_32` |
| T11 | Win32s has a low socket-handle ceiling and shared address space | `closesocket` promptly on disconnect; one listener + one conn at a time |
| T12 | `accept` blocks the single thread | Acceptable by design (single-client-sequential); no concurrent work expected while idle |

Sources to cite in code comments: README §447–453 (Win32s socket vs handle, no ws2_32), §1147–1199 (Winsock 1.1 TCP design + runtime detection), MS Docs *Winsock 1.1 reference* (`WSAStartup`, `recv`, `send`).

### Design: vtable interface + backend registry

A backend is a small struct of function pointers (C89 indirect calls — fine on i386; Phase 4's `feat.c` uses the same pattern). The protocol core knows only the interface.

```c
/* transport.h */
typedef struct Transport Transport;

struct Transport {
    const char *name;     /* "serial" | "tcp" | "mock" | ... — surfaced in ready message */
    int kind;             /* TRANSPORT_SERIAL | TRANSPORT_TCP | ... */
    int flags;            /* bit0: message-oriented (bypass LineBuffer); else byte-stream */

    /* Connection vtable. Return: >0 bytes moved, 0 = orderly peer close, <0 = error. */
    int  (*read)(Transport *t, void *buf, int len);
    int  (*write)(Transport *t, const void *buf, int len);
    void (*close)(Transport *t);

    /* Server lifecycle. NULL for point-to-point backends (serial).
     * For listeners (tcp): blocks for a client, returns a *connection* Transport
     * (may be `t` itself reused, or a distinct conn). NULL `accept` => one-shot peer. */
    Transport *(*accept)(Transport *t);

    union { HANDLE handle; unsigned int sock; void *ptr; } io;  /* backend-private */
};

/* Backend registry — enables agnostic auto-detect + future backends */
typedef struct {
    int kind;
    const char *name;
    int  (*probe)(void);                                   /* 1 if usable on this host */
    int  (*open)(const TransportConfig *cfg, Transport *out, char *err, int errSize);
} TransportBackend;

int  TransportOpen(const TransportConfig *cfg, Transport *out, char *err, int errSize);
int  TransportWriteAll(Transport *t, const void *buf, int len);   /* loops on short writes */
const char *TransportName(const Transport *t);
```

**Framing stays above the transport.** Newline-delimited JSON (`LineBuffer`) sits in the core and is fed by whatever bytes a backend's `read` delivers. Reliable, ordered byte transports (serial, TCP, and later QUIC/RDMA) need no change. A genuinely message-oriented exotic backend sets `flags` bit0 so the core treats one message = one command and skips `LineBuffer`. This is the only concession the core makes to non-stream transports — everything else is the backend's problem (reliability, ordering, MTU).

**The main loop becomes transport- and lifecycle-agnostic:**
```c
TransportOpen(&cfg, &listener, err, sizeof err);
for (;;) {
    Transport *conn = listener.accept ? listener.accept(&listener) : &listener;
    SendReady(conn);
    Serve(conn);                       /* read → LineBuffer → ProcessCommand(line, conn) */
    if (conn != &listener) conn->close(conn);
    if (!listener.accept) break;       /* serial: one peer, done */
    /* tcp: loop back to accept the next client (single-client-sequential) */
}
listener.close(&listener);
```

### Backends in scope here

| Backend | File | Mechanism | Availability |
|---------|------|-----------|--------------|
| serial | `src/serial.c` (refactor) | wraps existing `OpenSerialPort` + `ReadFile`/`WriteFile`; `accept = NULL` | All Win32 |
| tcp | `src/tcp.c` (new) | Winsock 1.1 `socket`/`bind`/`listen`/`accept`/`recv`/`send`/`closesocket`; `recv`/`send` (NOT ReadFile) | WfW 3.11 + TCP/IP-32, Win95+ |
| mock | `tests/mock_transport.c` (new) | in-memory buffers; captures written bytes, feeds scripted input | Test-only |

The **mock backend is a testability win**: today `ProcessCommand` tests pass `INVALID_HANDLE_VALUE` and cannot assert response bytes (`tests/test_serial.c:330`). With a mock transport, tests assert the exact JSON written.

### TCP backend (`src/tcp.c`) — Winsock 1.1, runtime-probed

`wsock32.dll` is absent on bare Win32s without TCP/IP-32, so a **static import would prevent the binary from loading there**. Per README §1191, the TCP backend `LoadLibraryA("wsock32.dll")` + `GetProcAddress` for every entry point and stores them in a function-pointer table (same philosophy as `feat.c`). Probe fails ⇒ backend unavailable ⇒ explicit `/TCP` errors cleanly, auto-detect falls back to serial.

- `WSAStartup(MAKEWORD(1,1), &wsaData)`; verify `wsaData.wVersion`.
- `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)` → `SOCKET` (`unsigned int`, `INVALID_SOCKET` on failure — **not** `INVALID_HANDLE_VALUE`).
- `bind` to `INADDR_ANY:htons(port)`; `listen(s, 1)` (backlog 1 — single client).
- `accept` blocks; returns the client `SOCKET` wrapped in a connection `Transport`.
- `recv(conn, buf, len, 0)`: `>0` data, `0` orderly close, `SOCKET_ERROR` (<0) error. `send` likewise.
- `closesocket` per socket; `WSACleanup` at process shutdown.
- **`htons`/`htonl` implemented manually** (`((x&0xff)<<8)|((x>>8)&0xff)`) — avoids pulling the symbols *and* avoids the banned `bswap` instruction. Integer-only, i386-safe.
- Blocking sockets only (no `select` loop needed for one client; single-threaded honored).

### Files

**Create:** `src/transport.{c,h}` (interface + registry + `TransportOpen`/`TransportWriteAll`), `src/tcp.{c,h}` (TCP backend + Winsock fnptr table), `tests/mock_transport.{c,h}`, `tests/test_transport.c` (≥10), `tests/test_tcp.c` (≥6, in-process loopback; proven natively on the Windows host against real Winsock), `specs/transport.allium`.

**Modify:**
- `src/serial.{c,h}` — keep `BuildSerialDCB`/`BuildSerialTimeouts`/`OpenSerialPort`; add `SerialBackendOpen` producing a serial `Transport`. **Move** `TransportConfig`, `TRANSPORT_*`, and `ParseCommandLine` out to `transport.{c,h}` (they are transport-level, not serial-level); `serial.h` includes `transport.h`.
- `src/mcp-w32s.c` — `MainLoop`/`SendReady`/`Serve`/`ProcessCommand`/`ProcessBuffer` handler take `Transport *` instead of `HANDLE`; writes go through `TransportWriteAll`. `main` calls `TransportOpen`, runs the accept loop, drops the "only serial supported" rejection.
- `tests/test_serial.c` — update handler signature to `Transport *`; switch the `ProcessCommand` stub tests to the mock backend and assert real response bytes; fix `ParseCommandLine` include path.
- `CMakeLists.txt` (the single source of truth; `build.sh`/`build.bat` are thin wrappers around the mingw/vc6 presets) — add `transport.c`, `tcp.c` to the link; add `test_transport`, `test_tcp` targets (link `-lwsock32` for the tcp test only). Link main with `-lwsock32` **only if** static-link is chosen; default is runtime-probe, so main does **not** statically import wsock32 (CI assertion below).
- `.github/workflows/build-and-test.yml` — run `test_transport`, `test_tcp` (CI is Ubuntu+Wine; local dev runs the PEs natively on the Windows host via WSL2 interop — Wine is a convenience, not the source of truth). **Import-table assertion:** `objdump -p mcp-w32s.exe | grep -i wsock32` must be empty (TCP is runtime-loaded, so the binary still loads on bare Win32s). FPU/486 grep auto-applies to `transport.o`/`tcp.o`.
- `README.md` — replace the "TCP is Phase 3+ / not yet implemented" notes (§1161, §1191–1194) with the implemented design; document the vtable interface and the backend-registry extension point for future UDP/QUIC/RDMA backends.
- `specs/mcp-protocol.allium` — tend the existing `entity Transport { ready: Boolean }` and `surface SerialPort` into a backend-agnostic model (see below).

### Public APIs

```c
/* transport.h — interface, registry, config (moved here from serial.h) */

#define TRANSPORT_NONE   0
#define TRANSPORT_SERIAL 1
#define TRANSPORT_TCP    2
#define TRANSPORT_PIPE   3        /* reserved — Phase 5+ */
#define TRANSPORT_MOCK   99       /* test-only */

#define TRANSPORT_FLAG_MESSAGE 0x01   /* one message = one command; bypass LineBuffer */

typedef struct {
    int   transport;              /* TRANSPORT_SERIAL | TRANSPORT_TCP | ... */
    char  port[32];               /* "COM1" ... (serial) */
    DWORD baudRate;               /* serial */
    int   tcpPort;                /* TCP listen port */
    char  pipeName[260];          /* reserved */
    int   autodetect;             /* 1 = probe TCP then fall back to serial */
} TransportConfig;

typedef struct Transport Transport;
struct Transport {
    const char *name;             /* "serial" | "tcp" | "mock" — surfaced in ready message */
    int   kind;
    int   flags;                  /* TRANSPORT_FLAG_* */
    int   (*read)(Transport *t, void *buf, int len);        /* >0 / 0=close / <0=error */
    int   (*write)(Transport *t, const void *buf, int len); /* >0 / <0=error */
    void  (*close)(Transport *t);
    Transport *(*accept)(Transport *t);                     /* NULL for point-to-point */
    union { HANDLE handle; unsigned int sock; void *ptr; } io;
};

typedef struct {
    int   kind;
    const char *name;
    int   (*probe)(void);                                              /* 1 if usable here */
    int   (*open)(const TransportConfig *cfg, Transport *out,
                  char *err, int errSize);
} TransportBackend;

int         ParseCommandLine(const char *cmdLine, TransportConfig *cfg);   /* moved from serial */
int         TransportOpen(const TransportConfig *cfg, Transport *out,
                          char *err, int errSize);  /* registry dispatch + fallback */
int         TransportWriteAll(Transport *t, const void *buf, int len);     /* loops short writes */
const char *TransportName(const Transport *t);
int         TransportRegister(const TransportBackend *backend);            /* used by backends */

/* serial.h — backend factory (config/parse now live in transport.h) */
int  SerialBackendOpen(const TransportConfig *cfg, Transport *out, char *err, int errSize);
/* existing BuildSerialDCB / BuildSerialTimeouts / OpenSerialPort / CloseSerialPort retained */

/* tcp.h — Winsock 1.1 backend, runtime-probed */
int  TcpBackendProbe(void);   /* 1 if wsock32 loads + WSAStartup(1,1) succeeds */
int  TcpBackendOpen(const TransportConfig *cfg, Transport *out, char *err, int errSize);
void TcpBackendCleanup(void); /* WSACleanup at process shutdown */
unsigned short McpHtons(unsigned short x);   /* manual; no bswap */
unsigned long  McpHtonl(unsigned long x);

/* tests/mock_transport.h — in-memory backend */
typedef struct {
    Transport t;
    const char *scriptIn;   /* bytes delivered by read(), then 0 (close) */
    int  inPos, inLen;
    char outBuf[MCP_MAX_RESPONSE];   /* bytes captured from write() */
    int  outLen;
    int  shortWrite;        /* if >0, write() returns at most this many bytes/call */
} MockTransport;
void MockTransportInit(MockTransport *m, const char *scriptIn, int scriptLen);
```

### Implementation checklist (the dangerous parts)

**Serial refactor (do first — pure restructure, behavior-preserving):**
1. Move `TransportConfig`, `TRANSPORT_*`, `ParseCommandLine` into `transport.{c,h}`. `serial.h` includes `transport.h`. Update includes in `mcp-w32s.c`, `test_serial.c`.
2. Wrap the existing serial open into `SerialBackendOpen`: fill `Transport` with `name="serial"`, `kind=TRANSPORT_SERIAL`, `flags=0`, `accept=NULL`, `read`/`write` = thin `ReadFile`/`WriteFile` wrappers over `io.handle`, `close` = `CloseSerialPort`.
3. Register the serial backend at startup. Confirm the existing serial behavior is byte-identical (regression test).

**Core dispatch retargeting:**
4. Change `ProcessBuffer`'s handler typedef and `ProcessCommand`/`SendReady`/`MainLoop`/new `Serve` from `HANDLE` to `Transport *`. Replace every `WriteFile(...)` with `TransportWriteAll(t, buf, len)`.
5. Rewrite `main` to: `ParseCommandLine` → `TransportOpen` → accept loop (see below). Delete the "only serial supported" rejection.

**Accept loop (transport- and lifecycle-agnostic):**
```c
if (!TransportOpen(&cfg, &listener, err, sizeof err)) { /* MessageBoxA(err); return 1; */ }
for (;;) {
    Transport *conn = listener.accept ? listener.accept(&listener) : &listener;
    if (conn == NULL) break;                 /* accept error */
    SendReady(conn);
    Serve(conn);                             /* read → LineBuffer → ProcessCommand(line, conn) */
    if (conn != &listener) conn->close(conn);
    if (!listener.accept) break;             /* serial: single peer, done */
}
listener.close(&listener);
TcpBackendCleanup();                          /* no-op unless TCP was used */
```

**TCP backend (`tcp.c`) — strict Winsock 1.1 ordering:**
6. Probe: `LoadLibraryA("wsock32.dll")`; `GetProcAddress` for `WSAStartup,WSACleanup,socket,bind,listen,accept,recv,send,closesocket,WSAGetLastError`; store in a fnptr table. Any NULL ⇒ probe fails (return 0).
7. Open listener: `WSAStartup(MAKEWORD(1,1),&wsa)`; verify `wsa.wVersion==0x0101`; `socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)`; fill `sockaddr_in` with `sin_family=AF_INET`, `sin_port=McpHtons(cfg->tcpPort)`, `sin_addr=INADDR_ANY`; `bind`; `listen(s,1)`. On any failure: `errMsg` via `WSAGetLastError`, `closesocket`, `WSACleanup`, return 0.
8. `accept` method: blocking `accept(listener.io.sock,...)`; on `INVALID_SOCKET` return NULL; else fill a connection `Transport` (`name="tcp"`, `read`=`recv` wrapper, `write`=`send` wrapper, `close`=`closesocket`, `accept`=NULL).
9. `read` wrapper: `n=recv(sock,buf,len,0)`; map `0`→0 (close), `SOCKET_ERROR`→-1, else `n`. `write` wrapper: `send(...)`; `SOCKET_ERROR`→-1.
10. Cleanup: `closesocket` both sockets; `TcpBackendCleanup` calls `WSACleanup` once. Track init so double-cleanup is safe.

**`TransportWriteAll`:** loop `t->write` over the buffer; sum bytes; return total or <0 on hard error. Handles serial short writes and TCP `send` partials (T8).

**`TransportOpen`:** look up backend by `cfg->transport` in the registry; if `autodetect`, try TCP `probe`+`open`, on failure fall back to the serial backend; write a clear `errMsg` if the explicitly-requested backend is unavailable.

**Mock backend (`mock_transport.c`):** `read` drains `scriptIn` then returns 0; `write` appends to `outBuf` (honoring `shortWrite` to exercise `TransportWriteAll`); `accept=NULL`; `close` is idempotent.

### Allium lifecycle (mandatory)

1. `/allium:elicit` — settle the transport domain model (listener vs connection lifecycle, the message-vs-stream `flags` bit, fallback semantics). This also resolves the standing open question in `mcp-protocol.allium` ("Should the ready message include transport metadata?") — yes: the ready message names the active backend.
2. `/allium:tend` — write `specs/transport.allium`; update `mcp-protocol.allium`. `allium check` clean.
3. `/allium:propagate` — derive test obligations (table below is the floor).
4. Implement.
5. `/allium:weed` — zero spec↔code drift before this work is marked done.

`specs/transport.allium` sketch (tend owns final form):
```
entity Transport {
    name: String
    kind: serial | tcp | mock
    role: listener | connection | point_to_point
    message_oriented: Boolean
    status: opening | listening | connected | closed | error
    transitions status {
        opening   -> listening      -- server backends (tcp)
        opening   -> connected      -- point-to-point (serial)
        opening   -> error
        listening -> connected      -- accept() returns a client
        connected -> closed         -- peer disconnect / orderly close
        connected -> listening      -- tcp: client gone, back to accept (single-client-sequential)
        terminal: closed, error
    }
}
rule SerialIsPointToPoint   { ... }   -- serial has no accept; role = point_to_point
rule TcpListensThenAccepts  { ... }   -- tcp: listening -> connected via accept
rule UnavailableBackendFallsBack { ... } -- probe fail + auto-detect => serial
rule ReadyOnConnect         { ... }   -- ready message emitted once per connection
invariant ConnectionCanIO   { for t in Transports: t.status = connected implies t.name.size > 0 }
invariant ClosedIsTerminal  { ... }
```

### Tests (floor; propagate may add)

`tests/test_transport.c` (≥10): registry lookup by kind; `TransportOpen` selects serial by default; explicit unknown kind errors; `TransportWriteAll` loops on short writes (mock returns partial); mock read delivers scripted bytes then 0 (close); `accept == NULL` ⇒ one-shot loop exits; message-oriented flag routes around `LineBuffer`; serial backend `accept` is NULL; name surfaced correctly; double-close is safe.

`tests/test_tcp.c` (≥6, run natively on Windows against real Winsock — not skipped locally; self-skips with a printed reason only where Winsock is genuinely absent, e.g. CI/Wine): probe returns availability honestly; open listener binds a port; `accept` + `recv` round-trips a line over loopback (client = a second socket in the test); `send` delivers a response; orderly close returns 0 from `read`; `htons` matches a known value (e.g. `htons(8932)` byte pattern).

Integration (extend `tests/test_serial.c`): full command → mock transport → assert exact response JSON bytes (now possible).

### Future backends (design intent, NOT implemented here)

The registry + vtable is the extension seam. A new backend implements `{probe, open}` and the connection vtable, then registers — the core is untouched.
- **UDP / HTTP-3 (QUIC):** QUIC gives reliable, ordered byte streams → reuse the stream path and `LineBuffer` unchanged; only the backend differs. Modern-only ⇒ runtime-probed/feature-detected, never statically linked on the Win32s path.
- **Exotic message/RDMA (ibverbs-over-Thunderbolt class):** set `flags` message-oriented bit; one message = one command, bypassing `LineBuffer`. These are uplift backends present only on capable hosts; the Win32s baseline always retains serial.

### Test execution environment (WSL2 + Windows host)

The dev host is **WSL2 on Windows**, so MinGW-built PEs run **natively on the Windows host via WSL interop** (`./test_tcp.exe` executes through real `kernel32`/`wsock32`, no Wine). This is the source of truth for local verification — **Wine is a convenience/fallback, not a requirement.** Consequence: `test_tcp` and the end-to-end TCP path are **proven against real Winsock locally and must not be skipped**; the `wsock32`-probe self-skip exists only for environments that genuinely lack Winsock (e.g. CI's Ubuntu+Wine if its Winsock is unusable). `build.sh test` should detect WSL2-with-interop and run the PEs natively, falling back to Wine only when no Windows host is reachable.

### Build/CI integration

- `CMakeLists.txt` (single source of truth; `build.sh`/`build.bat` wrap the mingw/vc6 presets): add `src/transport.c` + `src/tcp.c` to the main link; add `test_transport` and `test_tcp` targets (link `-lwsock32` for `test_tcp` only). Main does **not** statically import `wsock32` (runtime-probed) — so do **not** add `-lwsock32` to the main link.
- `build.sh test`: prefer **native Windows execution** of the test PEs on WSL2 (run `tests/*.exe` directly via interop); use Wine only as a fallback. `host-pbt` (Phase 4) stays native Linux.
- `.github/workflows/build-and-test.yml` (Ubuntu — no Windows host): runs `test_transport` + `test_tcp` under Wine; `test_tcp` self-skips with a printed reason only if Wine's Winsock is unusable. Existing FPU/486 grep auto-applies to `transport.o`/`tcp.o`. **Import-table assertion:** `objdump -p mcp-w32s.exe | grep -i 'wsock32\|ws2_32'` must be empty.
- Stack-frame watch: `sockaddr_in`/`WSADATA` are small, but keep them off oversized frames; if `__chkstk` appears in `tcp.o`, move buffers to `static`.

### Out of scope for Phase 3 (architectural reasons)

- **Named pipes backend.** Win95+ only, not Win32s; same vtable shape, deferred to Phase 5+ (cross-platform) where it adds value. The registry already reserves `TRANSPORT_PIPE`.
- **Multi-client / `select` concurrency.** Conflicts with the single-threaded, single-exec model. Single-client-sequential is the deliberate design.
- **UDP / HTTP-3 / RDMA backends.** Design seam is provided (registry + `flags`), but implementations are modern-host uplift work, not part of the Win32s baseline. Future phases.
- **TLS / authentication.** No crypto libraries compile on the Win32s target; out of the project's threat model (trusted serial/LAN link).

### Verification (sub-agent acceptance criteria)

1. `./build.sh test` clean (strict flags); `transport.o`/`tcp.o` FPU/486-free.
2. `objdump -p mcp-w32s.exe | grep -i 'wsock32\|ws2_32'` empty — binary still loads on bare Win32s; TCP is runtime-loaded.
3. `test_transport`, `test_tcp`, and refactored `test_serial` all pass **run natively on the Windows host (WSL2 interop)**; `test_tcp` is proven against real Winsock (not skipped). Wine is a fallback only.
4. End-to-end serial path unchanged: existing behavior preserved (regression check).
5. End-to-end TCP, run natively on the Windows host: start `mcp-w32s.exe /TCP:8932` as a Windows process; a Windows-side client (e.g. `powershell.exe` `System.Net.Sockets.TcpClient`, so both ends share Windows loopback) sends `{"cmd":"echo","id":"1","line":"hi"}\n` and receives the echo response; disconnect; the server then accepts a second client (sequential). The in-process loopback in `test_tcp.exe` is the primary automated proof.
6. `specs/transport.allium` `allium check` clean; `/allium:weed` reports zero drift; all six Allium skills exercised per the lifecycle above.
7. Phase 4 exec/ready code, when written, uses `Transport *` — no `HANDLE`-typed I/O in the protocol core.
8. Total tests: 87 + ≥10 (transport) + ≥6 (tcp) + mock-backed `test_serial` response-byte assertions = **≥103 tests**.

## Phase 4: Command Execution — Spec'd (not started)

**Goal:** Replace the `exec` stub with a complete implementation: spawn child processes via `CreateProcessA`, capture stdout/stderr/exit code, return them base64-encoded in the JSON response. Ship a JSON command catalog so MCP clients can discover what's safe to run when `--help` is unavailable, and load it server-side as a whitelist (with bypass flag).

Phase 4 is fully self-contained — argv quoting, timeouts, stdin pass-through, 16-bit detection, catalog enforcement, and the full Allium spec are all in scope here.

**Hooks the existing stub at** `src/mcp-w32s.c:171–174`. Reuses `Base64Encode` (`src/base64.h`) and `BuildJsonResponse` (`src/json_parser.h`).

### Required workflow (Allium lifecycle — order is mandatory)

Phase 4 runs spec-first using the Allium plugin skills (see CLAUDE.md "Specification & Test Workflow"):

1. **`/allium:elicit`** — resolve the open question in `specs/mcp-protocol.allium` ("Should the ready message include transport metadata?") — answer: yes, the extended ready message below carries `codepage`, `version`, `features`. Confirm the exec/catalog/capability domain model before specifying.
2. **`/allium:tend`** — write `specs/process-ops.allium` + `specs/catalog.allium` and update `specs/mcp-protocol.allium`. The spec sketches in this plan are *input*; tend owns the final form. `allium check` must pass on all specs.
3. **`/allium:propagate`** — generate the test-obligation list from the three specs. The test tables in this plan are a floor; propagated obligations may add tests, never remove them. Each test file documents which obligation it covers.
4. **Implement** — `src/*.c` + `tests/*.c`, coding to the specs.
5. **`/allium:distill`** — backfill specs for the pre-existing unspecified modules: `specs/base64.allium`, `specs/json-parser.allium`, `specs/serial.allium`. This is Phases 1–2 spec debt, folded into Phase 4 (no sub-phase).
6. **`/allium:weed`** — audit all specs against implementation. Zero drift is a hard gate for marking Phase 4 Complete.

### theft host-side PBT harness (new in Phase 4)

`vendor/theft` is vendored but unwired. Phase 4 wires it as a **host-native** test layer (Linux `gcc -std=c99`, no MinGW, no Wine) for OS-independent modules. Shipped sources stay C89; only `tests/host/*.c` harness files are C99. Win32-API-dependent code is out of theft's scope.

| File | Properties (autoshrinking, ≥50k trials each) |
|------|----------------------------------------------|
| `tests/host/theft_base64.c` | roundtrip, alphabet validity, length formula — deep version of the prop.h suite |
| `tests/host/theft_json.c` | parse(build(cmd)) == cmd; parser never reads past terminator; malformed input never crashes |
| `tests/host/theft_argv.c` | ArgvEscapeArg/ArgvJoin roundtrip against a reference CommandLineToArgvW tokenizer implemented in the harness |
| `tests/host/theft_catalog.c` | CatalogValidateArgs never accepts unknown flags; glued (`/A:v`) and split (`/A v`) flag-arg forms validate identically |

- `build.sh host-pbt`: builds `vendor/theft` + harness natively, runs it. Same properties mirrored in `prop.h` at lower trial counts for the Wine/target run.
- CI: new `host-pbt` step runs **before** the Wine suite (fail fast on logic bugs with minimal counterexamples).

### Critical Win32 quirks to design around

| # | Quirk | Mitigation |
|---|-------|-----------|
| Q1 | `WaitForSingleObject(hProcess)` returns immediately on Win32s 1.25a (KB Q125213) | Poll `GetExitCodeProcess` until `!= STILL_ACTIVE` |
| Q2 | Pipe deadlock when child fills stdout buffer (default 4KB) and parent isn't reading | Pump pipes inside the wait loop, not after |
| Q3 | `PeekNamedPipe` is the only single-threaded non-blocking pipe read on Win32 | Use it before every `ReadFile` |
| Q4 | Parent must `CloseHandle` on child's pipe ends after spawn or pipes never EOF | Close child ends in parent immediately after `CreateProcessA` returns |
| Q5 | Inherited handles on parent's read/write ends cause child to hang on read | `SetHandleInformation(*, HANDLE_FLAG_INHERIT, 0)` on parent-only ends |
| Q6 | Console output uses **OEM** code page; pipes deliver raw bytes | Always base64; bridge decodes using `codepage` from ready message |
| Q7 | `CreateProcessA` cmdline max 32767; via `cmd.exe` 8192 | Cap at 8192 if `shell:true`, else 32767 |
| Q8 | `CommandLineToArgvW` quoting: `2N\` + `"` ⇒ `N\` + quote toggle; `2N+1\` + `"` ⇒ `N\` + literal `"` | `argv.c` implements reverse-rules; PBT roundtrip |
| Q9 | Win 3.x ships `COMMAND.COM`, NT/95+ ships `cmd.exe`; built-ins live in the shell, not as `.exe` | Pick shell at runtime via `GetVersion`; catalog records both `shell_modern` and `shell_win32s` |
| Q10 | Console flash on GUI Win32s when child is a console app | `STARTF_USESHOWWINDOW` + `SW_HIDE` always |
| Q11 | `CREATE_NO_WINDOW` is Win95+; ignored on Win32s | Use `STARTF_USESHOWWINDOW`+`SW_HIDE`, NOT `CREATE_NO_WINDOW` |
| Q12 | 16-bit DOS apps run in shared VDM — killing one VDM process can kill siblings | On 16-bit + timeout, drain pipes without `TerminateProcess`; mark `vdm-best-effort` |
| Q13 | `GetTickCount` not `QueryPerformanceCounter` (QPC is 95+ and may pull FP libs) | Use `GetTickCount` |
| Q14 | `lpApplicationName=NULL` + full cmdline lets Windows resolve via PATH | Pass `NULL` for app name |
| Q15 | `cmd.exe` metacharacters `& \| < > ^ ( ) %` need `^`-escape inside `cmd /c` | When `shell:true`, `argv.c` does cmd-aware double-escape after CmdLineToArgv-escape |
| Q16 | `GetBinaryTypeA` resolves via path; not on Win32s | `GetProcAddress`-detect; manual MZ/NE/PE classify on Win32s |

Sources to cite in code comments: KB Q125213 (Win32s synchronous spawn), KB 131896 (Win32s general limitations), Old New Thing 2011-07-07 (pipe deadlock), MS Docs *Creating a Child Process with Redirected Input and Output*, Daniel Colascione *Everyone quotes command line arguments the wrong way*.

### Feature detection & graceful uplift

**Principle.** The binary's baseline target is Win32s 1.25a — every required path works there. But when running on NT 4.0+, Win 9x, XP, Win 10+, the binary detects available APIs at startup and *uplifts* to a better implementation. One binary, every Windows era from 1995 to 2026, best behavior the host can provide.

**Mechanism.** A new `src/feat.{c,h}` module probes APIs via `GetProcAddress(GetModuleHandleA("kernel32"))` (and `LoadLibraryA` for psapi/etc.) at startup. Results cached in a `Features` struct. **Function pointers for delay-loaded APIs are stored in the struct** so they're never linked at compile time — that would prevent the binary from loading on Win32s, where most of these symbols are absent from the import resolver.

**Capability matrix:**

| Capability | Detection | Min OS | Used For | Win32s/9x fallback |
|------------|-----------|--------|----------|--------------------|
| Threads | `GetVersion` high bit + Win32s probe (kernel32 thunk pattern) | Win 95 / NT 3.1 | Reader threads in capture loop — eliminates `Sleep(10)` polling latency | `PeekNamedPipe` polling loop (Q3) |
| `CreateJobObjectA` + `AssignProcessToJobObject` + `SetInformationJobObject` | kernel32 GetProcAddress | NT 4.0 | Auto-kill child trees on server exit (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`); per-process memory cap (`JOB_OBJECT_LIMIT_PROCESS_MEMORY`); per-process CPU-time cap (`JOB_OBJECT_LIMIT_PROCESS_TIME`) | Children survive server crash; no resource caps |
| `GetBinaryTypeA` | kernel32 GetProcAddress | NT 3.51 / Win 95 | Refines `binary_type` reporting; preferred over manual MZ/NE/PE classification when present (knows about WoW64) | Manual MZ/NE/PE header read (Q16) |
| `IsWow64Process` | kernel32 GetProcAddress | XP SP2 | Reports `binary_type:"pe32-wow64"` when applicable | Plain `pe32` |
| `GenerateConsoleCtrlEvent` | kernel32 GetProcAddress + child spawned with `CREATE_NEW_PROCESS_GROUP` | NT 4.0 | Graceful Ctrl-Break on timeout, give child 1 sec to clean up before falling through to `TerminateProcess` | Direct `TerminateProcess` (Q12 still applies for VDM) |
| `QueryFullProcessImageNameA` | kernel32 GetProcAddress | Vista | Resolves child's actual exe path (better than `GetModuleFileNameEx` from psapi) | `SearchPathA` from cmdline first token |
| `CreatePseudoConsole`/`ClosePseudoConsole`/`ResizePseudoConsole` | kernel32 GetProcAddress | Win 10 1809 | New optional `ptyExec` command — spawn child with real PTY (interactive stdin, ANSI color, `cols`/`rows` resize); output `output_kind:"ansi"` | `ptyExec` returns `error:"pty not available on this Windows"`; regular `exec` still works |
| `InitializeProcThreadAttributeList` + extended `STARTUPINFOEX` | kernel32 GetProcAddress | Vista | Required by `CreatePseudoConsole`; also enables `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` to reduce inadvertent handle inheritance | Plain `STARTUPINFO`; relies on `SetHandleInformation` (Q5) |
| `SetProcessMitigationPolicy` | kernel32 GetProcAddress | Win 7 (process); Win 8 (thread attr) | Defense-in-depth on spawned child (DEP, ASLR force, dynamic-code-disable) | None |

**`feat.h` shape:**
```c
typedef struct {
    /* OS version */
    int win_major, win_minor, win_build;
    int is_win32s;       /* GetVersion high bit + major==3 */
    int is_win9x;        /* GetVersion high bit + major==4 */
    int is_nt;           /* GetVersion high bit clear */
    int is_wow64;        /* IsWow64Process(GetCurrentProcess()) — defaults 0 */
    /* Boolean capability flags (mirror function-pointer presence) */
    int has_threads;
    int has_create_job_object;
    int has_get_binary_type;
    int has_is_wow64_process;
    int has_generate_ctrl_event;
    int has_query_full_image_name;
    int has_create_pseudo_console;
    int has_proc_thread_attr_list;
    int has_set_process_mitigation;
    /* Function pointers — NULL when capability absent */
    HANDLE  (WINAPI *pCreateJobObjectA)(LPSECURITY_ATTRIBUTES, LPCSTR);
    BOOL    (WINAPI *pAssignProcessToJobObject)(HANDLE, HANDLE);
    BOOL    (WINAPI *pSetInformationJobObject)(HANDLE, int, LPVOID, DWORD);
    BOOL    (WINAPI *pGetBinaryTypeA)(LPCSTR, LPDWORD);
    BOOL    (WINAPI *pIsWow64Process)(HANDLE, PBOOL);
    BOOL    (WINAPI *pGenerateConsoleCtrlEvent)(DWORD, DWORD);
    BOOL    (WINAPI *pQueryFullProcessImageNameA)(HANDLE, DWORD, LPSTR, PDWORD);
    HRESULT (WINAPI *pCreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, void**);
    void    (WINAPI *pClosePseudoConsole)(void*);
    BOOL    (WINAPI *pResizePseudoConsole)(void*, COORD);
    BOOL    (WINAPI *pInitializeProcThreadAttributeList)(LPVOID, DWORD, DWORD, PSIZE_T);
    BOOL    (WINAPI *pUpdateProcThreadAttribute)(LPVOID, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T);
    void    (WINAPI *pDeleteProcThreadAttributeList)(LPVOID);
    BOOL    (WINAPI *pSetProcessMitigationPolicy)(int, PVOID, SIZE_T);
} Features;

extern Features g_features;
void        FeatInit(void);                /* call once at startup, before any exec */
const char *FeatVersionString(void);       /* e.g. "Windows 10.0.19045 (NT)" */
int         FeatForceFallback(int flags);  /* test-only: zero out selected flags + fnptrs */
```

**Win32s probe** (the one tricky detection — Win32s reports `GetVersion` major==3 with high bit set, but so does plain Win 3.x without Win32s, which we can never run on anyway). Defensive secondary probe: try `GetCurrentDirectoryA` (works on Win32s) and `CreateThread` with a NOOP routine; if `CreateThread` returns NULL with `GetLastError()==ERROR_NOT_SUPPORTED` or similar, set `is_win32s=1` regardless of version DWORD. This double-check catches both Win32s and any future system where threads are explicitly disabled.

**OS-detection sequence in `FeatInit`:**
```
1. ver = GetVersion();
2. is_nt = !(ver & 0x80000000);
3. major = LOBYTE(LOWORD(ver)); minor = HIBYTE(LOWORD(ver));
4. build = is_nt ? HIWORD(ver) : 0;
5. if (!is_nt && major == 4) is_win9x = 1;
6. if (!is_nt && major == 3) is_win32s = 1;          /* presumptive */
7. Probe CreateThread; if it fails outright, is_win32s = 1, has_threads = 0.
8. For each delay-loaded API, GetProcAddress; populate p* fields and has_* flags.
9. If has_is_wow64_process, call IsWow64Process(GetCurrentProcess(), &is_wow64).
```

**Where uplifts apply** (cross-cuts the rest of Phase 4):

1. **Capture loop in `exec_ops.c`** — branch on `g_features.has_threads`:
   - **Threaded path (Win 9x / NT+):** spawn one reader thread per stdout/stderr pipe. Threads loop `ReadFile` into a shared buffer guarded by a `CRITICAL_SECTION`; main thread `WaitForSingleObject(hProc, timeoutMs)` (which works correctly outside Win32s — Q1 only affects Win32s). Threads exit naturally when their pipe EOFs after child exit. Far lower latency for chatty children.
   - **Polling path (Win32s):** the `PeekNamedPipe` loop already specified.

2. **Process containment in `exec_ops.c`** — when `g_features.has_create_job_object`:
   - Spawn child with `CREATE_SUSPENDED`.
   - Create job object; set `JOBOBJECT_EXTENDED_LIMIT_INFORMATION` with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (always) plus `JOB_OBJECT_LIMIT_PROCESS_MEMORY` if request specifies `mem_cap_bytes`, plus `JOB_OBJECT_LIMIT_PROCESS_TIME` if request specifies `cpu_time_ms`.
   - `AssignProcessToJobObject(hJob, hProc)` before resuming.
   - `ResumeThread(hThread)`. Children now die automatically if mcp-w32s.exe crashes.
   - Skip on Win32s/Win9x — children survive but no resource caps.

3. **Graceful termination in `exec_ops.c`** — when `g_features.has_generate_ctrl_event` AND child was spawned with `CREATE_NEW_PROCESS_GROUP`:
   - On timeout: send `CTRL_BREAK_EVENT` to child's process group. Wait up to 1000ms for clean exit (loop `GetExitCodeProcess`). If still alive, `TerminateProcess(hProc, 1)`.
   - Win32s/Win9x path: direct `TerminateProcess` (Q12 still applies for VDM/16-bit).

4. **Binary classification in `binfmt.c`** — when `g_features.has_get_binary_type`, prefer it (knows about WoW64). When `g_features.has_is_wow64_process`, refine `BIN_PE32` → emit `binary_type:"pe32-wow64"` if true. Otherwise manual MZ/NE/PE classification (Q16).

5. **Image path resolution** — when `g_features.has_query_full_image_name`, prefer it post-spawn for `binary_type` accuracy; otherwise `SearchPathA` on the first token.

6. **PTY (`src/pty_exec.{c,h}`, new optional `ptyExec` JSON command)** — gated on `g_features.has_create_pseudo_console`. Implementation:
   - Two pipes (input, output — PTY merges stdout/stderr by design).
   - `CreatePseudoConsole({cols, rows}, hInputRd, hOutputWr, 0, &hPC)`.
   - `InitializeProcThreadAttributeList` + `UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC)`.
   - `CreateProcessA` with `EXTENDED_STARTUPINFO_PRESENT` + `STARTUPINFOEX`.
   - Output retains ANSI escape sequences. Response carries `output_kind:"ansi"` and a single `output_b64` (no separate stdout/stderr).
   - When capability absent, dispatcher returns `error:"pty not available on this Windows"`; regular `exec` is unaffected.

7. **Ready message extension** — emitted by `mcp-w32s.c` after `FeatInit`:
   ```json
   {
     "status":   "ready",
     "codepage": 437,
     "version":  "Windows 10.0.19045 (NT)",
     "transport": "tcp",
     "features": {
       "is_win32s": false,
       "is_win9x":  false,
       "is_nt":     true,
       "is_wow64":  false,
       "threads":   true,
       "job_objects": true,
       "ctrl_events": true,
       "pty":       true,
       "binary_classify": "GetBinaryTypeA",
       "process_mitigation": false
     }
   }
   ```
   Bridge consumes `features` to surface capability-aware UI to MCP clients.

**The uplift architecture is intentionally additive:** every new code path has a `if (g_features.has_X)` gate with a Win32s-correct fallback. Removing `feat.c` entirely would leave a working binary that runs at the lowest common denominator. Adding `feat.c` lets us claim Win 11 features without breaking Win 3.1 + Win32s 1.25a.

### Pre-decisions (non-negotiable)

1. **`argv` and `line` both supported, `argv` preferred.** Legacy `line` stays for back-compat. If both present, `argv` wins.
2. **stdout/stderr always base64.** No encoding interpretation in the binary.
3. **Single concurrent exec.** Second exec while one runs returns `error:"busy"`.
4. **Catalog loaded at startup; whitelist on by default.** `/UNSAFE` cmdline flag disables; per-request `unsafe:true` bypasses for one exec.
5. **16-bit detected.** `GetBinaryTypeA` when present, manual MZ/NE/PE classification on Win32s. Best-effort exec for 16-bit, no `TerminateProcess` on timeout.
6. **`GetExitCodeProcess` polling on Win32s; `WaitForSingleObject(hProc)` on threaded path.** Branch on `g_features.has_threads`. The polling path is the Win32s baseline; uplift uses the better primitive.
7. **`MCP_MAX_RESPONSE` bumps 128KB → 256KB** to fit two base64-encoded 64KB streams + envelope.
8. **Feature detection at startup, not compile-time.** All version-specific APIs are `GetProcAddress`-loaded into the `g_features` struct. The binary itself imports only Win32s 1.25a symbols. No `#ifdef _WIN32_WINNT` guards in source — branching is runtime via `g_features.has_*`.
9. **One PTY uplift command, gated on Win 10 1809+.** New `ptyExec` JSON command is the only protocol surface for interactive/ANSI exec; absent capability returns explicit error. Regular `exec` is unaffected on every Windows version.
10. **Job-object containment is opt-on-by-default when available.** Every spawned child is assigned to a job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` on NT 4.0+. No request flag needed; this is server hygiene, not a per-request choice. Per-request `mem_cap_bytes` and `cpu_time_ms` extend the job's limits.

### Protocol extension

**`exec` request:**
```json
{
  "cmd":         "exec",
  "id":          "e1",
  "argv":        ["cl","/c","test.c"],
  "line":        "cl /c test.c",
  "cwd":         "C:\\PROJECTS",
  "shell":       false,
  "timeout_ms":  30000,
  "stdin_b64":   "",
  "max_output":  65536,
  "unsafe":      false,
  "mem_cap_bytes": 0,                  // 0 = no cap; only honored when has_create_job_object
  "cpu_time_ms":   0                   // 0 = no cap; only honored when has_create_job_object
}
```

**`exec` response (success):**
```json
{
  "id":             "e1",
  "status":         "ok",
  "exit_code":      0,
  "stdout_b64":     "aGVsbG8NCg==",
  "stderr_b64":     "",
  "stdout_truncated": false,
  "stderr_truncated": false,
  "duration_ms":    47,
  "exec_method":    "direct",
  "binary_type":    "pe32",
  "killed_by":      ""                 // "" | "timeout" | "ctrl_break" | "memory_cap" | "cpu_cap"
}
```

**`ptyExec` request** (only available when `g_features.has_create_pseudo_console`):
```json
{
  "cmd":         "ptyExec",
  "id":          "p1",
  "argv":        ["cmd"],
  "cwd":         "C:\\",
  "timeout_ms":  60000,
  "stdin_b64":   "",
  "cols":        80,
  "rows":        25,
  "max_output":  65536,
  "unsafe":      false
}
```

**`ptyExec` response (success):**
```json
{
  "id":            "p1",
  "status":        "ok",
  "exit_code":     0,
  "output_b64":    "...",              // single merged stream — PTY does not separate stdout/stderr
  "output_kind":   "ansi",             // contains ANSI escape sequences; bridge interprets/strips
  "output_truncated": false,
  "duration_ms":   123
}
```

**Error reasons** (both commands): `"spawn failed: <code>"`, `"timed out after <N>ms"`, `"busy"`, `"command line too long"`, `"invalid argv"`, `"invalid base64"`, `"command not in catalog"`, `"argument not allowed"`, `"pty not available on this Windows"` (ptyExec only), `"job object setup failed"` (only when has_create_job_object and assignment fails).

`exec_method` ∈ {`direct`, `shell`, `vdm-best-effort`}. `binary_type` ∈ {`pe32`, `pe32-wow64`, `ne16`, `mz`, `unknown`, `shell-builtin`}. `killed_by` ∈ {`""`, `"timeout"`, `"ctrl_break"`, `"memory_cap"`, `"cpu_cap"`}.

README §1554 (current protocol doc using `output` key) is updated to `stdout_b64`/`stderr_b64` as part of Phase 4, and a new section documents `ptyExec`.

### Files to create

| Path | Purpose |
|------|---------|
| `src/feat.{c,h}` | OS detection + `GetProcAddress` probing for capability uplift; `g_features` global; `FeatInit`, `FeatVersionString`, `FeatForceFallback` (test-only) |
| `src/exec_ops.{c,h}` | Pipe + spawn + capture loop with capability-gated branches (threaded vs polling, job-object containment, ctrl-event termination); `ExecOpRun`, `ExecResult` |
| `src/pty_exec.{c,h}` | `CreatePseudoConsole`-based exec for `ptyExec` command; `PtyExecRun`; only operative when `g_features.has_create_pseudo_console` |
| `src/argv.{c,h}` | argv array → CreateProcess command line (CommandLineToArgvW reverse-rules + cmd metachar escape) |
| `src/binfmt.{c,h}` | MZ/NE/PE classifier; uses `g_features.pGetBinaryTypeA`/`pIsWow64Process` when available |
| `src/catalog.{c,h}` | JSON catalog loader (reuses `json_parser.c`); whitelist + arg-validation |
| `src/ready.{c,h}` | Builds the extended JSON ready message including `version`, `codepage`, `features` object |
| `tests/test_feat.c` | ≥6 tests: probe results, version-string format, fallback consistency, mock-zeroed struct exec path |
| `tests/test_exec_ops.c` | ≥22 unit tests (18 baseline + 4 capability fallbacks via `FeatForceFallback`) |
| `tests/test_pty_exec.c` | ≥4 tests: PTY spawn (skip if absent), echo round-trip, resize, capability-absent error |
| `tests/test_argv.c` | 12 fixed + PBT 1000 trials for argv quoting roundtrip |
| `tests/test_binfmt.c` | 6 tests against fixture binaries (MZ, NE, PE32); plus uplift test using `pGetBinaryTypeA` when present |
| `tests/test_catalog.c` | 8 tests for load + lookup + validation |
| `tests/argv_echo.c` | Helper: prints argc + each argv[i] base64-encoded for PBT roundtrip |
| `tests/fixtures/{tiny_mz.exe,tiny_ne.exe}` | Minimal binary headers for `binfmt` classification tests |
| `specs/process-ops.allium` | `Process` + `ExecResult` + `Capabilities` entities, 8+ rules (incl. capability-gated rules), 3+ invariants — written via `/allium:tend` |
| `specs/catalog.allium` | `Catalog` + `CatalogEntry` entities, lookup/validate rules — written via `/allium:tend` |
| `specs/base64.allium` | Distilled from `src/base64.c` via `/allium:distill` (Phase 1–2 spec debt) |
| `specs/json-parser.allium` | Distilled from `src/json_parser.c` via `/allium:distill` |
| `specs/serial.allium` | Distilled from `src/serial.c` via `/allium:distill` |
| `tests/host/theft_base64.c` | theft host-native PBT: base64 properties with autoshrinking |
| `tests/host/theft_json.c` | theft host-native PBT: JSON parser robustness + roundtrip |
| `tests/host/theft_argv.c` | theft host-native PBT: argv quoting vs reference tokenizer |
| `tests/host/theft_catalog.c` | theft host-native PBT: catalog validation properties |
| `catalog/win32-commands.json` | ≥30 entries (built-ins + externals + build tools) |
| `catalog/README.md` | How to extend the catalog |

### Files to modify

| Path | Change |
|------|--------|
| `src/common.h` | `JsonCommand` adds `argv_count`, `argv[MCP_MAX_ARGV][MCP_MAX_ARG_LEN]`, `cwd`, `timeout_ms`, `shell_flag`, `stdin_b64`, `max_output`, `unsafe_flag`, `mem_cap_bytes`, `cpu_time_ms`, `cols`, `rows`. Constants: `MCP_MAX_ARGV=64`, `MCP_MAX_ARG_LEN=512`. Bump `MCP_MAX_RESPONSE` to `262144`. |
| `src/json_parser.{c,h}` | Parse new fields. Array parsing for `argv`. Number parsing for ints. Boolean for `shell`/`unsafe`. |
| `src/mcp-w32s.c` | Call `FeatInit()` first thing in `main` (before transport open). Replace stub at lines 171–174 with: catalog lookup → argv build → `ExecOpRun` → response. Add `ptyExec` dispatch (returns capability-error when absent). Track `g_exec_busy` flag. Load catalog at startup; honor `/UNSAFE` cmdline. Send extended ready message via `BuildReadyMessage` from `ready.c`. **Builds on the transport abstraction (foundational work above): all dispatch/response I/O is via `Transport *`, never `HANDLE`.** |
| `src/serial.{c,h}` | Parse `/UNSAFE` and `/CATALOG:path` cmdline flags into `TransportConfig`. |
| `specs/mcp-protocol.allium` | Replace `rule ExecCommand` (lines 211–221) with rule that delegates to `process-ops.ExecResult` and gates on `CatalogLookup`. Add `rule PtyExecCommand` (gated on `Capabilities.has_pty`). Remove `deferred ExecCommand.implementation` (line 244). Add `Capabilities` reference. |
| `CMakeLists.txt` (single source of truth; `build.sh`/`build.bat` wrap the mingw/vc6 presets) | Add seven new `.c` files (`feat`, `exec_ops`, `pty_exec`, `argv`, `binfmt`, `catalog`, `ready`). Add six test targets + `argv_echo` helper. Copy `catalog/win32-commands.json` next to test binaries. |
| `.github/workflows/build-and-test.yml` | Run new test binaries under Wine: `test_feat`, `test_exec_ops`, `test_pty_exec`, `test_argv`, `test_binfmt`, `test_catalog`. Add catalog file to artifact upload. **Verify uplift on Wine:** Wine reports as NT — assert `is_nt=true` and `has_threads=true` in `test_feat.exe` output, but skip `test_pty_exec` if Wine version doesn't expose `CreatePseudoConsole` (probe-and-skip pattern). |
| `README.md` | §1554: protocol shape (`stdout_b64`/`stderr_b64`); Implementation Phases: Phase 4 → Complete; new "Command Execution: Win32s caveats" referencing Q1, Q9, Q12; new "Command Catalog" section; new "Feature Detection & Graceful Uplift" section with capability matrix; new "PTY Execution (`ptyExec`)" section. |
| `CLAUDE.md` | Phase 4 → Complete; bump test count to ≥152; document `g_features` global and the runtime-detection convention (no `#ifdef _WIN32_WINNT`). |

### Public APIs

```c
/* exec_ops.h */
typedef struct {
    int  exit_code;          /* 0 = success; -1 if spawn failed */
    int  duration_ms;
    int  stdout_len;
    int  stderr_len;
    int  stdout_truncated;
    int  stderr_truncated;
    int  timed_out;
    int  binary_type;        /* see binfmt.h */
} ExecResult;

int ExecOpRun(
    const char *cmdLine,
    const char *cwd,                    /* NULL = inherit */
    int  timeoutMs,                     /* 0 = no timeout */
    int  hideWindow,
    const unsigned char *stdinBytes,    /* NULL ok */
    int  stdinLen,
    unsigned char *stdoutBuf, int stdoutBufSize,
    unsigned char *stderrBuf, int stderrBufSize,
    ExecResult *result,
    char *errMsg, int errSize
);

/* argv.h */
int ArgvEscapeArg(const char *arg, char *out, int outSize);
int ArgvJoin(const char **argv, int argc, char *out, int outSize);
int ArgvCmdEscape(const char *line, char *out, int outSize);

/* binfmt.h */
typedef enum {
    BIN_UNKNOWN = 0, BIN_PE32 = 1, BIN_NE16 = 2, BIN_MZ = 3, BIN_SHELL = 4
} BinaryType;
int BinFmtClassify(const char *exePath, BinaryType *outType, char *errMsg, int errSize);

/* catalog.h */
typedef struct CatalogEntry CatalogEntry;
typedef struct Catalog      Catalog;

int  CatalogLoad(const char *path, Catalog **outCat, char *errMsg, int errSize);
void CatalogFree(Catalog *cat);
const CatalogEntry *CatalogLookup(const Catalog *cat, const char *cmdName);
int  CatalogValidateArgs(const CatalogEntry *entry, const char **argv, int argc, char *errMsg, int errSize);

/* feat.h — see "Feature detection & graceful uplift" above for full Features struct */
extern Features g_features;
void        FeatInit(void);
const char *FeatVersionString(void);
int         FeatForceFallback(int flagsMask);   /* test-only: zero out selected flags + fnptrs */

/* pty_exec.h */
typedef struct {
    int  exit_code;
    int  duration_ms;
    int  output_len;
    int  output_truncated;
    int  timed_out;
} PtyExecResult;

int PtyExecRun(
    const char *cmdLine,
    const char *cwd,
    int  cols, int rows,
    int  timeoutMs,
    const unsigned char *stdinBytes, int stdinLen,
    unsigned char *outputBuf, int outputBufSize,
    PtyExecResult *result,
    char *errMsg, int errSize
);

/* ready.h */
int BuildReadyMessage(char *json, int jsonSize);   /* uses g_features + GetACP() */
```

`ExecOpRun` extended signature (over the baseline above) takes `int memCapBytes` and `int cpuTimeMs` for job-object limits — silently ignored on non-NT-4-or-later. Plus `int *killedBy` out-param: 0 normal, 1 timeout, 2 ctrl_break, 3 memory_cap, 4 cpu_cap. Implementations that elide these args may pass 0/NULL — they no-op on Win32s/9x.

### Implementation checklist

**Startup (`mcp-w32s.c main`):**
1. `FeatInit()` first — populates `g_features`. Must complete before any spawn / catalog load / ready message.
2. Parse cmdline (`/SERIAL`, `/UNSAFE`, `/CATALOG:path`, etc.).
3. Catalog load (`CatalogLoad`); on failure record `warning:"catalog not loaded"` for ready message.
4. Open transport.
5. `BuildReadyMessage` + `WriteFile` — extended ready message with `version`/`codepage`/`features`.
6. Enter main loop.

**Pipe + spawn (common to `exec_ops.c` and `pty_exec.c`):**
1. Three pipes (stdin, stdout, stderr) — or two (input, output) for PTY. `SECURITY_ATTRIBUTES.bInheritHandle=TRUE`. `SetHandleInformation` on parent-only ends with `HANDLE_FLAG_INHERIT=0` (Q5).
2. `STARTUPINFO`: `dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW`, `wShowWindow = SW_HIDE` (Q10/Q11). For PTY path use `STARTUPINFOEX` with `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`.
3. `CreateProcessA`:
   - `bInheritHandles=TRUE`, `lpApplicationName=NULL` (Q14), full cmdline in `lpCommandLine`.
   - `dwCreationFlags = 0` baseline; OR `CREATE_NEW_PROCESS_GROUP` if `g_features.has_generate_ctrl_event` (enables `GenerateConsoleCtrlEvent` later); OR `EXTENDED_STARTUPINFO_PRESENT` for PTY path.
   - **NOT** `CREATE_NO_WINDOW` (Q11).
   - If `g_features.has_create_job_object`, also OR in `CREATE_SUSPENDED` so we can assign to job before the child's first instruction runs.
4. **Job-object setup (when `g_features.has_create_job_object`):**
   - `g_features.pCreateJobObjectA(NULL, NULL)` → `hJob`.
   - Build `JOBOBJECT_EXTENDED_LIMIT_INFORMATION`: always set `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. If `memCapBytes>0`, set `JOB_OBJECT_LIMIT_PROCESS_MEMORY` + `ProcessMemoryLimit`. If `cpuTimeMs>0`, set `JOB_OBJECT_LIMIT_PROCESS_TIME` + `PerProcessUserTimeLimit` (in 100ns ticks: `cpuTimeMs * 10000`).
   - `pSetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits))`.
   - `pAssignProcessToJobObject(hJob, pi.hProcess)`.
   - `ResumeThread(pi.hThread)`.
   - On any failure: `TerminateProcess` + `CloseHandle(hJob)` + return `"job object setup failed"`. Don't silently degrade — the user explicitly opted in via `mem_cap_bytes`/`cpu_time_ms`.
5. After spawn (and after job assignment if applicable): close child ends in parent (Q4). Write stdin if any, then close `parentInWr`.

**Capture loop — Win32s/Win9x polling path** (`g_features.has_threads == 0`):
```
start = GetTickCount();
loop {
    PumpPipe(parentOutRd, stdoutBuf, &stdoutLen, stdoutBufSize, &stdoutTruncated);
    PumpPipe(parentErrRd, stderrBuf, &stderrLen, stderrBufSize, &stderrTruncated);
    GetExitCodeProcess(hProc, &exitCode);
    if (exitCode != STILL_ACTIVE) break;        /* Q1 */
    if (timeoutMs > 0 && GetTickCount() - start >= timeoutMs) {
        TimeoutTerminate(hProc, binaryType, &killedBy);   /* see below */
        break;
    }
    Sleep(10);
}
PumpPipe(parentOutRd, ...);  /* final drain */
PumpPipe(parentErrRd, ...);
GetExitCodeProcess(hProc, &exitCode);
```

**Capture loop — threaded path** (`g_features.has_threads == 1`):
```
start = GetTickCount();
hOutThread = CreateThread(NULL, 0, ReaderThread, &outCtx, 0, NULL);
hErrThread = CreateThread(NULL, 0, ReaderThread, &errCtx, 0, NULL);
HANDLE waitObj[1] = { hProc };
DWORD wait = WaitForSingleObject(hProc, timeoutMs > 0 ? timeoutMs : INFINITE);
if (wait == WAIT_TIMEOUT) {
    TimeoutTerminate(hProc, binaryType, &killedBy);
    WaitForSingleObject(hProc, INFINITE);   /* await actual exit */
}
WaitForSingleObject(hOutThread, INFINITE);
WaitForSingleObject(hErrThread, INFINITE);
GetExitCodeProcess(hProc, &exitCode);
```
`ReaderThread`: loops `ReadFile(pipe, buf, ...)` until 0 bytes or error; appends to caller's buffer guarded by a `CRITICAL_SECTION`. Marks `truncated=1` if buffer fills.

**`TimeoutTerminate(hProc, binaryType, &killedBy)`:**
```
if (binaryType == BIN_NE16 || binaryType == BIN_MZ) {
    /* Q12: do not kill VDM */
    killedBy = 1;  /* "timeout" — but TerminateProcess skipped */
    return;
}
if (g_features.has_generate_ctrl_event) {
    g_features.pGenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, processGroupId);
    /* Wait up to 1 sec for graceful exit */
    if (WaitForSingleObject(hProc, 1000) == WAIT_OBJECT_0) {
        killedBy = 2;  /* "ctrl_break" */
        return;
    }
}
TerminateProcess(hProc, 1);
killedBy = 1;  /* "timeout" */
```

When job-object containment kills the child via `JOB_OBJECT_LIMIT_PROCESS_MEMORY` or `JOB_OBJECT_LIMIT_PROCESS_TIME`, `GetExitCodeProcess` returns the special exit codes set by the kernel (memory cap: per Win32 docs the child gets terminated and exit code is 1 or implementation-defined; CPU cap: similar). Detect by also calling `pQueryInformationJobObject(hJob, JobObjectBasicAccountingInformation, ...)` to read termination flag; or simpler: track via `JOBOBJECT_ASSOCIATE_COMPLETION_PORT` (NT 4.0+) — use a completion port on the job and read messages to know precisely which limit killed the child. Set `killedBy=3` (memory_cap) or `4` (cpu_cap) accordingly.

**`PumpPipe` (polling path only):** `PeekNamedPipe` → `dwAvail`. If 0 return. Else `ReadFile` for `min(dwAvail, bufRemaining)`. If `bufRemaining==0` and `dwAvail>0`, set `truncated=1`.

**PTY path (`pty_exec.c`):** when `g_features.has_create_pseudo_console`:
1. Create input + output pipes.
2. `g_features.pCreatePseudoConsole({cols,rows}, hInputRd, hOutputWr, 0, &hPC)`.
3. Allocate `STARTUPINFOEX`; size attribute list via `pInitializeProcThreadAttributeList(NULL, 1, 0, &size)`; allocate; init.
4. `pUpdateProcThreadAttribute(attrList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(hPC), NULL, NULL)`.
5. `CreateProcessA` with `EXTENDED_STARTUPINFO_PRESENT`.
6. Capture via threaded reader (PTY only meaningful on systems with `has_threads`, which is implied by Win 10).
7. Cleanup: `pClosePseudoConsole(hPC)`; `pDeleteProcThreadAttributeList(attrList)`; close handles.

**Cleanup (universal):** `CloseHandle(hProc)`, `CloseHandle(hThread)`, remaining pipe ends, job handle if any, attr list if any — always, including every early-return path.

### `argv.c` quoting algorithm

For each arg (Q8):
- Empty → `""`.
- No `[ \t\n\v"]` → emit verbatim.
- Else: emit `"`; for each char with backslash run accumulation:
  - On `\`: increment count.
  - On `"`: emit `2N+1` backslashes, then `\"`. Reset count.
  - On other: emit `N` backslashes, then char. Reset count.
  - At end: emit `2N` backslashes (closing quote unescaped). Emit `"`.

Join with single spaces. When `shell=true`, run `ArgvCmdEscape` after to `^`-escape cmd metacharacters outside double-quoted regions (Q15).

PBT property: random argv (printable + space + tab + `\` + `"` + control, length 0–32, count 1–8) → `argvJoin → CreateProcessA → tests/argv_echo.exe → child argv[i]` matches input byte-for-byte.

### `binfmt.c` classification

Resolve via `SearchPathA` (Q16). Read first 512 bytes:
- `MZ` magic at 0 + valid `e_lfanew` → check magic at `e_lfanew`: `PE\0\0` = `BIN_PE32`, `NE` = `BIN_NE16`, else `BIN_MZ`.
- `MZ` magic + invalid `e_lfanew` → `BIN_MZ`.
- No `MZ` → `BIN_UNKNOWN`.
- Shell built-in (no resolvable .exe) → `BIN_SHELL`.

`GetBinaryTypeA` via `GetProcAddress(GetModuleHandleA("kernel32"), "GetBinaryTypeA")` on NT/95+; prefer its result when present (knows about WoW64).

### Catalog (`catalog/win32-commands.json`)

```json
{
  "version": 1,
  "commands": {
    "dir": {
      "kind":            "shell-builtin",
      "shell_modern":    "cmd.exe /c dir",
      "shell_win32s":    "command.com /c dir",
      "supports_win32s": true,
      "description":     "List directory contents.",
      "options": [
        {"flag":"/A","arg":"attrs","desc":"Filter by attribute (D R H S A; negate with -)."},
        {"flag":"/B","desc":"Bare format (filenames only)."},
        {"flag":"/S","desc":"Recurse into subdirectories."},
        {"flag":"/O","arg":"order","desc":"Sort by N E S D; negate with -."}
      ],
      "positional": [{"name":"path","optional":true,"type":"path"}],
      "examples": ["dir","dir /B","dir C:\\PROJECTS /S /B"]
    }
  }
}
```

**Validation** (`CatalogValidateArgs`):
- Flags must appear in `options` list (case-insensitive on flag name).
- Unknown flags → `"argument not allowed"`.
- Flag-with-arg consumes next token if it doesn't start with `/` or `-`, OR accepts glued `/A:value`.
- Positional `type` is advisory only (no path-validity check).

**Initial entries (≥30):**
- *Shell built-ins (13):* `dir`, `copy`, `del`, `type`, `echo`, `cd`, `mkdir`, `rmdir`, `ren`, `set`, `path`, `ver`, `cls`
- *Externals (5):* `attrib`, `xcopy`, `find`, `more`, `sort`
- *Build tools (10):* `cl`, `link`, `lib`, `nmake`, `ml`, `rc`, `mt`, `mc`, `gcc`, `make`
- *Diagnostics (2):* `mem`, `chkdsk`

Each: `description`, `options` (descriptions sourced from official MS docs), `positional`, ≥2 `examples`, `supports_win32s`.

**Server-side enforcement:**
- Default load location: `catalog/win32-commands.json` next to `mcp-w32s.exe`. Missing → ready message includes `"warning":"catalog not loaded"`.
- `/CATALOG:<path>` cmdline overrides location.
- `/UNSAFE` disables whitelist (catalog still consulted for `binary_type`).
- Per-request `unsafe:true` bypasses whitelist for that exec; logged in stderr buffer.

### Allium specs

`specs/process-ops.allium`:
```
entity Capabilities {
    is_win32s, is_win9x, is_nt, is_wow64: Boolean
    has_threads, has_job_objects, has_ctrl_events: Boolean
    has_pty, has_get_binary_type, has_query_full_image_name: Boolean
}

entity Process {
    cmd_line: String
    cwd: String
    binary_type: pe32 | pe32_wow64 | ne16 | mz | shell | unknown
    capabilities: Capabilities
    status: not_started | running | exited | timed_out | spawn_failed
    killed_by: none | timeout | ctrl_break | memory_cap | cpu_cap
    transitions status {
        not_started -> running
        not_started -> spawn_failed
        running     -> exited
        running     -> timed_out
        terminal: exited, timed_out, spawn_failed
    }
}

entity ExecResult {
    process: Process
    exit_code: Integer
    stdout_b64, stderr_b64: String
    stdout_truncated, stderr_truncated: Boolean
    duration_ms: Integer
    request_id: String
    status: ok | error
    error_reason: String when status = error
    transitions status { terminal: ok, error }
}

entity PtyExecResult {
    process: Process
    exit_code: Integer
    output_b64: String
    output_kind: ansi | text
    output_truncated: Boolean
    duration_ms: Integer
    request_id: String
    status: ok | error
    error_reason: String when status = error
    transitions status { terminal: ok, error }
}

rule ExecSpawnSuccess { ... }
rule ExecSpawnFailed  { ... }
rule ExecCompleted    { ... }
rule ExecTimedOut     { ... }
rule ExecOutputTruncated { ... }
rule ExecBusyRejected { ... }
rule ExecCtrlBreakKilled    { ... }   -- requires capabilities.has_ctrl_events
rule ExecMemoryCapKilled    { ... }   -- requires capabilities.has_job_objects
rule ExecCpuCapKilled       { ... }   -- requires capabilities.has_job_objects
rule PtyExecSpawnSuccess    { ... }   -- requires capabilities.has_pty
rule PtyExecCapabilityAbsent { ... }  -- emits "pty not available on this Windows" when !capabilities.has_pty

invariant ExitCodeOnSuccess { ... }
invariant TimedOutHasReason { ... }
invariant PtyOnlyWhenCapable { for r in PtyExecResults: r.status = ok implies r.process.capabilities.has_pty = true }
invariant Win32sNoJobObjects { for p in Processes: p.capabilities.is_win32s = true implies p.capabilities.has_job_objects = false }
```

`specs/catalog.allium`:
```
entity Catalog { path: String; loaded: Boolean; entry_count: Integer }
entity CatalogEntry { name: String; kind: shell_builtin | external; supports_win32s: Boolean }
rule CatalogLookupHit  { ... }
rule CatalogLookupMiss { ... }     -- emits "command not in catalog"
rule CatalogArgValid   { ... }
rule CatalogArgInvalid { ... }     -- emits "argument not allowed"
invariant LoadedCatalogHasEntries { for c in Catalogs: c.loaded implies c.entry_count > 0 }
```

`specs/mcp-protocol.allium`: rewrite `rule ExecCommand` to gate on `CatalogLookup` then delegate to `process-ops/ExecResult`. Remove `deferred ExecCommand.implementation` line.

### Tests

`tests/test_feat.c` (≥6):
1. `FeatInit` populates struct without crashing.
2. Win-version parses correctly under Wine (`is_nt=true` typically; `win_major>=4`).
3. `FeatVersionString` returns non-empty starting with "Windows ".
4. Each `has_*` flag is consistent with corresponding `p*` function pointer (TRUE iff non-NULL).
5. `has_create_pseudo_console` reflects host accurately (test guards subsequent calls with skip-if-absent).
6. `FeatForceFallback(FORCE_NO_THREADS | FORCE_NO_JOB_OBJECTS | FORCE_NO_CTRL_EVENTS)` zeroes flags+fnptrs and exec_ops still succeeds via polling/no-job/Terminate path (verifies fallback equivalence).

`tests/test_exec_ops.c` (≥22 — 18 baseline + 4 capability-fallback):
1. `cmd /c echo hello` → exit=0, stdout = `"hello\r\n"` (b64 `"aGVsbG8NCg=="`)
2. `cmd /c exit 7` → exit=7
3. nonexistent exe → spawn_failed, errMsg has Win32 error code
4. 80KB stdout → `stdout_truncated=1`
5. Timeout (`cmd /c "ping -n 30 127.0.0.1"`, 200ms) → `timed_out=1`, `killed_by` ∈ {`timeout`,`ctrl_break`}
6. Stdin pass-through (`cmd /c findstr foo` with `"foo\nbar\nfoo\n"`) → 2 lines
7. cwd respected (`cmd /c cd` cwd=`C:\`) → stdout starts with `C:\`
8. Stderr capture (`cmd /c dir nonexistent_xyz`) → stderr non-empty, exit≠0
9. Empty cmdline → spawn_failed
10. Cmdline > 32767 → `"command line too long"`
11. Cmdline > 8192 with shell → `"command line too long"`
12. Nonexistent cwd → spawn_failed
13. timeout=0 + quick command → completes
14. stdin_b64 invalid → `"invalid base64"`
15. shell=true vs shell=false: `dir` works only with shell=true
16. exit_code=-1 sentinel for spawn-failed
17. Two concurrent execs → second returns `"busy"` until first finishes
18. Final drain: child writes 1KB then exits — all 1KB captured
19. **Capability fallback — polling path:** `FeatForceFallback(FORCE_NO_THREADS)` then run test #1 → identical result via `PeekNamedPipe` loop (verifies Win32s code path on Wine/NT).
20. **Capability fallback — no job objects:** `FeatForceFallback(FORCE_NO_JOB_OBJECTS)` then run test #1 → succeeds; no Win32 error from missing call.
21. **Capability fallback — no ctrl events:** force off, run test #5 → falls through to direct `TerminateProcess`, `killed_by:"timeout"`.
22. **Job-object memory cap (skip if `!has_create_job_object`):** run `cmd /c "for /L %i in (1,1,9999999) do @set X=%X%blah"` with `mem_cap_bytes=8388608` → child killed, `killed_by:"memory_cap"`.

`tests/test_pty_exec.c` (≥4 — skipped if `!has_create_pseudo_console`):
1. PTY echo: spawn `cmd` with stdin `"echo hi\r\nexit\r\n"`, cols=80 rows=25 → `output_b64` decodes to text containing `"hi"` and ANSI escape sequences (output_kind="ansi").
2. PTY exit code: `cmd /c "exit 5"` via PTY → exit=5.
3. PTY resize: spawn `cmd`, send sized to 132×43 → no error from resize call.
4. PTY capability absent: `FeatForceFallback(FORCE_NO_PTY)` then `PtyExecRun` → returns error `"pty not available on this Windows"`.

`tests/test_argv.c` (12 fixed + PBT 1000 trials):
- `["a","b"]` → `"a b"`
- `["hello world"]` → `"\"hello world\""`
- `["a\"b"]` → `"\"a\\\"b\""`
- `["a\\"]` → `"\"a\\\\\""`
- `["a\\b"]` → `"a\\b"`
- `["a\\\""]` → trailing-backslash-before-quote
- `[""]` → `"\"\""`
- `["x","",""]` → `"x \"\" \"\""`
- shell=false `["a&b"]` → `"\"a&b\""`
- shell=true `[..., "a&b"]` → `^&` inside escape
- All ASCII printable
- DBCS lead bytes (0x81–0x9F, 0xE0–0xFC) — verify no mid-character split
- PBT roundtrip via `argv_echo.exe`

`tests/test_binfmt.c` (6):
- `mcp-w32s.exe` → `BIN_PE32`
- Fixture NE16 → `BIN_NE16`
- Fixture MZ → `BIN_MZ`
- Text file → `BIN_UNKNOWN`
- Missing file → error
- Shell built-in name (`"dir"`) → `BIN_SHELL` without file read

`tests/test_catalog.c` (8):
- Load valid file → entry_count ≥ 30
- Missing file → error
- Malformed JSON → error
- Lookup `"dir"` → entry; `kind=shell_builtin`
- Lookup `"unknown_xyz"` → NULL
- Validate `dir /B` → ok
- Validate `dir /UNKNOWN` → `"argument not allowed"`
- Validate `cl /TC file.c` → ok

Integration (extending `tests/test_serial.c`):
- Full exec JSON → `ProcessCommand` → response shape with all new keys
- `unsafe:true` bypasses catalog
- `unsafe:false` + uncatalogued cmd → `"command not in catalog"`

### Build/CI integration

- `CMakeLists.txt` (single source of truth; `build.sh`/`build.bat` wrap the mingw/vc6 presets): append seven new modules — `src/feat.c src/exec_ops.c src/pty_exec.c src/argv.c src/binfmt.c src/catalog.c src/ready.c` — to main link. Add six test targets (`test_feat`, `test_exec_ops`, `test_pty_exec`, `test_argv`, `test_binfmt`, `test_catalog`) + `argv_echo` helper. Copy `catalog/win32-commands.json` next to test binaries. **Linker note:** none of the delay-loaded APIs (`CreateJobObjectA`, `CreatePseudoConsole`, `IsWow64Process`, etc.) may be referenced by name at link time — they are only called via `g_features.p*` function pointers. If the linker pulls them in, `mcp-w32s.exe` will fail to load on Win32s 1.25a.
- `.github/workflows/build-and-test.yml`: add Wine runs for all six new test binaries. Existing FPU/486 grep auto-applies to all new `.o` files. **Critical checks:**
  - `objdump -d {feat,exec_ops,pty_exec,argv,binfmt,catalog,ready}.o | grep -E 'fld|fst[^r]|cpuid|cmpxchg|bswap|chkstk'` must be empty. If `__chkstk` appears, shrink stack frames (move large `STARTUPINFO`/`PROCESS_INFORMATION`/buffers to `static`).
  - `objdump -p mcp-w32s.exe | grep -E 'CreateJobObject|CreatePseudoConsole|IsWow64Process|GenerateConsoleCtrlEvent|QueryFullProcessImageName'` must be empty (these must NOT appear in the import table — they are runtime-loaded only).
  - `test_feat.exe` output asserts host capabilities under Wine: `is_nt=1`, `has_threads=1`. PTY tests skip if Wine version doesn't expose `CreatePseudoConsole`.
- `build.sh`: new `host-pbt` target — native `gcc -std=c99` build of `vendor/theft` (`src/theft*.c`) + `tests/host/*.c` against the C89 modules under test (`base64.c`, `json_parser.c`, `argv.c`, `catalog.c`); runs without Wine.
- `.github/workflows/build-and-test.yml`: `host-pbt` step runs before the MinGW/Wine suite (fail fast with shrunk counterexamples).
- The seven new `.c` files build under the `vc6` preset too (CMake's NMake Makefiles generator); the theft harness is NOT added — host-side only.
- Artifact upload: `catalog/win32-commands.json` alongside `mcp-w32s.exe`.

### Out of scope for Phase 4 (architectural reasons)

- **Streaming chunked output.** Current MCP-Win32s protocol is one JSON line in, one out. Streaming requires multi-frame response handling on the bridge side. Phase 5+.
- **Async exec (job-id, poll-later).** Conflicts with single-threaded request/response. Phase 5+ if needed.

(Items previously listed as out-of-scope — interactive stdin/TTY, process signals, resource limits — are now **in scope** via the feature-detection uplift. They function on Windows versions that support them and gracefully degrade on Win32s.)

### Verification (sub-agent acceptance criteria)

1. `./build.sh test` clean on Linux/MinGW with strict flags.
2. All new test binaries pass under Wine: `test_feat`, `test_exec_ops`, `test_pty_exec` (or skipped with reason), `test_argv`, `test_binfmt`, `test_catalog`, plus extended `test_serial`.
3. CI FPU/486 grep stays empty for new `.o` files.
4. **Import-table check:** `objdump -p mcp-w32s.exe | grep CreatePseudoConsole` (and the other delay-loaded APIs) returns empty — confirms binary still loads on Win32s 1.25a.
5. End-to-end: `printf '{"cmd":"exec","id":"e1","argv":["cmd","/c","echo","hi"]}\n' | wine mcp-w32s.exe …` returns `exit_code:0`, `stdout_b64:"aGkNCg=="`, `binary_type:"pe32"`, `exec_method:"direct"`, `killed_by:""`.
6. End-to-end (catalog miss): `argv:["nonexistent_xyz"]` without `unsafe` → `"command not in catalog"`.
7. End-to-end (timeout, modern host with ctrl-events): `argv:["cmd","/c","ping","-n","30","127.0.0.1"]` with `timeout_ms:200` → `timed_out:true`, `killed_by:"ctrl_break"` or `"timeout"`.
8. End-to-end (PTY, host with `has_create_pseudo_console`): `{"cmd":"ptyExec","argv":["cmd"],"stdin_b64":"<echo+exit>","cols":80,"rows":25}` → `output_kind:"ansi"`, output contains echoed text.
9. End-to-end (PTY absent): same request with `FeatForceFallback(FORCE_NO_PTY)` (debug build) or on a Win 7 host → `error:"pty not available on this Windows"`.
10. **Ready-message capability assertion** under Wine:
    ```
    printf '\n' | wine mcp-w32s.exe /STDIO | head -1
    ```
    First line parses as JSON with `status:"ready"`, `version` non-empty, `features.is_nt:true`, `features.threads:true`. (Field set varies by Wine version — must always include the documented keys.)
11. `specs/process-ops.allium` (with `Capabilities` entity) and `specs/catalog.allium` follow `specs/file-ops.allium` lexical conventions.
12. README §1554 updated; new "Feature Detection & Graceful Uplift" + "PTY Execution" sections; CLAUDE.md test count bumped to ≥152; `PLAN.md` Phase 4 marked Complete.
13. Total tests: 87 + ≥6 (feat) + ≥22 (exec_ops, incl. 4 capability fallbacks) + ≥4 (pty_exec) + ≥12 fixed + 1000 PBT trials (argv) + ≥6 (binfmt) + ≥8 (catalog) + ≥3 integration = **≥152 tests**.
14. Catalog file ships with binary in CI artifact; loads without warning on startup.
15. **Manual smoke (optional, documented):** load `mcp-w32s.exe` on a real Windows 3.1 + Win32s 1.25a system; ready message advertises `is_win32s:true`, `threads:false`, `pty:false`, `job_objects:false`; `exec` with simple `command.com /c dir` returns expected output through the polling/Terminate fallback path.
16. **Allium lifecycle complete:** all six skills exercised as per "Required workflow" — elicit notes recorded, all specs tend-written/`allium check` clean (including the three distilled backfill specs), test files reference their propagated obligations, and a final `/allium:weed` audit reports zero spec↔code drift.
17. **theft harness green:** `./build.sh host-pbt` builds `vendor/theft` + `tests/host/*` natively and passes ≥50k trials per property; CI runs it before the Wine suite.
18. Every theft property has a mirrored `prop.h` equivalent running on the target binary under Wine.

## Phase 5: MCP Integration — Not Started
- Python bridge: map MCP tool calls to serial/TCP protocol
- Test with Claude Code / Claude Desktop

## Phase 6: Cross-Platform Testing — Not Started
- Verify on Windows 3.1 + Win32s 1.25a
- Verify on Windows 11
- Test across Win9x, NT, XP, modern Windows

## Phase 7: Documentation & Polish — Not Started
- Final README, usage examples, troubleshooting
