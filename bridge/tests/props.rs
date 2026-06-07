//! 5.0 property tests (the theft analog): the pure capability-gating and
//! response-mapping logic. Cites obligations from bridge/OBLIGATIONS-5.0.md.

use mcp_w32s_bridge::capabilities::{Capabilities, EncodingMode, MemTier};
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

fn enc_strat() -> impl Strategy<Value = EncodingMode> {
    prop_oneof![Just(EncodingMode::Codepage), Just(EncodingMode::Utf8Native)]
}

fn caps(pty: bool, mem: MemTier, enc: EncodingMode, allow_memory_write: bool) -> Capabilities {
    Capabilities {
        has_pty: pty,
        mem,
        encoding: enc,
        codepage: 437,
        version: "t".to_string(),
        toolchains: vec![],
        toolchain_registration: false,
        allow_memory_write,
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
        prop_assert_eq!(c.satisfies("utf8"), enc == EncodingMode::Utf8Native);
        prop_assert!(!c.satisfies("unknown_capability_xyz"));
        // mem_write is strictly stronger than mem: it never holds where mem
        // does not (a poke can never advertise where peek cannot).
        if c.satisfies("mem_write") {
            prop_assert!(c.satisfies("mem"));
        }
    }

    /// from_ready reads the mem/encoding tiers (rule-success.CapabilitiesResolved,
    /// enum-comparable.{MemTier,EncodingMode}) from the features payload,
    /// defaulting conservatively when absent.
    #[test]
    fn from_ready_reads_tiers(pty in any::<bool>(), mem in "process|arena|shared_vm|none|garbage", enc in "utf8_native|codepage|garbage") {
        let mut f = Features { pty, ..Default::default() };
        f.extra.insert("mem".to_string(), serde_json::Value::String(mem.clone()));
        f.extra.insert("encoding".to_string(), serde_json::Value::String(enc.clone()));
        let c = Capabilities::from_ready(437, "t".to_string(), &f, false, false);
        prop_assert_eq!(c.has_pty, pty);
        let want_mem = match mem.as_str() {
            "process" => MemTier::Process, "arena" => MemTier::Arena,
            "shared_vm" => MemTier::SharedVm, _ => MemTier::None,
        };
        prop_assert_eq!(c.mem, want_mem);
        let want_enc = if enc == "utf8_native" { EncodingMode::Utf8Native } else { EncodingMode::Codepage };
        prop_assert_eq!(c.encoding, want_enc);
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
