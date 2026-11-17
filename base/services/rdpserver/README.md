# LiberNT RDP console server

`rdpserver.exe` mirrors the interactive console desktop over RDP for a test
LAN. FreeRDP implements the wire protocol and TLS; the server captures the
desktop with GDI and injects keyboard and mouse input with `SendInput` once a
client has authenticated. It does not implement Terminal Services or create
remote sessions. The RDP drive redirection channel also carries authenticated
file transfers to a configured local directory.

## Building

The server is built outside the LiberNT toolchain, with an x86_64 MinGW
compiler for 64-bit images or an i686 one for i386 images, against FreeRDP
3.32.0 and OpenSSL 3 built with the same compiler:

1. Apply `freerdp-rdpdr.patch` to FreeRDP 3.32.0 and build and install it
   with MinGW (`WITH_SERVER=ON`, `WITH_CHANNELS=ON`, `WITH_SERVER_CHANNELS=ON`
   and the `rdpdr` channel; the client, shadow and proxy parts are not
   needed). The patch keeps the 64-bit offsets of RDPDR read and write
   requests and handles a write acknowledgement as a length without a data
   payload ([MS-RDPEFS]).
2. Configure and build `mingw/CMakeLists.txt` against that installation and
   install it to the same prefix.
3. Add the OpenSSL and MinGW runtime DLLs next to the server, so that the
   install directory holds:

   ```
   bin/rdpserver.exe
   bin/libfreerdp3.dll  bin/libfreerdp-server3.dll  bin/libwinpr3.dll
   bin/libssl-3-x64.dll  bin/libcrypto-3-x64.dll
   bin/libgcc_s_seh-1.dll  bin/libwinpthread-1.dll
   bin/ossl-modules/legacy.dll
   licenses/FreeRDP-LICENSE  licenses/OpenSSL-LICENSE
   ```

   An i686 build holds `bin/libssl-3.dll`, `bin/libcrypto-3.dll` and
   `bin/libgcc_s_dw2-1.dll` instead of the x64 OpenSSL and GCC runtime
   DLLs. Use a compiler with win32 threads (for example Ubuntu's
   `i686-w64-mingw32-gcc-win32`), which needs no `libwinpthread-1.dll`.
   When cross-compiling FreeRDP for i686, also pass
   `-DCMAKE_C_FLAGS=-D__STDC_NO_THREADS__=1`, `-DUSE_UNWIND=OFF` and
   `-DHAVE_SSIZE_T=0 -DSSIZE_T= -DHAVE_WIN_SSIZE_T=1 -DWIN_SSIZE_T=4`: the
   `ssize_t` and `BaseTsd.h` checks give a wrong `SSIZE_T` on a
   case-sensitive host.

Point a build directory of the matching architecture at it (arm64 and
riscv64 run the server as x64 code) (or set *System > RDP console server
build directory* in rosconfig):

```sh
cmake -DRDPSERVER_DIR=/path/to/install .
```

The live and preinstalled images then carry `%SystemRoot%\system32\rdpserver`
and an HKCU `Run` value that starts the server with the interactive desktop.
`RDPSERVER_DIR` is empty by default, which leaves the server out.

## Configuration

The in-tree `rdpserver.ini` sets `enabled=0`, so the server exits at once.
To enable it, keep a private directory outside the source tree with an
`rdpserver.ini` and a TLS certificate/key pair, and point the build at it
with `-DRDPSERVER_CONFIG_DIR=/path/to/private/rdpserver`. Its
`rdpserver.ini`, `*.crt`, `*.key` and `*.pem` files replace the in-tree
configuration in the images:

```ini
[server]
enabled=1
bind=10.42.0.2
allow=10.42.0.1
username=ros-test
password=REPLACE_WITH_A_RANDOM_TEST_PASSWORD
certificate=server.crt
privatekey=server.key
interface_mac=02:de:ad:10:ec:68,20:46:a1:08:d0:53
netmask=255.255.255.0
transfer_root=C:\
```

- `bind` and `allow` are explicit IPv4 addresses. The server accepts one
  controller, from exactly the `allow` address, over TLS with these
  credentials; NLA is not enabled. Use a random password of at least 16
  characters and pin the certificate fingerprint in the client.
- A relative `certificate` or `privatekey` path names a file next to the
  configuration, so the same configuration works on the live image (`X:`)
  and on an installed disk.
- `interface_mac` (a comma-separated list of up to eight addresses) and
  `netmask` are optional. With them, the server waits for a network adapter
  with one of these MAC addresses and adds `bind` to it with `AddIPAddress`,
  keeping its other addresses, and removes that address again on a clean
  exit. Without them, an adapter must already have the `bind` address. The
  server waits for PnP and the network stack instead of failing when the
  address is not ready yet.
- `transfer_root` enables file transfers below that local directory after
  authentication. Leave it out to disable transfers.
- `clipboard=0` turns clipboard redirection off; it is on by default.
- `loopback=1` also listens on 127.0.0.1:3389 for `hvsockbridge`, which
  carries the enhanced session of Hyper-V Virtual Machine Connection (see
  below). `hvsock_trust=1` lets those sessions in without credentials.
- `console=1` keeps the server's console window visible. By default the
  server hides it: on the interactive desktop it could take the keyboard
  focus from the application a client types into, and a Ctrl+C typed into
  it would stop the server.

Authentication, the first frame and disconnects are logged as `RDPTEST`
lines on the kernel debugger and on stdout. An idle server captures nothing;
a connected session captures at most ten frames per second and sends the
changed 64x32 tiles, planar (RDP 6.0) compressed.

## Clipboard

While a client is connected, the `cliprdr` channel redirects the console
clipboard in both directions. Text (`CF_UNICODETEXT`, `CF_TEXT`,
`CF_OEMTEXT` and `CF_LOCALE`), device-independent bitmaps (`CF_DIB`,
`CF_DIBV5`) and registered memory formats such as `HTML Format` travel; GDI
handle formats and file copies do not. A copy on the client makes the server
the owner of the console clipboard with delayed rendering, so the data
crosses the channel only when an application pastes it. A copy in the VM is
announced to the client, which then fetches the formats it needs. Clipboard
events are logged on the kernel debugger only: console output from the
clipboard window can block while the console itself pastes.

## Enhanced session (Hyper-V)

Virtual Machine Connection opens its enhanced session to a guest through a
Hyper-V socket on the RDP service GUID, as for Linux guests with xrdp.
vmbus.sys accepts it on `\\.\HvSocket`, and the `hvsockbridge` service,
installed with the server, forwards each connection to 127.0.0.1:3389. The
bridge listens only while the server answers there, because the host offers
the enhanced session as soon as the guest accepts its probe. Set the VM's
transport on the host with
`Set-VM <vm> -EnhancedSessionTransportType HvSocket`.

With `loopback=1` the server takes loopback connections only from the
bridge: it looks up the process that owns the client end of the TCP
connection and compares it with the `hvsockbridge` service process. Only
that peer gets standard RDP security without encryption, which is what
Virtual Machine Connection uses here; the Hyper-V socket never leaves the
host. Network clients keep TLS and the configured credentials.

Virtual Machine Connection sends no guest credentials. `hvsock_trust=1`
(off by default) trusts the bridge instead: only the Hyper-V host can open
the channel, so, like the basic console of Virtual Machine Connection, the
session is open to whoever may connect to the VM on the host. Without it
the enhanced session is refused. The session mirrors the console at its
current resolution.

One session drives the console at a time, and a new connection replaces it,
as on a Windows console. The replaced client is told that another connection
took over, so it does not reconnect on its own. The new session starts once
the old one has released its keys. Virtual Machine Connection opens the
enhanced session with a negotiation that fails on purpose and reconnects at
once, so that reconnect must not be refused while the first connection is
still closing.

While another desktop has the input, such as the Winlogon desktop after
Ctrl+Alt+Del or a locked console, input cannot be injected. The session stays
connected and shows the screen, but drops input until the user's desktop is
back (close that screen from the basic session). It then releases any keys
whose release was lost.

## Client and file transfer

Connect with a FreeRDP 3 client: `/sec:tls`, the configured user name and
password (prefer `/args-from:stdin` over the command line) and
`/cert:fingerprint:sha256:<fingerprint of the certificate>`. For file
transfers, also redirect a dedicated host directory as the `ROSXFER` drive
(`/drive:ROSXFER,<directory>`).

The server polls `request.txt` in that share. A request is UTF-8 text with
LF line ends: a header `RDPFILES1<TAB><job id of 32 hex digits>`, then one
line per item, `<operation><TAB><size><TAB><sha256><TAB><source><TAB><destination>`:

- `PUT`: copy the share file `<source>` to `<destination>` below
  `transfer_root`, checking `<size>` and the SHA-256. A destination that
  already has that size and hash is skipped. The copy goes to a temporary
  name that is verified, flushed and renamed over the destination, so an
  interrupted upload keeps the previous file.
- `GET`: copy `<source>` below `transfer_root` to the share file
  `<destination>`; the size field must be a number and the hash field is
  not checked.
- `MKDIR`: create the directory `<destination>`.

The server writes `results/<job id>.txt` to the share: `RDPRESULT1<TAB><job>`,
one `<index><TAB><status><TAB><bytes><TAB><sha256><TAB>COPY|SKIP` line per
item, and `END<TAB><job>`. A job runs once; after a reconnect the same job
restarts and skips the files already verified.

Paths must be relative, without traversal, alternate data streams, device
names or reparse points, and fit in `MAX_PATH` with room for the temporary
suffix. One job holds at most 16,384 items and a 4 MiB request. Transfers do
not keep ACLs or timestamps.

FreeRDP 3.32.0's client `channels/drive/client/drive_file.c` passes the wrong
handle to `GetFileSize`: reads run past the end of the file and the server
rejects the request. Use a client with that call fixed.
