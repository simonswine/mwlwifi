# Native single-channel contexts (experimental)

This driver uses native channel-context callbacks with unmodified mac80211.
No external mac80211 patch is required.

## Ownership

`mwl_priv.chanctx` identifies the single accepted operating context.
`radio_conf` holds a copy of the last successfully configured radio state.
The context definition and radar requirement, not `hw->conf.chandef` or
`hw->conf.radar_enabled`, determine operating configuration. `config()`
continues to handle idle state and software-scan/off-channel tuning.

Native add/remove/change and VIF assign/unassign callbacks replace all
emulation callbacks. Multiple VIFs can share one context. A second context
returns -EBUSY; nonzero link IDs are unsupported. Native SWAP_CONTEXTS
replacement is described in [CSA.md](CSA.md).

Radar-only changes directly configure firmware without requiring retuning
or a config() callback. Firmware/RX/TX helpers obtain their channel from
`mwl_radio_conf()`, including rate selection and RX channel validation.

RX can run before the first context is configured, while the native
`hw->conf.chandef.chan` is NULL. RX paths must drop packets without a valid
configured channel. Retain the last non-NULL channel snapshot for in-flight
RX/TX during teardown. Startup station/survey accesses also guard NULL.

## DFS sequencing

A new radar-required context starts with action 1. Before enabling DFS
beacons, action 3 must succeed. Operational monitoring is latched until
context removal or radar disable, so disabling beacons does not immediately
re-enter CAC mode. Context removal stops detection and disables the radio.
This is context-lifecycle tracking, not an explicit CAC-completion signal:
mac80211/hostapd remain responsible for permitting beacon startup.

Failed tuning disables the radio. Outside CSA, a failure in the void change
callback also requests a mac80211 restart; it cannot synchronously return an
error. During CSA, failure instead quiesces interfaces and latches a fault,
without automatically restarting on a potentially unavailable old channel.

## Validation and limitations

Built for OpenWrt kernel 6.12.94 and mac80211 backports 6.18.26. Tested on
WRT1900ACS/88W8864, firmware 0x0702091b, FR/ETSI, 20 MHz. Tests covered
fresh CAC, AP startup, native radar-only notification, firmware-reported
radar evacuation, non-DFS CSA, associated-client traffic and switch failure
recovery. See [CSA.md](CSA.md) for results and qualifications.

This is not a production-ready conversion. RX-chain/SMPS context changes
and per-context power policy need further review. Scanning/restoration,
multiple VIFs, firmware restart/recovery, monitor/station operation, other
regions and other chip families need hardware regression testing. The
channel-state snapshot follows the driver's existing RX/TX synchronization
model; concurrency checks remain necessary.

Use external kernel logs and retain rollback modules when testing. Initial
startup testing exposed an RX NULL-channel panic; subsequent guarded builds
passed the reported tests without panic. A successful build or firmware
command does not establish RF radar-detection effectiveness or regulatory
certification. Never bypass CAC, alter NOP state, or weaken detection for
these tests.
