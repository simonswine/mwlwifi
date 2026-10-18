# Native channel-switch implementation (experimental)

This is driver-only code for stock mac80211 backports 6.18.26, built for
the test router's 6.12.94 kernel. Single-AP CSA, firmware-reported DFS
evacuation and selected failure-path live tests passed; see results and
remaining limitations below.

## Lifecycle and callback ordering

The examined AP path in mac80211 is:

1. pre_channel_switch(): validate and save intent only.
2. Reserve replacement context and install CSA beacon(s).
3. channel_switch_beacon(): submit firmware command after all participating
   interfaces have completed steps 1-2.
4. Firmware switch interrupt: mark physical completion, publish the target
   channel for RX/TX and call ieee80211_csa_finish() for participants only.
5. switch_vif_chanctx(SWAP_CONTEXTS): validate the entire replacement list,
   apply target power/radar configuration without another RF retune and
   commit the driver context/interface pointers.
6. mac80211 installs after-CSA beacons and calls post_channel_switch().
   Release driver queue blocking after all participants have posted.

State progression:

    IDLE -> PREPARED -> PENDING -> COMPLETING -> IDLE
                         \              \
                          +----> RECOVERING (transmission quiesced)

Requests with count <= 1 have no CSA beacon callback in this mac80211 AP
path. They use PREPARED -> COMPLETING: stop queues and BSS transmission,
retune synchronously in switch_vif_chanctx, configure radar, then restart
BSSes from post_channel_switch after normal beacons have been installed.
Only a single AP is supported for this immediate path.

## Validation and ownership

- One physical operating channel, with native SWAP_CONTEXTS replacement.
  No temporary admission of a second hardware channel/context.
- REASSIGN_VIF is explicitly unsupported.
- All assigned interfaces must be beaconing APs, share the old context,
  beacon interval, target, count and block_tx policy, and participate in the
  same replacement. No partial-radio move or nonzero link IDs.
- Same-band 20/40/80 MHz only; puncturing and frequency offsets rejected.
- Saved target intent is distinct from the last programmed radio channel.
- Beacon uploads are refreshed for CSA and after-CSA changes. Upload errors
  stop the switch rather than submitting a countdown with stale templates.
- The switch firmware helper checks firmware result codes. The legacy
  count+1 encoding is preserved, not assumed experimentally validated.
- Exactly one firmware countdown command is submitted for the group.

## Completion, concurrency and failure

Callback state and VIF lifetime access use the wiphy lock. IRQ handling only
checks pending state, tags queued work with the generation, and queues the
existing completion worker. That worker takes the wiphy lock. Known stale
queued work and duplicate completions outside PENDING are ignored.

The interrupt has no firmware transaction ID or reported target. Generation
tracking cannot distinguish a genuinely late old hardware interrupt arriving
*during* a newer pending request. This protocol limitation needs hardware
review/testing; generation tagging is not proof of physical target identity.

Preparation, firmware completion and mac80211 finalization all have bounded
timeouts. Pending timeout is derived from count and beacon interval plus
three seconds; requests exceeding sixty seconds are rejected. Preparation
and finalization each allow five seconds. Deadline checks prevent an old
running timeout from expiring a newer request.

Abort, participating-interface unassignment, invalid replacement, command
failure and timeout stop queues, request BSS/radio shutdown and request
cfg80211 interface stop. Firmware stop failures are logged. Recovery never
retunes/resumes the old channel automatically: it may be in DFS NOP.

A fault latch blocks radio/context restart even if mac80211 tears down the
context and clears switch bookkeeping. Explicit driver/firmware recovery is
required. Reload alone does not guarantee a cold firmware reset on this
hardware; use an actual firmware reset/reboot if switch execution is unknown.

Stop/context removal invalidate queued completions and cancel timeouts.
Module deinit synchronously drains delayed timeout and completion work after
unregistering hardware. No synchronous cancellation is done under the wiphy
lock, avoiding a worker/wiphy-lock deadlock.

## DFS policy

Strict cfg80211 beacon-permission checks cover the target chandef, including
secondary channels. A CSA never substitutes for CAC or clears DFS state.
Targets requiring fresh CAC or in non-occupancy must be handled by stopping
and using the ordinary CAC path, not by this CSA path.

Firmware-countdown switches currently accept only non-DFS targets: whether
firmware preserves detection across its autonomous retune is not yet known,
and it could resume target beacons before the completion interrupt. This
restriction permits DFS evacuation to an available non-DFS channel without
claiming unverified DFS-to-DFS monitoring continuity.

The immediate stopped-BSS path can accept an already-available DFS target
only when monitoring was already active, with no Japan radar-profile group
change. Detection is applied before BSS restart. Non-DFS-to-DFS CSA, cross-
band CSA and new-CAC targets are intentionally rejected.

Off-channel config requests are rejected while a switch is in progress.
Radar detection/thresholds, NOP and CAC durations are never overridden.

## Diagnostics and remaining verification

`dfs_radar` reports native context/radar state, csa_state (enum in core.h),
fault latch, generation and expected/ready/posted MAC-ID masks. Debug-level
logs identify switch lifecycle transitions; errors retain failure reasons.
Investigation-only configuration/beacon snapshots and per-command payload/
success dumps have been removed.

Build/link checks and git diff --check pass.

## Live results (stock mac80211, WRT1900ACS/88W8864)

An isolated WPA2 AP at 20 MHz passed three consecutive non-DFS switches:

- 36 -> 44, count 5, block_tx=true: firmware completion, native context
  commit, post callback and idle state; hostapd remained ENABLED on 44.
- 44 -> 36, count 1: immediate stopped-BSS path committed and finished;
  hostapd ENABLED on 36.
- 36 -> 44, count 5: repeated firmware countdown completed successfully.

No extra SetRfChannel command occurred between firmware CSA submission
and finish. No CSA failure, panic, kernel warning or reboot was observed.
The fault latch stayed clear. AP teardown cleared the context and returned
CSA bookkeeping to idle. Original UCI configuration was restored (channel
100 configured but both radios disabled); no DFS channel was used in these
CSA tests. The new module remains loaded temporarily, not installed.

These first three switches had no associated clients. Additional client,
DFS evacuation and teardown-failure tests are recorded below. Over-the-air
CSA beacon contents/countdown, multi-AP, duplicate/late events, firmware
command-rejection injection, other bands/widths and firmware recovery still
require validation. Completion-timeout injection is covered below. No production deployment until
these tests pass.

Router logs: /tmp/csa-live-1792248831/.
Externally streamed logs/local final capture: /tmp/mwlwifi-csa-live/.
Module: /tmp/mwlwifi-native-csa.ko.
Rollback module on router: /tmp/mwlwifi-before-csa.ko.

## Client, DFS evacuation and fault-path follow-up

The test host joined using its Wi-Fi adapter while management SSH stayed
on Ethernet. The WPA2 test AP remained unbridged; the client had only an
IPv6 link-local address and tested HTTP to the AP's scoped link-local
address. The temporary NetworkManager profile was deleted afterward and
the original Wi-Fi connection restored.

- Associated client remained authorized through non-DFS 36 -> 44 (count 5)
  and 44 -> 36 (count 30), both block_tx=true. Station connected-time did
  not reset. HTTP probing recorded 110 successes and nine one-second
  connection timeouts across the switching observation window. Data resumed;
  this is not loss-free continuity or a precise outage-duration measurement.
- A second/conflicting request during the count-30 switch was rejected;
  the first switch completed. A request to a fresh-CAC DFS target (116)
  was also rejected. Both returned ubus Operation not supported; these
  establish control-path rejection before firmware submission, not isolated
  coverage of a particular driver rejection branch. Fault latch stayed clear.
- Full CAC on channel 100 completed with action 1 followed by action 3.
  Approximately five seconds after AP startup firmware reported radar
  naturally (no injected radar event). Restricted selection chanlist=36 100
  led hostapd to request CSA to 36, which completed in about 0.7 seconds
  from detection to finish. Channel 100 entered dfs_state=1 (unavailable).
  The client finished associating ten seconds AFTER evacuation and HTTP
  worked afterward: uninterrupted client continuity during DFS evacuation
  was not demonstrated. The firmware report is not proof of genuine RF
  radar versus a false positive.
- During a non-DFS count-50 CSA, diagnostics confirmed PENDING before AP
  teardown. The driver logged "participating AP stopped beaconing", stopped
  interfaces/radio and latched csa_faulted=1. A subsequent start attempt on
  non-DFS channel 36 failed with I/O error, with no active radio context.
  No panic/reboot was observed. Firmware timeout itself was not injected.

At the end of that test, original router UCI was restored and both radios
were disabled; the module was deliberately left fault-latched. No reload or
DFS state reset was done during that test, preserving channel 100's NOP.

Router logs/snapshots: /tmp/csa-client-1792249919/.
External logs, HTTP probes and final log: /tmp/mwlwifi-csa-client/.

## Completion-timeout injection and cleanup

After the previous NOP interval had elapsed, a clean reboot established a
new test baseline. An isolated build directory, not the repository, was
modified to return early from the valid pending-completion handler. No
module parameter, debugfs injection control, or test hook was added to the
committed source.

On non-DFS 36 -> 44, count 5, block_tx=true:

- Firmware completion was observed at uptime 50.129 s and deliberately
  dropped by the isolated build.
- The pending watchdog expired at 53.274 s, about 3.74 seconds after command
  submission, and stopped the radio/interfaces.
- The fault latch remained set after context teardown. A subsequent start
  attempt failed with I/O error; no context became active.
- No panic, deadlock or unexpected reboot was observed.

The injected build/source directories were deleted. The router was rebooted
again to discard the injected module and recover firmware state. The cleaned
module passed another 36 -> 44, count-5 CSA: AP enabled on 44, state IDLE,
fault latch clear. Original UCI configuration was then restored and both
radios disabled. The clean module remains temporarily loaded, not installed.
Its binary and repository source contain no completion-drop hook. The
obsolete alternative mac80211 emulation patch was also removed.

The clean module built/linked without compiler diagnostics for kernel
6.12.94. git diff --check passed. Kernel checkpatch reported no code errors
or warnings; including the new documentation adds only its MAINTAINERS
file-addition advisory.
Clean smoke-test router capture: /tmp/csa-clean-1792311218/.
Clean module on the router/test host: /tmp/mwlwifi-native-clean.ko.

Externally saved timeout logs/state: /tmp/mwlwifi-timeout-test/.
These local test paths are evidence locations, not files shipped with the
driver; router /tmp captures do not survive the recovery reboot.
