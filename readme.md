# ServerStartup

Run these shell commands in Ubuntu/Linux from the `ServerStartup` folder. Use separate terminals for the server, Peer 01, and Peer 02.

Requires GCC and OpenSSL development files (`build-essential` and `libssl-dev` on Ubuntu).

## Compile & Start Server

```bash
mkdir -p torrents
gcc -pthread -o tracker tracker.c worker.c tracker_files.c file_hash.c -lcrypto && ./tracker
```

## Compile Peer (once, before starting either peer)

```bash
gcc -pthread -o peer peer.c peer_download.c peer_upload.c file_hash.c -lcrypto
```

## Peer 01

From `ServerStartup` in its own terminal:

```bash
cd peer01_config && ../peer
```

Uses port `6001` and shared folder `Peer01`.

## Peer 02

From `ServerStartup` in its own terminal:

```bash
cd Peer02_Config && ../peer
```

Uses port `6002` and shared folder `Peer02`.

## Tracker Commands

Enter these at the peer's `Enter a tracker request` prompt. Replace the placeholders with actual values. Commands are case-sensitive; use underscores instead of spaces in descriptions.

### Create Tracker

Register a shared file and its seed's listening address.

```text
<createtracker filename filesize description md5 peer_ip peer_port>
```

`filesize` is in bytes. `md5` is the file's 32-character hexadecimal checksum. `peer_ip` and `peer_port` identify the peer serving the file (`6001` for Peer 01 or `6002` for Peer 02).

### Update Tracker

Add or update a peer's available byte range.

```text
<updatetracker filename byte_start byte_end peer_ip peer_port>
```

`byte_start` and `byte_end` are nonnegative byte offsets, with `byte_start <= byte_end`.

### Request File List

List the files registered with the tracker.

```text
<REQ LIST>
```

### Get Tracker

Request a `.track` file. The peer verifies it, then automatically downloads the shared file from a listed complete seed into its shared folder.

```text
<GET filename.track>
```

Type `quit` at the peer prompt to stop a peer. Press `Ctrl+C` in the server terminal to stop the tracker.
