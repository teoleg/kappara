# DLPI datalink access, userland dhcpagent, and the ARP module

Status: implemented (stages 1-5 landed); lease renewal and a
real route table remain future work.

## Why

Both Ethernet drivers (`uts/virt/ena.c`, `uts/virt/virtio_net.c`)
grew a private embedded DHCP client during AWS bring-up: ~300
duplicated lines each of packet building, option parsing, and state
machine, running *below* the STREAMS stack by spin-polling the RX
ring.  It worked, but it is the opposite of the SVR4 shape this
kernel aims for.  ARP is in the same state: each driver caches
exactly one MAC (the gateway's) in a private field, invisible and
unaged.

The Solaris lineage does it like this:

- The **driver** is a dumb DLPI datalink provider.  It moves
  Ethernet frames and answers DLPI primitives.  No IP, no DHCP.
- **dhcpagent(1M)** is a userland daemon.  It opens the datalink
  device raw, runs DISCOVER/OFFER/REQUEST/ACK itself (encoding its
  own IP/UDP headers), then plumbs the lease onto the interface via
  ifconfig-style ioctls on an IP stream.
- **arp(7P)** is a kernel module paired with ip, owning a real,
  inspectable, aging cache.

That is the target architecture.  This doc records the design and
the staged rollout.

## Target boot flow

```
kmain:
  driver init: rings up, netif registered UNPLUMBED (ip=0),
               datalink cdev /dev/eth0 registered,
               stream wired under IP mux, RX kthread running
  telnetd/ftpd: bind on 0.0.0.0 -- dormant until IP arrives
init (tty0):
  spawn /usr/bin/dhcpagent
    open /dev/eth0            (raw datalink, mini-DLPI)
    DL_INFO_REQ  -> DL_INFO_ACK      (MAC, MTU)
    DL_BIND_REQ(sap=0x0800) -> DL_BIND_ACK
    DISCOVER -> OFFER -> REQUEST -> ACK   (frames built in userland)
    ioctl(/dev/udp): SIOCSIFADDR, SIOCSIFNETMASK, SIOCSIFGW
    exit (lease renewal: future work -- see Limitations)
```

## Components

### 1. ARP module -- `uts/os/net/arp.c` + `/proc/arp`

A single shared cache (16 entries) replacing each driver's
one-entry `arp_gw_mac`:

```
struct arp_entry { uint32_t ip; uint8_t mac[6]; const char *ifname;
                   uint64_t stamp_ticks; int state; };
```

- `arp_ifattach(struct arpif *)` -- driver registers {netif, mac,
  tx(frame), cookie} at init.
- `arp_input(arpif, frame, len)` -- driver RX path hands every ARP
  frame here.  REQUEST for our IP -> reply.  REPLY (and gratuitous
  REQUEST) -> cache insert/update.  A MAC *change* for a cached IP
  is logged (`arp: 172.31.0.1 moved a:b:c -> d:e:f`) -- visibility
  when a VPC router fails over.
- `arp_resolve(arpif, ip, mac_out)` -- cache hit returns 1; miss
  broadcasts a REQUEST and returns 0 (caller drops the packet;
  retransmission is the upper layer's problem, same as classic BSD).
- Entries age: older than 300 s -> re-request on next resolve.
  `/proc/arp` shows `IP  MAC  IF  AGE  STATE`.

Drivers choose the next hop per packet: destination on-subnet ->
ARP the destination itself; off-subnet -> ARP `nif->gateway`.
(Previously every frame went to the gateway MAC unconditionally.)

### 2. Netif plumbing -- SIOCSIF* ioctls

`struct netif` gains `gateway` and `flags` (IFF_UP).  New ioctls,
handled by the IP multiplexor's wput (M_IOCTL -> M_IOCACK), carried
over any stream with ip at the bottom (/dev/udp is the canonical
choice, matching Solaris):

```
struct kifreq { char ifr_name[8]; uint32_t ifr_addr; };
SIOCGIFADDR / SIOCSIFADDR       address
SIOCGIFNETMASK / SIOCSIFNETMASK netmask
SIOCGIFGW / SIOCSIFGW           default gateway (kappara-local; SVR4
                                would use SIOCADDRT -- we don't have
                                a route table yet, one gateway per
                                netif is honest about what exists)
```

`ip_route()` falls back to the highest-prefix netif with a non-zero
gateway when no subnet matches -- the **default route**.  (Until
now, off-subnet destinations were unroutable; slirp never noticed
because everything is 10.0.2.x.)

`ifconfig` grows set verbs:
`ifconfig eth0 <ip> netmask <mask> gw <gw>`.

### 3. Raw datalink access -- `uts/os/net/dl.c`, `/dev/eth0`

Shared provider helper so ena and virtio don't duplicate it:

- `dl_register(name, mac, mtu, tx, cookie)` -> registers a cdev and
  `/dev/<name>`; at most a couple of raw streams open per NIC.
- `dl_input(name-handle, frame, len)` -- called from the driver RX
  path for *every* received frame; fans a dup of the full Ethernet
  frame to each open raw stream whose bound SAP matches (0 = all).
- Mini-DLPI over M_PROTO, real SVR4 primitive numbers:
  `DL_INFO_REQ/ACK` (returns MAC + MTU), `DL_BIND_REQ/ACK`
  (SAP = ethertype filter), `DL_ERROR_ACK`.  Data path is raw
  M_DATA in both directions carrying complete Ethernet frames --
  the DLIOCRAW convention rather than DL_UNITDATA envelopes.
  Divergence from full DLPI is deliberate: no attach/detach (one
  PPA per device), no promiscuity levels, no multicast primitives.

### 4. dhcpagent -- `cmd/dhcpagent.c`

Userland DHCP client, structurally a port of the (now deleted)
driver state machines:

- putmsg/getmsg for the DLPI handshake, read/write for frames.
- Builds/parses full Ethernet+IP+UDP+BOOTP frames itself (the
  Solaris dhcpagent does exactly this via dlpi + pfmod).
- Accepts compact real-server replies (>= 240 B of BOOTP past UDP,
  options bounded by frame length) -- lesson learned on EC2.
- On ACK: plumbs via SIOCSIF* and logs the lease.
- After 3 timeouts: falls back to 10.0.2.15/24 gw 10.0.2.2 (QEMU
  slirp's static topology) so dev boots still get a network.
- Spawned by init from tty0 (same pattern as ftpd), before ftpd.

### 5. Drivers after the diet

ena.c / virtio_net.c keep: ring management, cache maintenance,
STREAMS personality (IP payloads up/down), `dl_register` +
`dl_input` calls, `arp_ifattach` + `arp_input`/`arp_resolve`
calls.  They lose: DHCP structs/state machines/filters, private
gateway MAC fields, the DHCP-gated init ordering (netif +
IP wire-up now happen unconditionally at init).

## Limitations / future work

- **I_SRDTMO**: dhcpagent needs read timeouts to retransmit and
  kappara has no poll(2).  `ioctl(fd, I_SRDTMO, ms)` arms a
  stream-head read timeout (0 restores block-forever); the timed
  wait parks on the scheduler tick queue and re-checks each 10 ms
  tick.  Kappara-local divergence, retired when poll lands.
- **kfs 11/26 dirents**: shipping dhcpagent pushed /usr/bin past
  the 25-entries-per-block cap; the dirent type byte folded into
  start_block bit 31 (19 B entries, 26 per block) and KFS_MAGIC
  bumped to 0x024b4653 -- old /home images reformat on first
  mount rather than misparse.
- **Lease renewal**: dhcpagent exits after plumbing; no T1/T2
  timers.  Acceptable on EC2 (a VPC private IP is bound to the ENI
  for the instance lifetime) and slirp (static).  Needs a
  sleep/alarm primitive to do properly.
- **Route table**: one gateway per netif instead of SIOCADDRT and a
  real table.  Revisit when a second Ethernet-class interface
  exists.
- **arp as a STREAMS module**: the cache is a C-API module, not an
  autopushed `arp` module between ip and the datalink.  The
  interface boundary (arpif ops vector) is shaped so it can become
  one without touching drivers again.

## Rollout stages (each builds + passes `make test`)

1. arp.c + /proc/arp, drivers converted to it.
2. netif gateway/flags + SIOCSIF* + default route + ifconfig verbs.
3. dl.c + /dev/eth0 mini-DLPI.
4. cmd/dhcpagent.c + init spawn; DHCP deleted from both drivers.
5. Docs sweep (ARCHITECTURE/PROCFS/SHELL/AWS/README) + AWS verify.
