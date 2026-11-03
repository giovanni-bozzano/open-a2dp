# Codec configuration design (planned: M5b -> M6)

Implement in order:
1. Per-headset registry settings (`Parameters\Devices\<addr>`) and global defaults: codec priority, SBC rate/channel mode/blocks/subbands/allocation/bitpool, AAC rate/channels/bitrate/VBR.
2. Preference-aware `SbcConfig`/`AacConfig` selection with supported-value fallback. Test Default, SBC HQ and SBC XQ presets against media MTU limits.
3. Config IOCTLs and desktop UI (`OpenA2dpConfig.exe`) for device, codec, presets and parameters. Apply via SUSPEND/RECONFIGURE/START, or reconnect for codec changes. Existing CLI status/codec preference remains the interim test path.
4. Multi-rate ACX: 44.1/48 kHz and selected mono, matching the negotiated format or resampling; last to protect the validated audio path.
5. M6: finish UI and clean install/uninstall restoring `BthA2dp.sys`.
