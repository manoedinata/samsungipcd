# samsungipcd

__This is an early prototype.__

This is a proxy that provides standard AT command interface to Samsung modems using libsamsung-ipc. Can be used with ModemManager or oFono as clients.

This copy is a fork of [sleirsgoevy/samsungipcd](https://gitlab.com/sleirsgoevy/samsungipcd) that adds support for Shannon (LTE) modems, tested on the Samsung Galaxy J4 (SM-J400F) running Debian. See [Shannon modems](#shannon-lte-modems-galaxy-j4) below.

## Works

* Mobile data
  * Verified on a Galaxy J4 (Shannon, LTE) with ModemManager + NetworkManager, see below. Needs the Shannon changes in libsamsung-ipc, see [Building](#building)
* Sending and receiving SMS (upstream; not re-tested on the J4)
* Voice calls (upstream; not re-tested on the J4)

## Unimplemented

* Call volume control
* Putting calls on hold, having multiple simultaneous calls
* Automatic detection of GPRS connection details (Does libsamsung-ipc support that at all? Needs research)
* USSD codes (pseudo-numbers with textual answers)
* SIM unlocking
* IPv6 on Shannon modems (the modem side has not been tried)
* A real SIM state: `AT+CPIN?` and `AT+CIMI` are stubs that always say "ready" and return a fake IMSI

## Building

```
gcc server.c samsung-ipc.c -o samsungipcd $(pkg-config --cflags --libs samsung-ipc)
```

Or to build the simulator (for testing without real hardware):

```
gcc server.c phonesim.c -o samsungipcd
```

`make` builds the daemon with the right flags (`PREFIX` defaults to `/usr`).

For Shannon modems it needs a libsamsung-ipc with the messages added in
[manoedinata/libsamsung-ipc, branch `shannon-lte-data`](https://github.com/manoedinata/libsamsung-ipc/tree/shannon-lte-data)
(`IPC_NET_SERVICE_DOMAIN_CONFIG`, `IPC_GPRS_LTE_ATTACH_APN_INFO` and friends). The upstream
library only has the older, Intel-style messages.

## Usage

The daemon is given one path per AT port. A plain path (a pty master, or a driver's
master node) is opened as it is. A path starting with `cuse:` makes the daemon create the
device itself through CUSE.

### Recommended: ModemManager plugin, data over `rmnet0` (no PPP)

This is the way regular modems with a network interface work, and the one used on the J4.
A [ModemManager plugin](https://github.com/manoedinata/debian_j4lte/tree/master/mm-plugin-samsungipcd)
makes ModemManager activate the data session with `AT+CGACT`, read the address and DNS with
`AT+CGCONTRDP` and report them as a static IP configuration on the modem's network interface
(`rmnet0`). NetworkManager then puts that on `rmnet0`; there is no PPP, no `ppp0`, and no packet
passes through this daemon. ModemManager shows one modem with the ports `rmnet0 (net)`,
`ttyGSM0 (at)` and `ttyGSM1 (at)`, and `mmcli -b` reports method `static`. It needs the kernel
`ttygsm` driver described next for the AT ports, and a udev rule that ties the ports to the plugin.

### AT ports as real tty devices: virtual tty ports from the kernel (Linux with `CONFIG_TTY_GSM_VIRTUAL`)

ModemManager only watches the `tty`, `net` and `usb` udev subsystems, so the ports must be
real tty devices. Neither a pty nor a CUSE device is. The
[`ttygsm` driver](https://github.com/DaunScape/android_kernel_samsung_universal7570/blob/postmarketos-v2/drivers/tty/ttygsm.c)
(kernel option `CONFIG_TTY_GSM_VIRTUAL`) registers `/dev/ttyGSM0` and `/dev/ttyGSM1` as tty
devices, plus a master node for each (`/dev/ttygsm-master0`, `/dev/ttygsm-master1`): what the
tty writes can be read from the master and the other way around, like a pty. The daemon uses
the master nodes:

```
samsungipcd /dev/ttygsm-master0 /dev/ttygsm-master1
```

A udev rule makes ModemManager treat them as one modem, with `ttyGSM0` as the AT port and
`ttyGSM1` as the PPP data port:

```
ACTION!="add|change|move|bind", GOTO="mm_samsungipcd_end"
SUBSYSTEM!="tty", GOTO="mm_samsungipcd_end"
KERNEL=="ttyGSM[0-9]*", ENV{ID_MM_PHYSDEV_UID}="samsungipcd"
KERNEL=="ttyGSM0", ENV{ID_MM_PORT_TYPE_AT_PRIMARY}="1"
KERNEL=="ttyGSM1", ENV{ID_MM_PORT_TYPE_AT_PPP}="1"
LABEL="mm_samsungipcd_end"
```

Start the daemon before ModemManager (ModemManager probes the ports once when it sees them).
Then connect with `mmcli` or, as a normal mobile broadband connection, with NetworkManager
(`nmcli connection add type gsm ifname '*' con-name "Mobile data" gsm.apn <apn>`).

With the plugin above there is no PPP. Without it, data goes through PPP on the data port. ModemManager only dials (`ATD`) and NetworkManager
runs pppd on the port, which needs a kernel with `CONFIG_PPP_ASYNC` (the `N_PPP` line
discipline) so that pppd can create `ppp0`. The daemon answers LCP, PAP and IPCP itself and
hands the packets to the modem's `rmnet0` interface. Note that IPv4 packets from the modem
come back through `rmnet0` straight to the host, not through `ppp0`, so `ppp0`'s RX counters
stay near zero and a socket must not be bound to `ppp0`.

### Without a kernel driver

1. With `CONFIG_LEGACY_PTYS=y`: launch it like this: `sudo samsungipcd /dev/ptywc /dev/ptywd` (note: the master end)
2. Write a udev rule like this: `KERNEL=="ttywc", ENV{ID_MM_DEVICE_PROCESS}="1"` (note: the slave end, same rule for ttywd)
3. Start ModemManager. It should now detect the proxy as a modem.

Or let the daemon allocate the ports with CUSE (`CONFIG_CUSE`): `samsungipcd cuse:/dev/ttyGSM0 cuse:/dev/ttyGSM1`.
The ports are a pty bind-mounted over the CUSE node, which udev and ModemManager do not see, so
this works for talking to the AT port directly and for a plain pppd, not for ModemManager.

### Packet data without PPP (`AT+CGACT`)

Ordinary modems with a separate network interface do not use PPP: the host activates the
PDP context with AT commands, reads the address back and configures the modem's network
interface itself. samsungipcd supports this too, for the modem's own interface (`rmnet0` on the J4):

```
AT+CGDCONT=1,"IP","<apn>"
AT+CGACT=1,1         OK once the session is up (ERROR after 25 s); AT+CGACT=0,1 deactivates
AT+CGPADDR           +CGPADDR: 1,"10.81.183.131"
AT+CGCONTRDP         +CGCONTRDP: 1,5,"<apn>","10.81.183.131.255.255.255.255","","<dns1>","<dns2>"
```

Then give `rmnet0` that address as a `/32` and route over it; the kernel's modem driver carries
the packets, nothing passes through this daemon and there is no `ppp0`. The modem gives no
gateway and no netmask, it is a point-to-point link. A dropped session is announced with
`+CGEV: NW PDN DEACT 1`. Only IPv4 and context 1.

The first activation after the daemon starts usually fails with `ERROR`: sending the LTE attach
profile makes the modem re-register for a few seconds and the request in flight is lost. Repeat it.

### Plain pppd

Without ModemManager, dial with chat and run pppd on the port, for example
`chat ... AT+CGDCONT=1,"IP","<apn>"  ATD*99***1#` and then `pppd /dev/ttyGSM0 noauth noipdefault ipcp-accept-local ipcp-accept-remote usepeerdns nodefaultroute`.
The first command after boot, or after a pppd session, can come back as `ERROR` because of stale
bytes on the line; send `AT` until it answers `OK` before dialing. pppd assigns the address to `ppp0` but
sets no route (the peer address equals the local one), so add routes yourself.

On kernels without `CONFIG_PPP_ASYNC` there is `ppp-hook.so`, a pppd plugin that does the PPP framing in
userspace (`plugin /usr/share/samsungipcd/ppp-hook.so`). With pppd 2.4.9 it cannot create the `ppp0`
interface, so the address has to be taken from pppd's log and put on `rmnet0` by hand.

## Shannon modems (Galaxy J4)

The J4's Shannon modem is booted by libsamsung-ipc from the RADIO partition using its `herolte`
profile (build libsamsung-ipc with `-DIPC_DEVICE_NAME='"herolte"'`, since arm64 kernels have no
`Hardware:` line in `/proc/cpuinfo` for it to detect the board with). Packet data does not come up
with the old message sequence. Verified with a SIM that has an active data plan (Telkomsel):

* The modem boots with circuit switched service only. After `SMS_DEVICE_READY` the daemon sets the
  service domain to circuit + packet (`NET_SERVICE_DOMAIN_CONFIG` 0x0808). Both this and the
  next step are required, each alone breaks it.
* Before the PDP context is activated it sends the LTE attach profile with the APN
  (`GPRS_LTE_ATTACH_APN_INFO` 0x0D14). The modem then re-registers for a few seconds, and a data
  request that arrives in the middle fails. A repeated enable restarts the sequence, so a redial
  works; ModemManager/NetworkManager retry on their own.
* A data request that comes before the modem is ready is held until it is.
* The IP configuration only needs the IP address; the second DNS server can be missing.

The layouts were read from the stock `libsec-ril.so` and checked against the modem. The names of
the service domain values and the meaning of some attach profile fields are inferred, see the
comments in libsamsung-ipc's `include/net.h` and `include/gprs.h`.

Registration is reported separately for the circuit and the packet domain (`+CREG`, `+CGREG`) and,
on LTE, as `+CEREG`, so ModemManager shows `home` instead of `home-sms-only`. That the modem's
registration status 7 means "registered" for the packet domain is an inference, see `map_registration_status()`
in `samsung-ipc.c`.

Known limitations on the J4: the SIM is only detected in slot 2; IPv6, other carriers and an
inactive data plan have not been tried.
