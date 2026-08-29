#!/usr/bin/env python3
"""
Papaya Net (P2P Multiplayer LAN Tunnel)
Routes Goldberg Emulator LAN broadcast packets across the internet via lightweight
peer-to-peer UDP tunnels using 6-digit room codes, restoring multiplayer for DRM-free games.

Architecture:
  Goldberg -> LAN Broadcast (47584/UDP) -> PapayaNet Daemon
       |                                        |
  Virtual Subnet (10.99.x.x/24)    <->   Room Peers (raw UDP)

Usage:
  python3 papaya_net.py host --port 12340
  python3 papaya_net.py join --room ABCD12 --host 1.2.3.4 --port 12340
"""

import hashlib
import json
import os
import random
import select
import socket
import struct
import sys
import threading
import time
import argparse
from typing import Dict, Any, Optional, List, Tuple

GOLDBERG_BROADCAST_PORT = 47584
PAPAYA_NET_DEFAULT_PORT = 12340
HEARTBEAT_INTERVAL_S    = 5.0
PEER_TIMEOUT_S          = 30.0
BUFFER_SIZE             = 65536

PKT_DATA      = 0x01
PKT_HEARTBEAT = 0x02
PKT_JOIN      = 0x03
PKT_LEAVE     = 0x04
PKT_ACK       = 0x05


class PapayaNetRoom:
    def __init__(self, room_code: str):
        if not room_code or len(room_code) < 4:
            raise ValueError("Room code must be at least 4 characters")
        self.room_code = room_code.upper()
        self.subnet_ip = self._compute_virtual_ip(room_code)
        self.is_connected = False

    def _compute_virtual_ip(self, code: str) -> str:
        h = int(hashlib.sha256(code.upper().encode()).hexdigest(), 16)
        octet3 = (h >> 8) % 254 + 1
        octet4 = h % 254 + 1
        return f"10.99.{octet3}.{octet4}"

    def connect(self) -> bool:
        print(f"[PAPAYA_NET] Joining P2P Room #{self.room_code} -> Virtual IP {self.subnet_ip}/24")
        self.is_connected = True
        return True

    def disconnect(self):
        if self.is_connected:
            print(f"[PAPAYA_NET] Disconnected from P2P Room #{self.room_code}")
            self.is_connected = False

    def generate_goldberg_broadcast_config(self) -> Dict[str, Any]:
        return {
            "room_code": self.room_code,
            "virtual_ip": self.subnet_ip,
            "custom_broadcasts": [
                f"10.99.255.255:{GOLDBERG_BROADCAST_PORT}",
                f"127.0.0.1:{GOLDBERG_BROADCAST_PORT}",
                f"255.255.255.255:{GOLDBERG_BROADCAST_PORT}"
            ]
        }

    def write_goldberg_config(self, game_dir: str) -> str:
        import pathlib
        settings_dir = pathlib.Path(game_dir) / "steam_settings"
        settings_dir.mkdir(parents=True, exist_ok=True)
        cfg = self.generate_goldberg_broadcast_config()
        broadcasts_file = settings_dir / "custom_broadcasts.txt"
        with open(broadcasts_file, "w", encoding="utf-8") as f:
            for addr in cfg["custom_broadcasts"]:
                f.write(addr + "\n")
        print(f"[PAPAYA_NET] Written Goldberg broadcast config -> {broadcasts_file}")
        return str(broadcasts_file)


def _build_packet(pkt_type: int, room_code: str, payload: bytes = b"") -> bytes:
    room_bytes = room_code.upper().encode()[:6].ljust(6, b"\x00")
    header = struct.pack("!B6sI", pkt_type, room_bytes, len(payload))
    return header + payload


def _parse_packet(data: bytes) -> Optional[Tuple[int, str, bytes]]:
    if len(data) < 11:
        return None
    pkt_type, room_bytes, payload_len = struct.unpack_from("!B6sI", data, 0)
    room_code = room_bytes.rstrip(b"\x00").decode(errors="replace")
    payload = data[11:11 + payload_len]
    return pkt_type, room_code, payload


class PapayaNetHost:
    def __init__(self, room_code: str, bind_port: int = PAPAYA_NET_DEFAULT_PORT):
        self.room = PapayaNetRoom(room_code)
        self.bind_port = bind_port
        self.peers: Dict[Tuple[str, int], float] = {}
        self._lock = threading.Lock()
        self._running = False
        self._sock: Optional[socket.socket] = None

    def start(self):
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("0.0.0.0", self.bind_port))
        self._running = True
        print(f"[HOST] Papaya Net Host for Room #{self.room.room_code} listening on UDP:{self.bind_port}")
        print(f"[HOST] Virtual subnet: {self.room.subnet_ip.rsplit('.', 1)[0]}.0/24")
        threading.Thread(target=self._heartbeat_loop, daemon=True).start()
        threading.Thread(target=self._goldberg_listener, daemon=True).start()
        self._recv_loop()

    def _heartbeat_loop(self):
        while self._running:
            now = time.monotonic()
            pkt = _build_packet(PKT_HEARTBEAT, self.room.room_code)
            with self._lock:
                dead = [a for a, t in self.peers.items() if now - t > PEER_TIMEOUT_S]
                for a in dead:
                    print(f"[HOST] Peer {a} timed out, removing")
                    del self.peers[a]
                live = list(self.peers.keys())
            for addr in live:
                try:
                    self._sock.sendto(pkt, addr)
                except OSError:
                    pass
            time.sleep(HEARTBEAT_INTERVAL_S)

    def _goldberg_listener(self):
        try:
            gb_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            gb_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            gb_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            gb_sock.bind(("0.0.0.0", GOLDBERG_BROADCAST_PORT))
        except OSError as e:
            print(f"[HOST] Could not bind Goldberg port {GOLDBERG_BROADCAST_PORT}: {e}")
            return
        while self._running:
            try:
                readable, _, _ = select.select([gb_sock], [], [], 1.0)
                if not readable:
                    continue
                data, addr = gb_sock.recvfrom(BUFFER_SIZE)
                wrapped = _build_packet(PKT_DATA, self.room.room_code, data)
                with self._lock:
                    peers = list(self.peers.keys())
                for peer in peers:
                    try:
                        self._sock.sendto(wrapped, peer)
                    except OSError:
                        pass
            except Exception as e:
                if self._running:
                    print(f"[HOST] Goldberg listener error: {e}")

    def _recv_loop(self):
        while self._running:
            try:
                readable, _, _ = select.select([self._sock], [], [], 1.0)
                if not readable:
                    continue
                data, addr = self._sock.recvfrom(BUFFER_SIZE)
                parsed = _parse_packet(data)
                if not parsed:
                    continue
                pkt_type, room_code, payload = parsed
                if room_code != self.room.room_code:
                    continue
                now = time.monotonic()
                with self._lock:
                    if addr not in self.peers:
                        print(f"[HOST] New peer joined: {addr[0]}:{addr[1]}")
                    self.peers[addr] = now
                if pkt_type == PKT_JOIN:
                    ack = _build_packet(PKT_ACK, self.room.room_code)
                    self._sock.sendto(ack, addr)
                elif pkt_type == PKT_DATA:
                    with self._lock:
                        peers = [p for p in self.peers if p != addr]
                    for peer in peers:
                        try:
                            self._sock.sendto(data, peer)
                        except OSError:
                            pass
                elif pkt_type == PKT_LEAVE:
                    with self._lock:
                        self.peers.pop(addr, None)
                    print(f"[HOST] Peer {addr} left room")
            except Exception as e:
                if self._running:
                    print(f"[HOST] recv error: {e}")

    def stop(self):
        self._running = False
        if self._sock:
            self._sock.close()


class PapayaNetClient:
    def __init__(self, room_code: str, host_addr: str, host_port: int = PAPAYA_NET_DEFAULT_PORT):
        self.room = PapayaNetRoom(room_code)
        self.host_addr = (host_addr, host_port)
        self._running = False
        self._sock: Optional[socket.socket] = None
        self._connected = False

    def connect(self) -> bool:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self._running = True
        join_pkt = _build_packet(PKT_JOIN, self.room.room_code)
        self._sock.sendto(join_pkt, self.host_addr)
        self._sock.settimeout(5.0)
        try:
            data, _ = self._sock.recvfrom(BUFFER_SIZE)
            parsed = _parse_packet(data)
            if parsed and parsed[0] == PKT_ACK:
                self._connected = True
                print(f"[CLIENT] Connected to room #{self.room.room_code} via {self.host_addr[0]}:{self.host_addr[1]}")
                self.room.connect()
        except socket.timeout:
            print(f"[CLIENT] Timeout waiting for ACK from host")
            return False
        finally:
            self._sock.settimeout(None)
        if not self._connected:
            return False
        threading.Thread(target=self._heartbeat_loop, daemon=True).start()
        threading.Thread(target=self._goldberg_listener, daemon=True).start()
        threading.Thread(target=self._recv_loop, daemon=True).start()
        return True

    def _heartbeat_loop(self):
        while self._running and self._connected:
            pkt = _build_packet(PKT_HEARTBEAT, self.room.room_code)
            try:
                self._sock.sendto(pkt, self.host_addr)
            except OSError:
                pass
            time.sleep(HEARTBEAT_INTERVAL_S)

    def _goldberg_listener(self):
        try:
            gb_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            gb_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            gb_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            gb_sock.bind(("0.0.0.0", GOLDBERG_BROADCAST_PORT))
        except OSError as e:
            print(f"[CLIENT] Could not bind Goldberg port {GOLDBERG_BROADCAST_PORT}: {e}")
            return
        while self._running and self._connected:
            try:
                readable, _, _ = select.select([gb_sock], [], [], 1.0)
                if not readable:
                    continue
                data, addr = gb_sock.recvfrom(BUFFER_SIZE)
                wrapped = _build_packet(PKT_DATA, self.room.room_code, data)
                self._sock.sendto(wrapped, self.host_addr)
            except Exception as e:
                if self._running:
                    print(f"[CLIENT] Goldberg listener error: {e}")

    def _recv_loop(self):
        try:
            inject_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            inject_sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        except OSError as e:
            print(f"[CLIENT] Could not open injection socket: {e}")
            return
        while self._running and self._connected:
            try:
                readable, _, _ = select.select([self._sock], [], [], 1.0)
                if not readable:
                    continue
                data, addr = self._sock.recvfrom(BUFFER_SIZE)
                parsed = _parse_packet(data)
                if not parsed:
                    continue
                pkt_type, room_code, payload = parsed
                if pkt_type == PKT_DATA and payload:
                    inject_sock.sendto(payload, ("127.0.0.1", GOLDBERG_BROADCAST_PORT))
            except Exception as e:
                if self._running:
                    print(f"[CLIENT] recv error: {e}")

    def disconnect(self):
        if self._connected and self._sock:
            pkt = _build_packet(PKT_LEAVE, self.room.room_code)
            try:
                self._sock.sendto(pkt, self.host_addr)
            except OSError:
                pass
        self._running = False
        self._connected = False
        self.room.disconnect()
        if self._sock:
            self._sock.close()


def generate_room_code() -> str:
    chars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
    return "".join(random.choices(chars, k=6))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Papaya Net - P2P LAN Tunnel for Goldberg Emulator Multiplayer")
    subparsers = parser.add_subparsers(dest="command")

    p_host = subparsers.add_parser("host", help="Host a P2P room relay")
    p_host.add_argument("--room", type=str, default=None)
    p_host.add_argument("--port", type=int, default=PAPAYA_NET_DEFAULT_PORT)

    p_join = subparsers.add_parser("join", help="Join an existing P2P room")
    p_join.add_argument("--room", type=str, required=True)
    p_join.add_argument("--host", type=str, required=True)
    p_join.add_argument("--port", type=int, default=PAPAYA_NET_DEFAULT_PORT)

    p_cfg = subparsers.add_parser("config", help="Write Goldberg broadcast config for a game")
    p_cfg.add_argument("--room", type=str, required=True)
    p_cfg.add_argument("--game-dir", type=str, default=".")

    args = parser.parse_args()

    if args.command == "host":
        code = args.room or generate_room_code()
        print(f"[PAPAYA_NET] Starting room host. Room code: {code}")
        host = PapayaNetHost(room_code=code, bind_port=args.port)
        try:
            host.start()
        except KeyboardInterrupt:
            host.stop()
    elif args.command == "join":
        client = PapayaNetClient(room_code=args.room, host_addr=args.host, host_port=args.port)
        if not client.connect():
            sys.exit(1)
        try:
            while True:
                time.sleep(1.0)
        except KeyboardInterrupt:
            client.disconnect()
    elif args.command == "config":
        room = PapayaNetRoom(args.room)
        room.write_goldberg_config(args.game_dir)
    else:
        parser.print_help()
