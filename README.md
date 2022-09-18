# samsungipcd

__This is an early prototype. Pretty much nothing works yet.__

This is a proxy that provides standard AT command interface to Samsung modems using libsamsung-ipc. Can be used with ModemManager or oFono as clients.

Currently only receiving SMS messages works.

## Building

```
gcc server.c samsung-ipc.c -o samsungipcd $(pkg-config --cflags --libs samsung-ipc)
```

Or to build the simulator (for testing without real hardware):

```
gcc server.c phonesim.c -o samsungipcd
```

## Usage

1. Enable BSD pseudoterminals (`CONFIG_LEGACY_PTYS=y`)
2. Launch it like this: `sudo samsungipcd -f /dev/ptywc` (note: the master end)
3. Write a udev rule like this: `KERNEL=="ttywc", ENV{ID_MM_DEVICE_PROCESS}="1"` (note: the slave end)
4. Start ModemManager. It should now detect the proxy as a modem.
