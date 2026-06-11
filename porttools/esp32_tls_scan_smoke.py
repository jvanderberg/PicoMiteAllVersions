#!/usr/bin/env python3
"""ESP32-S3 WiFi scan and TLS smoke with a local certificate authority."""

from __future__ import annotations

import argparse
import os
import queue
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from basic_serial import BasicSerial, default_port, strip_ansi
from esp32_fs_vm_smoke import basic_string_expr
from network_conformance import (
    CheckResult,
    MqttTestBroker,
    command_marker,
    local_ip_for,
    marker_value,
    mqtt_read_packet,
    maybe_connect,
    print_checks,
    sync_with_reopen_retry,
)


class TlsHttpServer:
    def __init__(self, host: str, port: int, cert: Path, key: Path):
        self.host = host
        self.port = port
        self.cert = cert
        self.key = key
        self.ready: queue.Queue[Exception | None] = queue.Queue()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.first_line = ""

    def __enter__(self) -> "TlsHttpServer":
        self.thread.start()
        state = self.ready.get(timeout=5)
        if state is not None:
            raise state
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop.set()
        try:
            socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
        except OSError:
            pass
        self.thread.join(timeout=1)

    def _serve(self) -> None:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.cert), str(self.key))
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(5)
        except Exception as exc:
            self.ready.put(exc)
            srv.close()
            return
        srv.settimeout(0.25)
        self.ready.put(None)
        with srv:
            while not self.stop.is_set():
                try:
                    conn, _addr = srv.accept()
                except socket.timeout:
                    continue
                try:
                    with context.wrap_socket(conn, server_side=True) as tls:
                        tls.settimeout(10)
                        request = tls.recv(2048)
                        self.first_line = request.splitlines()[0].decode("latin1", "replace") if request else ""
                        body = b"ESP32_TLS_REQUEST_OK\n"
                        tls.sendall(
                            b"HTTP/1.0 200 OK\r\n"
                            b"Content-Type: text/plain\r\n"
                            + f"Content-Length: {len(body)}\r\n".encode("ascii")
                            + b"Connection: close\r\n\r\n"
                            + body
                        )
                        return
                except ssl.SSLError:
                    continue


class TlsStreamServer:
    def __init__(self, host: str, port: int, cert: Path, key: Path):
        self.host = host
        self.port = port
        self.cert = cert
        self.key = key
        self.ready: queue.Queue[Exception | None] = queue.Queue()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.received = b""

    def __enter__(self) -> "TlsStreamServer":
        self.thread.start()
        state = self.ready.get(timeout=5)
        if state is not None:
            raise state
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop.set()
        try:
            socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
        except OSError:
            pass
        self.thread.join(timeout=1)

    def _serve(self) -> None:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.cert), str(self.key))
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(5)
        except Exception as exc:
            self.ready.put(exc)
            srv.close()
            return
        srv.settimeout(0.25)
        self.ready.put(None)
        with srv:
            while not self.stop.is_set():
                try:
                    conn, _addr = srv.accept()
                except socket.timeout:
                    continue
                try:
                    with context.wrap_socket(conn, server_side=True) as tls:
                        tls.settimeout(10)
                        self.received = tls.recv(512)
                        tls.sendall(b"TLS_STREAM_OK\n")
                        time.sleep(0.2)
                        return
                except ssl.SSLError:
                    continue


class TlsMqttBroker(MqttTestBroker):
    def __init__(self, host: str, port: int, cert: Path, key: Path):
        super().__init__(host, port)
        self.cert = cert
        self.key = key

    def _serve(self) -> None:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(self.cert), str(self.key))
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self.host, self.port))
            srv.listen(1)
        except Exception as exc:
            self.ready.put(exc)
            srv.close()
            return
        srv.settimeout(0.25)
        self.ready.put(None)
        with srv:
            while not self.stop.is_set():
                try:
                    conn, _addr = srv.accept()
                except socket.timeout:
                    continue
                try:
                    with context.wrap_socket(conn, server_side=True) as tls:
                        tls.settimeout(10)
                        self._handle_client(tls)  # type: ignore[arg-type]
                except ssl.SSLError:
                    continue
                if self.log.connected:
                    return


def clean_text(text: str) -> str:
    return strip_ansi(text.encode("latin1", "replace")).decode("latin1", "replace")


def make_cert(tempdir: Path, ip: str) -> tuple[Path, Path, Path]:
    key = tempdir / "tls_smoke.key"
    cert = tempdir / "tls_smoke.crt"
    config = tempdir / "openssl.cnf"
    config.write_text(
        "\n".join(
            [
                "[req]",
                "distinguished_name=req_distinguished_name",
                "x509_extensions=v3_req",
                "prompt=no",
                "[req_distinguished_name]",
                f"CN={ip}",
                "[v3_req]",
                f"subjectAltName=IP:{ip}",
                "basicConstraints=critical,CA:TRUE",
                "keyUsage=critical,keyCertSign,digitalSignature,keyEncipherment",
                "extendedKeyUsage=serverAuth",
                "",
            ]
        ),
        encoding="ascii",
    )
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            str(key),
            "-out",
            str(cert),
            "-days",
            "1",
            "-config",
            str(config),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return cert, key, cert


def upload_text_file(basic: BasicSerial, path: str, text: str, timeout: float) -> None:
    basic.command(f'ON ERROR SKIP : KILL "{path}"', timeout=timeout, check_error=False)
    basic.command(f'OPEN "{path}" FOR OUTPUT AS #1', timeout=timeout)
    try:
        for line in text.splitlines():
            basic.command(f"PRINT #1,{basic_string_expr(line)}", timeout=timeout)
    finally:
        basic.command("CLOSE #1", timeout=timeout, check_error=False)


def run_scan(basic: BasicSerial, timeout: float, long_timeout: float) -> list[CheckResult]:
    checks: list[CheckResult] = []
    basic.command("NEW", timeout=timeout)
    basic.command("OPTION BASE 0", timeout=timeout)
    basic.command("DIM INTEGER SC%(8192/8)", timeout=timeout)
    basic.command("WEB SCAN SC%()", timeout=long_timeout)
    length_text = basic.command('PRINT "SCAN_LEN=" + STR$(SC%(0))', timeout=timeout).clean_text
    scan_len = int(float(marker_value(length_text, "SCAN_LEN=") or "-1"))
    sample_value = ""
    if scan_len > 0:
        sample_value = command_marker(
            basic,
            'PRINT "SCAN_TEXT=" + LEFT$(LGETSTR$(SC%(),1,MIN(SC%(0),240)),120)',
            "SCAN_TEXT=",
            timeout=timeout,
        )
    checks.append(CheckResult("web_scan_array", scan_len > 0, f"bytes={scan_len}"))
    checks.append(CheckResult("web_scan_text", bool(sample_value), "scan produced text"))
    return checks


def run_tls_suite(args: argparse.Namespace) -> int:
    mac_ip = args.host or local_ip_for(args.gateway)
    ca_path = "A:esp32_tls_smoke_ca.pem"
    checks: list[CheckResult] = []
    with tempfile.TemporaryDirectory(prefix="esp32_tls_smoke_") as tmp:
        cert, key, ca = make_cert(Path(tmp), mac_ip)
        ca_text = ca.read_text(encoding="ascii")
        with (
            TlsHttpServer(args.bind, args.https_port, cert, key) as http_server,
            TlsStreamServer(args.bind, args.tls_stream_port, cert, key) as stream_server,
            TlsMqttBroker(args.bind, args.mqtt_tls_port, cert, key) as mqtt_broker,
            BasicSerial(args.port, args.baud) as basic,
        ):
            sync_with_reopen_retry(basic, args)
            maybe_connect(basic, args)
            checks.extend(run_scan(basic, args.timeout, args.long_timeout))
            upload_text_file(basic, ca_path, ca_text, args.timeout)
            basic.command(f'WEB TLS CERT "{ca_path}"', timeout=args.timeout)
            checks.append(CheckResult("tls_cert_load", True, ca_path))

            commands = [
                "NEW",
                "OPTION BASE 0",
                "DIM INTEGER A%(4096/8)",
                "CR$=CHR$(13)+CHR$(10)",
                f'WEB OPEN TLS CLIENT "{mac_ip}",{args.https_port}',
                f'WEB TLS CLIENT REQUEST "GET /tls-smoke HTTP/1.0"+CR$+"Host: {mac_ip}"+CR$+CR$,A%(),10000',
                'PRINT "TLS_REQ_LEN=" + STR$(LLEN(A%()))',
                'PRINT LGETSTR$(A%(),1,240)',
                "WEB CLOSE TLS CLIENT",
                "DIM INTEGER S%(256/8)",
                "DIM INTEGER R%,W%",
                "R%=0:W%=0",
                f'WEB OPEN TLS STREAM "{mac_ip}",{args.tls_stream_port}',
                "PAUSE 100",
                'WEB TLS CLIENT STREAM "TLS_INLINE"+CHR$(10),S%(),R%,W%',
                "PAUSE 1000",
                'PRINT "TLS_STREAM_PTR=" + STR$(R%) + "," + STR$(W%)',
                "S%(0)=W%",
                'IF W%>0 THEN PRINT LGETSTR$(S%(),1,W%) ELSE PRINT "TLS_STREAM_EMPTY"',
                "WEB CLOSE TLS CLIENT",
                f'WEB MQTT CONNECT "{mac_ip}",{args.mqtt_tls_port},"","",,1',
                'WEB MQTT SUBSCRIBE "netconf/in",0',
                "PAUSE 500",
                'PRINT "TLS_MQTT_TOPIC=" + MM.TOPIC$',
                'PRINT "TLS_MQTT_MSG=" + MM.MESSAGE$',
                'WEB MQTT PUBLISH "netconf/out","NETCONF_MQTT_OUT",0,0',
                'WEB MQTT UNSUBSCRIBE "netconf/in"',
                "WEB MQTT CLOSE",
                'WEB TLS CERT ""',
                f'ON ERROR SKIP : KILL "{ca_path}"',
            ]
            transcript_parts: list[str] = []
            for line in commands:
                timeout = args.long_timeout if line.startswith(("WEB OPEN TLS", "WEB TLS CLIENT", "WEB MQTT CONNECT", "PAUSE", "WEB SCAN")) else args.timeout
                print(f">>> {line}", flush=True)
                result = basic.command(line, timeout=timeout)
                print(result.text, end="" if result.text.endswith("\n") else "\n", flush=True)
                transcript_parts.append(result.text)

        clean = clean_text("".join(transcript_parts))
        checks.extend(
            [
                CheckResult("tls_request_peer", http_server.first_line == "GET /tls-smoke HTTP/1.0", repr(http_server.first_line)),
                CheckResult("tls_request_response", "ESP32_TLS_REQUEST_OK" in clean),
                CheckResult("tls_stream_peer", stream_server.received == b"TLS_INLINE\n", repr(stream_server.received)),
                CheckResult("tls_stream_response", "TLS_STREAM_OK" in clean),
                CheckResult("mqtt_tls_connect", mqtt_broker.log.connected and bool(mqtt_broker.log.client_id), mqtt_broker.log.client_id),
                CheckResult("mqtt_tls_subscribe", mqtt_broker.log.subscribed_topic == "netconf/in", repr(mqtt_broker.log.subscribed_topic)),
                CheckResult("mqtt_tls_receive_topic", (marker_value(clean, "TLS_MQTT_TOPIC=") == "netconf/in"), repr(marker_value(clean, "TLS_MQTT_TOPIC="))),
                CheckResult("mqtt_tls_receive_message", (marker_value(clean, "TLS_MQTT_MSG=") == "NETCONF_MQTT_IN"), repr(marker_value(clean, "TLS_MQTT_MSG="))),
                CheckResult("mqtt_tls_publish", mqtt_broker.log.published_topic == "netconf/out" and mqtt_broker.log.published_payload == b"NETCONF_MQTT_OUT", f"topic={mqtt_broker.log.published_topic!r} payload={mqtt_broker.log.published_payload!r}"),
            ]
        )
    return print_checks(checks)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=default_port())
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--boot-wait", type=float, default=1.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--long-timeout", type=float, default=60.0)
    parser.add_argument("--reset-before-suite", action="store_true")
    parser.add_argument("--connect-command")
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--host", help="Mac-side IP reachable from the ESP32")
    parser.add_argument("--gateway", default="192.168.4.1")
    parser.add_argument("--https-port", type=int, default=18443)
    parser.add_argument("--tls-stream-port", type=int, default=18444)
    parser.add_argument("--mqtt-tls-port", type=int, default=18445)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    return run_tls_suite(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"esp32_tls_scan_smoke: FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)
