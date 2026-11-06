# Codec configuration design (planned: M5b -> M6)

Implement in order:
1. Per-sink registry settings (`Parameters\Devices\<addr>`) and global defaults: codec priority, SBC rate/channel mode/blocks/subbands/allocation/bitpool, AAC rate/channels/bitrate/VBR. Done: the codec preference (`CodecPreference*` values, sink key over the global default, restored at device start); the per-codec parameter values are stored but not yet honoured by selection.
2. Preference-aware `SbcConfig`/`AacConfig` selection with supported-value fallback. Test Default, SBC HQ and SBC XQ presets against media MTU limits.
3. Config IOCTLs and desktop UI (`OpenA2dpConfig.exe`) for device, codec, presets and parameters. A codec change CLOSEs and rediscovers an active stream immediately (no sink reconnect); parameter changes that keep the codec use SUSPEND/RECONFIGURE/START. Existing CLI status/codec preference remains the interim test path.
4. Multi-rate ACX: 44.1/48 kHz and selected mono, matching the negotiated format or resampling; last to protect the validated audio path.
5. M6: finish UI and clean install/uninstall restoring `BthA2dp.sys`.
