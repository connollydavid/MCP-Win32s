//! 5.0 property tests (the theft analog): the pure capability-gating and
//! response-mapping logic. Cites obligations from bridge/OBLIGATIONS-5.0.md.

use mcp_w32s_bridge::capabilities::{Capabilities, EncodingProvenance, MemTier};
use mcp_w32s_bridge::server::{unsafe_gate, ExecGate};
use mcp_w32s_bridge::wire::{Features, Response};
use proptest::prelude::*;

fn mem_strat() -> impl Strategy<Value = MemTier> {
    prop_oneof![
        Just(MemTier::None),
        Just(MemTier::Process),
        Just(MemTier::Arena),
        Just(MemTier::SharedVm),
    ]
}

fn enc_strat() -> impl Strategy<Value = EncodingProvenance> {
    prop_oneof![
        Just(EncodingProvenance::Manifest),
        Just(EncodingProvenance::ViaWide),
        Just(EncodingProvenance::FromCodepage),
        Just(EncodingProvenance::Unknown),
    ]
}

fn caps(
    pty: bool,
    mem: MemTier,
    enc: EncodingProvenance,
    allow_memory_write: bool,
) -> Capabilities {
    Capabilities {
        has_pty: pty,
        mem,
        encoding: enc,
        codepage: 437,
        version: "t".to_string(),
        toolchains: vec![],
        toolchain_registration: false,
        allow_memory_write,
        allow_unsafe_exec: false,
    }
}

proptest! {
    /// capability_satisfied (rule-success.ToolAdvertised, invariant.
    /// AdvertisedToolsAreCapable; 5.3 capability_satisfied_mem): each named
    /// capability matches the device exactly; an unknown capability is never
    /// satisfied. The two-factor mem_write gate holds iff BOTH the tier is
    /// non-none AND the operator opted in (MemoryWriteToolRequiresOptIn).
    #[test]
    fn satisfies_matches_capabilities(
        pty in any::<bool>(),
        mem in mem_strat(),
        enc in enc_strat(),
        amw in any::<bool>(),
    ) {
        let c = caps(pty, mem, enc, amw);
        prop_assert_eq!(c.satisfies("pty"), pty);
        prop_assert_eq!(c.satisfies("mem"), mem != MemTier::None);
        prop_assert_eq!(c.satisfies("mem_write"), mem != MemTier::None && amw);
        // 5.4 retired "utf8": encoding is informational, so it never gates a
        // tool. The old gate name now falls through to the unknown arm.
        prop_assert!(!c.satisfies("utf8"));
        prop_assert!(!c.satisfies("unknown_capability_xyz"));
        // mem_write is strictly stronger than mem: it never holds where mem
        // does not (a poke can never advertise where peek cannot).
        if c.satisfies("mem_write") {
            prop_assert!(c.satisfies("mem"));
        }
    }

    /// from_ready reads the mem tier + the informational encoding provenance
    /// (rule-success.CapabilitiesResolved, enum-comparable.{MemTier,
    /// EncodingProvenance}) from the features payload, defaulting conservatively
    /// when absent/unrecognised (mem none, encoding Unknown).
    #[test]
    fn from_ready_reads_tiers(pty in any::<bool>(), mem in "process|arena|shared_vm|none|garbage", enc in "utf8_manifest|utf8_via_w|utf8_from_cp|garbage") {
        let mut f = Features { pty, ..Default::default() };
        f.extra.insert("mem".to_string(), serde_json::Value::String(mem.clone()));
        f.extra.insert("encoding".to_string(), serde_json::Value::String(enc.clone()));
        let c = Capabilities::from_ready(437, "t".to_string(), &f, false, false, false);
        prop_assert_eq!(c.has_pty, pty);
        let want_mem = match mem.as_str() {
            "process" => MemTier::Process, "arena" => MemTier::Arena,
            "shared_vm" => MemTier::SharedVm, _ => MemTier::None,
        };
        prop_assert_eq!(c.mem, want_mem);
        let want_enc = match enc.as_str() {
            "utf8_manifest" => EncodingProvenance::Manifest,
            "utf8_via_w" => EncodingProvenance::ViaWide,
            "utf8_from_cp" => EncodingProvenance::FromCodepage,
            _ => EncodingProvenance::Unknown,
        };
        prop_assert_eq!(c.encoding, want_enc);
    }

    /// invariant.UnsafeExecRequiresOperatorOptIn (THE SAFETY PIN, PBT) — over a
    /// random tool, an unsafe request, and the operator opt-in, the bridge's
    /// per-call decision (the SAME `unsafe_gate` the handler calls) satisfies the
    /// two pinned implications:
    ///   (1) dispatched ∧ unsafe_requested ⟹ allow_unsafe_exec
    ///       (an unsafe bypass reaches the device only under the opt-in), and
    ///   (2) outcome = unsafe_opt_in_absent ⟹ status = error ∧ unsafe_requested
    ///       (the local-refusal outcome is always a non-dispatched recoverable
    ///       error that did request unsafe).
    /// Only win32_exec carries unsafe; for any other tool unsafe_requested is
    /// always false (no exec semantics), so it always dispatches.
    #[test]
    fn unsafe_reaches_device_only_under_opt_in(
        is_exec in any::<bool>(),
        unsafe_req in any::<bool>(),
        allow_unsafe in any::<bool>(),
    ) {
        // Non-exec tools never carry an unsafe request.
        let unsafe_requested = is_exec && unsafe_req;

        // The bridge's decision: exec routes through unsafe_gate; a non-exec
        // tool (no unsafe) always relays.
        let gate = if is_exec {
            unsafe_gate(unsafe_requested, allow_unsafe)
        } else {
            ExecGate::Relay
        };

        // Map the gate onto the spec's (status, outcome) for this call.
        let (dispatched, status_error, outcome_unsafe_absent) = match gate {
            // Relay => the call dispatches (status reaches the device, not error
            // locally) with outcome `relayed`.
            ExecGate::Relay => (true, false, false),
            // RefuseLocally => a non-dispatched recoverable error with outcome
            // `unsafe_opt_in_absent`.
            ExecGate::RefuseLocally => (false, true, true),
        };

        // (1) dispatched ∧ unsafe_requested ⟹ allow_unsafe_exec.
        if dispatched && unsafe_requested {
            prop_assert!(allow_unsafe, "an unsafe bypass dispatched only under the opt-in");
        }
        // (2) outcome = unsafe_opt_in_absent ⟹ status = error ∧ unsafe_requested.
        if outcome_unsafe_absent {
            prop_assert!(status_error, "the refusal outcome is a recoverable error");
            prop_assert!(unsafe_requested, "the refusal outcome implies unsafe was requested");
            prop_assert!(!dispatched, "the refusal never dispatched to the device");
        }
        // And the refusal happens iff exec requested unsafe without the opt-in.
        prop_assert_eq!(
            outcome_unsafe_absent,
            unsafe_requested && !allow_unsafe,
            "RefuseLocally iff unsafe requested and not opted in"
        );
    }

    /// Response parsing never panics on arbitrary JSON-ish input and the
    /// ok/error discrimination is total (the isError-vs-protocol split
    /// rests on this).
    #[test]
    fn response_parse_is_total(status in "ok|error|weird", with_err in any::<bool>()) {
        let mut v = serde_json::json!({"id": "x", "status": status});
        if with_err { v["error"] = serde_json::Value::String("boom".into()); }
        let r: Response = serde_json::from_value(v).unwrap();
        // is_ok() is exactly status == "ok"
        prop_assert_eq!(r.is_ok(), status == "ok");
    }
}
