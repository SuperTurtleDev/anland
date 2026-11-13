# Service-context overlay and Android OTA

The installer generates a complete platform service-context overlay from the
current device's file, inserting the Anland fragment before the wildcard.
Every other platform mapping is preserved. The installer records a SHA-256
fingerprint of that source with Anland's own generated entry excluded. The
shared normalization makes repeated installation work while this module's
current overlay is visible.

KernelSU's module post-fs-data stage precedes its metamodule mount stage.
The boot hook compares the current source with the recorded fingerprint. A
change, unreadable source, missing helper or invalid fingerprint creates both
`disable` and `skip_mount` before the overlay can be applied. The hook leaves
those markers in place on later runs. Its log is
`/data/local/tmp/awl_post_fs_data.log`.

After an Android OTA disables Anland, reinstall the module for the new ROM and
reboot. Generate the replacement from the current ROM. Re-enabling a stale
module without reinstalling will trigger the guard again. Other modules that
replace the same complete file require coordinated overlay ownership.

The guard depends on the root implementation and metamodule honoring the
standard pre-mount stage and module disable/skip-mount markers. Hot mounting
an old overlay before this stage is outside that lifecycle contract.

Run the host script and ZIP-packaging regressions with

```sh
python3 -m unittest discover -s tests -v
```

The packaging test uses inert daemon/audio placeholders to exercise the real
module recipe. Its temporary archive is a fixture. Native/audio compilation
remains part of the normal build workflow.

KernelSU stage ordering is documented and implemented in
[the module guide](https://kernelsu.org/guide/module.html) and
[init_event.rs](https://github.com/tiann/KernelSU/blob/main/userspace/ksud/src/init_event.rs).
