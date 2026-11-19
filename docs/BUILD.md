# Build and deploy

[README](../README.md) | [Using OpenA2DP](USING.md) | [Testing](TESTING.md)

## Prerequisites

- Windows 11 x64.
- Visual Studio 2026 with the v145 C++ toolset and MSBuild.
- The WDK and Windows SDK packages pinned in `Directory.Packages.props`.
- An elevated prompt to install or remove the driver.
- Test signing enabled on any machine that loads the driver. The deploy script
  installs its test certificate in the machine's Root store and the restore path
  removes it.

The solution is x64 only. Debug and Release are the two configurations.

## Build

From a Developer PowerShell for Visual Studio:

```powershell
& MSBuild.exe OpenA2dp.slnx /p:Configuration=Debug /p:Platform=x64
```

Outputs go to `out\x64\<Configuration>\`:

| Output | Built from | Role |
|---|---|---|
| `OpenA2dp.sys` | `driver/` | KMDF/ACX driver, SBC encoder and A2DP transport |
| `OpenA2dpSvc.exe` | `service/` | AAC, aptX, aptX LL, aptX HD and LDAC encoders |
| `OpenA2dpCtl.exe` | `tools/OpenA2dpCtl/` | Command-line control and status |
| `OpenA2dpConfig.exe` | `tools/OpenA2dpConfig/` | Settings window |
| `CodecTests.exe` | `tests/CodecTests/` | Host tests, no hardware needed |

Warnings are errors. The vendored encoders are checked separately:

```powershell
.\scripts\check-vendored-encoders.ps1
```

The script compares `codecs/aptx/classic`, `codecs/aptx/hd` and `codecs/ldac`
against their `SHA256SUMS`. `-Update` rewrites the manifests after a deliberate re-vendor;
a normal build never needs it.

## Host tests

`CodecTests.exe` covers the codec math, packet layout, AVDTP negotiation, ring
buffers, frame records, latency arithmetic, configuration parsing and the status
report. It needs no sink and no driver.

```powershell
.\out\x64\Debug\CodecTests.exe
```

Exit code 0 means every test passed. [Testing](TESTING.md) describes the suites
and what they do not cover.

## Deploy

From an elevated prompt:

```powershell
.\scripts\deploy.ps1 -Build
```

`-Build` runs the solution and `CodecTests`, then installs. The script performs
the update without rebooting:

1. Stops the encoder service and both tools, which hold handles on the driver.
2. Disables the OpenA2DP device node so the driver unloads.
3. Confirms the driver unloaded; if it did not, re-enables the node and aborts.
4. Installs the package while no image is loaded.
5. Enables the node so the new image starts.
6. Confirms the loaded binary and starts the encoder service.

The package, tools and service are copied into the deploy folder printed by the
script. `OpenA2dpCtl.exe` and `OpenA2dpConfig.exe` are run from there, elevated.

| Option | Effect |
|---|---|
| `-Configuration Release` | Use the Release build instead of Debug |
| `-SkipTests` | With `-Build`, skip the host tests |
| `-NoRestart` | Stage the package only; the node picks it up on its next restart |
| `-KeepAudio` | Do not stop the Windows Audio service while updating |
| `-InstallTimeoutSec <n>` | Limit the installer step (default 240 seconds) |
| `-Vm` | Run the same installer in the test VM |
| `-Start` | Start the VM if it is stopped |
| `-Snapshot <name>` | Revert the VM to a snapshot before deploying |
| `-VmConfig <file>` | VM settings file; defaults to `.local/vm.json` |
| `-VmRun <path>` | `vmrun.exe` to use |

The VM target needs VMware Tools in the guest. `.local/vm.json` is untracked and
holds the `.vmx` path, the guest credentials and, for an encrypted VM, the VM
password:

```json
{
  "vmx": "test-vm/OpenA2dp.vmx",
  "user": "user",
  "password": "password",
  "vmPassword": "password"
}
```

## Setup

`out\x64\<Configuration>\OpenA2dpSetup.exe` is the self-contained installer. It embeds the
driver package, the encoder service and both tools, so it can be handed out on its own. Run it
elevated; it asks for an install folder, defaulting to `%ProgramFiles%\OpenA2DP`.

Setup adds the package to the driver store with `pnputil /add-driver` and deliberately omits
`/install`: no headset is switched. Choose headsets afterwards in `OpenA2dpConfig.exe` or with
`OpenA2dpCtl driver opena2dp`. The wizard and its operations live in `tools/OpenA2dpSetup/`;
`scripts/generate-setup-payload.ps1` embeds the build outputs.

Running setup again on the same machine updates the existing installation in place. It keeps the
saved headset settings, replaces the files and service, and restarts only headsets that were
already on OpenA2DP so they load the new driver. Headsets on the Windows driver are not touched.
After the new package is verified, setup removes the superseded `opena2dp.inf` packages from the
driver store. That cleanup does not use `/uninstall`, so it does not switch or uninstall a sink; if
Windows still holds a package, setup reports it and requests a restart when Windows requires one.

Remove OpenA2DP from **Settings > Apps**. The uninstaller returns every OpenA2DP sink to
`BthA2dp`, removes every `opena2dp.inf` package from the driver store, removes both service
entries, the shortcut, the Apps entry, the test certificate it added and the installed files.
Saved per-sink settings are kept unless **Also delete saved headset settings** is ticked on the
confirmation page. A restart is requested when Windows cannot finish a removal immediately;
setup reports that on the progress page.

## Restore

```powershell
.\scripts\deploy.ps1 -Uninstall
```

Add `-Vm` to restore the test VM. The restore path removes every `opena2dp.inf`
package, the kernel service entry, the encoder service and the test certificate,
then verifies that no device node, image, package or control device is left and
that a sink is bound to `BthA2dp` again. Copied binaries stay in the deploy
folder, so a redeploy does not need a rebuild.

The `BthA2dp` check needs the sink paired **and connected**. A powered-off sink
has no node; the script reports `pair and connect the sink, then rerun` rather
than guessing.

## Same build everywhere

The driver, service and tools share one ABI. Deploy all three from the same
build, and rebuild all three after any change to
`common/include/OpenA2dp/Ioctl.h`. The version is checked when a service or tool
attaches, and a mismatch is refused instead of adapted.

## When a build or deploy fails

- **MSBuild or the WDK is missing.** Use a Developer PowerShell from the matching
  Visual Studio install and restore the pinned packages; a user-mode-only build
  cannot produce the driver.
- **The package installs but the sink does not move.** Check the node's bound
  service and problem code with `OpenA2dpCtl driver`; the switch verifies both and
  puts the sink back on the driver it came from when it fails.
- **The driver is still loaded.** The deploy script aborts instead of replacing a
  loaded image. Stop the service and the tools, then retry.
- **A PnP call stops answering.** Do not reboot and do not repeat the call. Keep
  the state for debugging; on a VM, revert the snapshot.
- **The sink is gone after a restore.** Pair and connect it, then rerun the
  restore check; a powered-off sink looks the same as a failed restore.
