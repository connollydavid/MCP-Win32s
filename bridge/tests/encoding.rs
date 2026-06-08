//! 5.4 bridge passthrough tests. The device owns the whole text pipeline and
//! emits valid UTF-8 on every OS tier, so the wire is uniformly UTF-8: the
//! `encoding` ready tag is INFORMATIONAL provenance (it gates nothing) and the
//! bridge adds no charset crate. Obligations: bridge/OBLIGATIONS-5.4.md.

use mcp_w32s_bridge::capabilities::{tools_to_prune, Capabilities, EncodingProvenance};
use mcp_w32s_bridge::wire::Features;

fn caps_with_encoding(tag: Option<&str>, mem: Option<&str>) -> Capabilities {
    let mut f = Features::default();
    if let Some(t) = tag {
        f.extra
            .insert("encoding".into(), serde_json::Value::String(t.into()));
    }
    if let Some(m) = mem {
        f.extra
            .insert("mem".into(), serde_json::Value::String(m.into()));
    }
    Capabilities::from_ready(437, "t".into(), &f, false, false, false)
}

/// encoding_tag_informational (OBLIGATIONS-5.4.md, bridge): the tag is parsed
/// onto DeviceCapabilities.encoding but gates NOTHING — two devices that differ
/// only in their encoding provenance advertise/prune exactly the same tools,
/// and the retired "utf8" capability is never satisfied.
#[test]
fn encoding_tag_informational() {
    // Same memory tier, different encoding provenance.
    let manifest = caps_with_encoding(Some("utf8_manifest"), Some("process"));
    let viaw = caps_with_encoding(Some("utf8_via_w"), Some("process"));
    let absent = caps_with_encoding(None, Some("process"));

    // The tag is faithfully recorded (informational provenance).
    assert_eq!(manifest.encoding, EncodingProvenance::Manifest);
    assert_eq!(viaw.encoding, EncodingProvenance::ViaWide);
    assert_eq!(absent.encoding, EncodingProvenance::Unknown);

    // ...but it changes nothing about tool advertisement.
    let p_manifest = tools_to_prune(&manifest);
    assert_eq!(
        p_manifest,
        tools_to_prune(&viaw),
        "encoding provenance does not affect pruning"
    );
    assert_eq!(
        p_manifest,
        tools_to_prune(&absent),
        "an absent encoding tag prunes identically"
    );

    // The retired "utf8" capability never gates a tool.
    assert!(!manifest.satisfies("utf8"));
    assert!(!absent.satisfies("utf8"));
}

/// no_encoding_dep (OBLIGATIONS-5.4.md, bridge): the passthrough design adds NO
/// charset crate — the device owns all transcoding. Guards Cargo.toml so the
/// dropped prior-plan crates (`encoding_rs`/`oem_cp`) cannot creep back in.
#[test]
fn no_encoding_dep() {
    let manifest = include_str!("../Cargo.toml");
    for forbidden in ["encoding_rs", "oem_cp"] {
        assert!(
            !manifest.contains(forbidden),
            "Cargo.toml must not depend on {forbidden} (the device owns transcoding)"
        );
    }
}
