/*
 * toolchain_probe.h - Startup detection of installed build toolchains
 *
 * Spec: toolchains.allium (DetectedToolchain, the ToolchainDetected rule) and
 * wire-contract.allium ReadyShape (the ready message's features.toolchains
 * array). At startup the device probes each KNOWN built-in build command that
 * is present in the command catalog: it runs the command's version banner and,
 * if the banner is recognised, records one DetectedToolchain {vendor, command,
 * version}. The bridge consumes the array to gate and generate the build tools.
 *
 * The build tools themselves add NO new wire command - they compose the
 * existing catalogued exec. Only detection + the ready array live here.
 *
 * This is free and unencumbered software released into the public domain.
 * See LICENSE for details (Unlicense).
 */

#ifndef TOOLCHAIN_PROBE_H
#define TOOLCHAIN_PROBE_H

#include "catalog.h"

#define TOOLCHAIN_MAX            8   /* most distinct toolchains we report */
#define TOOLCHAIN_VENDOR_MAX    64
#define TOOLCHAIN_COMMAND_MAX   32
#define TOOLCHAIN_VERSION_MAX  128

/*
 * DetectedToolchain - one installed toolchain the device found. version is the
 * FULL banner version string (e.g. "12.00.8804") - the build number is what
 * distinguishes service packs (every VC6 SP reports _MSC_VER 1200).
 */
typedef struct {
    char vendor[TOOLCHAIN_VENDOR_MAX];
    char command[TOOLCHAIN_COMMAND_MAX];
    char version[TOOLCHAIN_VERSION_MAX];
} DetectedToolchain;

/*
 * ToolchainSet - the detected toolchains (bounded, value-typed: no heap).
 */
typedef struct {
    DetectedToolchain items[TOOLCHAIN_MAX];
    int count;
} ToolchainSet;

/*
 * ToolchainMatchBanner - Pure string logic: given a build command name and the
 * text it printed when run bare, fill *out (vendor + extracted version) if the
 * command is a recognised built-in probe and the banner yields a version.
 *
 * No OS calls - unit-testable without any compiler installed. out->command is
 * set to `command`. Returns 1 on a recognised match, 0 otherwise.
 */
int ToolchainMatchBanner(const char *command, const char *banner,
                         DetectedToolchain *out);

/*
 * ToolchainProbe - Detect installed toolchains. For each known built-in build
 * command that is (a) present in `cat` and (b) runs and prints a banner that
 * ToolchainMatchBanner recognises, append a DetectedToolchain to *out (capped
 * at TOOLCHAIN_MAX). `cat` NULL or no catalogue -> detects nothing.
 *
 * Runs subprocesses (the version banners) via the exec core; intended to run
 * once at startup after FeatInit + catalog load. Returns out->count.
 */
int ToolchainProbe(const Catalog *cat, ToolchainSet *out);

/*
 * ToolchainAppendJson - Append the detected set as the ready message's
 * features.toolchains member: `"toolchains":[{"vendor":..,"command":..,
 * "version":..},...]` (no leading comma; empty set -> `"toolchains":[]`).
 * Fields are JSON-escaped. Bounds-checked against jsonSize; advances *pos.
 * Returns 1 on success, 0 if the buffer is too small.
 */
int ToolchainAppendJson(const ToolchainSet *set, char *json, int jsonSize,
                        int *pos);

#endif /* TOOLCHAIN_PROBE_H */
