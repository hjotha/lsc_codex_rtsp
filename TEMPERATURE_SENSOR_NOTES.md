# Temperature Sensor Notes

This note records the investigation of the temperature-related functions found
in the stock `anyka_ipc` binaries.

## Summary

The temperature functions are not reading an internal CPU thermal sensor.

The stock firmware contains a baby-monitor temperature path that expects an
external ambient sensor exposed as:

```text
/dev/gx18b20
```

On the two currently tested cameras, that device node and matching kernel driver
are not present, so the temperature path is inactive and the live value remains
zero.

## Firmware Functions

Known function addresses:

| Firmware | Function | Address | Behavior |
|---|---|---:|---|
| `V3.2863.105` | `ht_tuya_get_temperature` | `0x00085a94` | copies the cached temperature field into the pointer passed in `r0` |
| `V3.2863.105` | `IPC_APP_report_temp` | `0x00096b3c` | reports DP `142` with the cached temperature value |
| `V3.2863.93` | `ht_tuya_get_temperature` | `0x000a333c` | copies the cached temperature field into the pointer passed in `r0` |
| `V3.2863.93` | `IPC_APP_report_temp` | `0x000b33fc` | reports the cached temperature and handles min/max alarm settings |

`ht_tuya_get_temperature` does not perform IO. It reads the baby-monitor global
state and copies the word at offset `+0x14` into the caller-provided pointer,
then returns `0`.

Known global state symbols:

| Firmware | Symbol | Address | Temperature field |
|---|---|---:|---:|
| `V3.2863.105` | `g_ht_tuya_baby_monitor` | `0x005e2024` | `0x005e2038` |
| `V3.2863.93` | `g_ht_tuya_baby_monitor` | `0x0057547c` | `0x00575490` |

## Sensor Thread Behavior

The updater thread is `ht_tuya_baby_monitor_upload_temp_th`.

It opens `/dev/gx18b20` and reads 4-byte integer samples. The accepted range is:

```text
-500..500
```

The firmware formats values as `%d.%d`, so the unit is tenths of a degree
Celsius. For example:

```text
245 = 24.5 C
```

The thread averages samples and ignores jumps greater than `50`, which is a
`5.0 C` jump in that scale. That filtering behavior matches an ambient
temperature sensor path, not a CPU thermal path.

Related strings in the binary:

```text
read temperature is error!
(%d)read temperature:%d.%d, avg temperature:%d.%d
last temp is:%d, current temp is: %d, diff more than 50, do not update.
upload temperature is %d.%d
```

## Live Hardware Check

Checked on `2026-05-18`:

| Camera | IP | Firmware | Result |
|---|---|---|---|
| `sala` | `192.168.1.130` | `V3.2863.105` | `/dev/gx18b20` missing; baby-monitor state all zero |
| `quintal` | `192.168.1.165` | `V3.2863.93` | `/dev/gx18b20` missing; baby-monitor state all zero |

Additional checks on both cameras:

- `/proc/devices` listed `i2c`, `usb`, and `usb_device`, but no `gx18b20`,
  `w1`, `thermal`, or temperature driver
- `/sys/class` did not expose `thermal`, `hwmon`, `w1`, or temperature nodes
- loaded modules included `ak_i2c` and `ak_saradc`, but no temperature sensor
  module
- `dmesg` had audio ADC and image sensor messages, but no `gx18b20` or thermal
  probe

## Conclusion

The code path appears to be compiled in for another hardware variant, likely a
baby-monitor model with an external ambient 18B20-style sensor.

For the currently tested LSC cameras, these functions do not provide the room
temperature and do not provide CPU temperature. The cached value is inactive
unless a compatible `/dev/gx18b20` driver and sensor are present.
