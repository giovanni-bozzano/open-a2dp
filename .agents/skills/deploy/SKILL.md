# Deploy and restore

Elevated prompt, here or in the VM; the script never reboots.

## Deploy
- `scripts/deploy.ps1 -Build` builds, runs CodecTests and deploys here (`-SkipTests` reuses a build).
- `scripts/deploy.ps1 -Vm -Build` runs the same installer in the guest from `.local/vm.json`
  (untracked); `-Start` starts a stopped VM, `-Snapshot <name>` reverts to it first.

## Restore
- `scripts/deploy.ps1 -Uninstall` (`-Vm -Uninstall`) removes every `opena2dp.inf` package, both
  service entries and the test certificate, then checks node, services, image, package and
  `BthA2dp`. Exit 0 only on `RESTORED`.
- `pair and connect the sink, then rerun` means a powered-off sink: no node, which the check cannot tell from a failed restore.

## Before a test session
1. Sink paired and connected, `OpenA2dpCtl list` shows it, `sc query OpenA2dpSvc` is RUNNING.
2. Kernel log: the `IHVDRIVER` mask in `Debug Print Filter` (reboot) or `ed
   nt!Kd_IHVDRIVER_Mask 0xF` under WinDbg; capture with `.logopen`. Service log:
   `%ProgramData%\OpenA2dp\OpenA2dpSvc.log`.
3. Verifier: `verifier /standard /driver OpenA2dp.sys`, reboot, confirm with `verifier /query`;
   `verifier /reset` plus the `Parameters\Wdf` values to stop it.

## When it fails
- pnputil timeout or exit 3: no further PnP calls and no reboot; capture with
  `windbg-deadlock-check`, then revert the VM snapshot.
- Exit 2: driver still loaded, nothing installed. Copy errors: stop the service and tools, rerun.
