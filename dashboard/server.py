#!/usr/bin/env python3
from __future__ import annotations

import ipaddress
import json
import os
import random
import socket
import struct
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


DASHBOARD_DIR = Path(__file__).resolve().parent
NETX_HOST = os.environ.get("NETX_HOST", "127.0.0.1")
NETX_PORT = int(os.environ.get("NETX_PORT", "5053"))
HTTP_HOST = os.environ.get("DASHBOARD_HOST", "127.0.0.1")
HTTP_PORT = int(os.environ.get("DASHBOARD_PORT", "8765"))
QUERY_TYPES = {"A": 1, "AAAA": 28}


def encode_dns_name(name: str) -> bytes:
    if not name or len(name) > 253:
        raise ValueError("Enter a valid domain name.")

    parts = name.rstrip(".").split(".")
    encoded = bytearray()
    for part in parts:
        raw = part.encode("ascii")
        if not raw or len(raw) > 63:
            raise ValueError("Enter a valid domain name.")
        encoded.append(len(raw))
        encoded.extend(raw)
    encoded.append(0)
    return bytes(encoded)


def read_dns_name(packet: bytes, offset: int) -> tuple[str, int]:
    labels: list[str] = []
    jumped = False
    next_offset = offset

    while True:
        length = packet[offset]
        if length & 0xC0 == 0xC0:
            pointer = ((length & 0x3F) << 8) | packet[offset + 1]
            if not jumped:
                next_offset = offset + 2
            offset = pointer
            jumped = True
            continue

        offset += 1
        if length == 0:
            if not jumped:
                next_offset = offset
            break

        labels.append(packet[offset:offset + length].decode("ascii", "replace"))
        offset += length

    return ".".join(labels), next_offset


def query_netx(name: str, query_type: str) -> dict:
    qtype = QUERY_TYPES.get(query_type.upper())
    if qtype is None:
        raise ValueError("Supported query types are A and AAAA.")

    query_id = random.randint(0, 65535)
    header = struct.pack("!HHHHHH", query_id, 0x0100, 1, 0, 0, 0)
    question = encode_dns_name(name) + struct.pack("!HH", qtype, 1)
    payload = header + question
    started = time.perf_counter()

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(4)
        sock.sendto(payload, (NETX_HOST, NETX_PORT))
        response, _ = sock.recvfrom(4096)

    latency_ms = round((time.perf_counter() - started) * 1000, 2)
    return parse_dns_response(response, query_id, latency_ms)


def parse_dns_response(packet: bytes, query_id: int, latency_ms: float) -> dict:
    if len(packet) < 12:
        raise ValueError("Received a malformed DNS response.")

    resp_id, flags, qdcount, ancount, _, _ = struct.unpack("!HHHHHH", packet[:12])
    if resp_id != query_id:
        raise ValueError("Received a mismatched DNS response.")

    rcode = flags & 0x000F
    offset = 12
    for _ in range(qdcount):
        _, offset = read_dns_name(packet, offset)
        offset += 4

    answers = []
    for _ in range(ancount):
        name, offset = read_dns_name(packet, offset)
        rtype, rclass, ttl, rdlength = struct.unpack("!HHIH", packet[offset:offset + 10])
        offset += 10
        rdata = packet[offset:offset + rdlength]
        offset += rdlength

        if rtype == 1 and rdlength == 4:
            answers.append({"name": name, "type": "A", "value": str(ipaddress.IPv4Address(rdata)), "ttl": ttl})
        elif rtype == 28 and rdlength == 16:
            answers.append({"name": name, "type": "AAAA", "value": str(ipaddress.IPv6Address(rdata)), "ttl": ttl})
        elif rtype == 5:
            cname, _ = read_dns_name(packet, offset - rdlength)
            answers.append({"name": name, "type": "CNAME", "value": cname, "ttl": ttl})

    return {"rcode": rcode, "latencyMs": latency_ms, "answers": answers}


class DashboardHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/":
            self.send_file("index.html", "text/html; charset=utf-8")
        elif parsed.path == "/api/status":
            self.send_status()
        elif parsed.path == "/api/resolve":
            self.send_resolution(parsed.query)
        else:
            self.send_error(404, "Not found")

    def send_status(self) -> None:
        try:
            result = query_netx("openai.com", "A")
            self.send_json({"running": True, "netx": f"{NETX_HOST}:{NETX_PORT}", **result})
        except Exception as exc:
            self.send_json({"running": False, "netx": f"{NETX_HOST}:{NETX_PORT}", "error": str(exc)}, status=503)

    def send_resolution(self, query: str) -> None:
        params = parse_qs(query)
        name = params.get("name", ["openai.com"])[0].strip()
        qtype = params.get("type", ["A"])[0].strip().upper()
        try:
            self.send_json({"name": name, "type": qtype, **query_netx(name, qtype)})
        except Exception as exc:
            self.send_json({"name": name, "type": qtype, "error": str(exc)}, status=400)

    def send_file(self, filename: str, content_type: str) -> None:
        body = (DASHBOARD_DIR / filename).read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[dashboard] {self.address_string()} - {fmt % args}")


if __name__ == "__main__":
    server = ThreadingHTTPServer((HTTP_HOST, HTTP_PORT), DashboardHandler)
    print(f"NetX dashboard: http://{HTTP_HOST}:{HTTP_PORT}")
    print(f"Testing NetX at {NETX_HOST}:{NETX_PORT}")
    server.serve_forever()
