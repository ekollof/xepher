#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, version 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Exercise registration TLS refusal in isolated WeeChat instances, without accounts."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest


class RegistrationTLS(unittest.TestCase):
    plugin = pathlib.Path(__file__).resolve().parents[1] / "xmpp.so"
    weechat = shutil.which("weechat-headless")
    openssl = shutil.which("openssl")

    def run_server(self, untrusted: bool) -> None:
        if not self.weechat or (untrusted and not self.openssl):
            self.skipTest("weechat-headless and OpenSSL CLI are required")
        with tempfile.TemporaryDirectory(prefix="xepher-ibr-") as temporary:
            directory = pathlib.Path(temporary)
            context = None
            if untrusted:
                certificate, key = directory / "cert.pem", directory / "key.pem"
                subprocess.run(
                    [self.openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                     "-keyout", str(key), "-out", str(certificate), "-days", "1",
                     "-subj", "/CN=localhost"],
                    check=True, capture_output=True, timeout=15,
                )
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.load_cert_chain(certificate, key)

            received = bytearray()
            failures = []
            # Raw registration uses the standard client port; never contact an
            # existing listener if that port is already occupied.
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    listener.bind(("127.0.0.1", 5222))
                except OSError as error:
                    self.skipTest(f"Loopback port 5222 unavailable: {error}")
                listener.listen(1)
                listener.settimeout(8)

                def serve() -> None:
                    try:
                        with listener.accept()[0] as connection:
                            connection.settimeout(8)

                            def read_until(ending: bytes) -> None:
                                while b"<stream:stream" not in received or not received.endswith(ending):
                                    chunk = connection.recv(4096)
                                    if not chunk:
                                        raise AssertionError("Client closed before opening its stream")
                                    received.extend(chunk)

                            read_until(b">")
                            feature = "<starttls xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>" if untrusted else ""
                            connection.sendall((
                                "<stream:stream xmlns='jabber:client' "
                                "xmlns:stream='http://etherx.jabber.org/streams' "
                                "from='127.0.0.1' id='probe' version='1.0'>"
                                f"<stream:features>{feature}</stream:features>"
                            ).encode())
                            if untrusted:
                                # Reset the delimiter check for the next stanza.
                                chunk = connection.recv(4096)
                                received.extend(chunk)
                                while b"starttls" not in received or not received.endswith(b">"):
                                    chunk = connection.recv(4096)
                                    if not chunk:
                                        raise AssertionError("Client did not request STARTTLS")
                                    received.extend(chunk)
                                connection.sendall(b"<proceed xmlns='urn:ietf:params:xml:ns:xmpp-tls'/>")
                                try:
                                    with context.wrap_socket(connection, server_side=True) as secured:
                                        received.extend(secured.recv(4096))
                                except (ssl.SSLError, ConnectionResetError):
                                    pass  # A rejected self-signed certificate aborts the handshake.
                            else:
                                try:
                                    received.extend(connection.recv(4096))
                                except ConnectionResetError:
                                    pass
                    except Exception as error:
                        failures.append(error)

                worker = threading.Thread(target=serve, daemon=True)
                worker.start()
                try:
                    commands = (
                        f"/plugin load {self.plugin};"
                        "/account register tlsprobe tlsprobe@127.0.0.1 test-only-password;"
                        "/wait 3 /quit"
                    )
                    process = subprocess.run(
                        [self.weechat, "--stdout", "-a", "-d", str(directory),
                         "-P", "buflist,logger", "-r", commands],
                        capture_output=True, text=True, timeout=12,
                    )
                finally:
                    worker.join(timeout=9)
                self.assertFalse(worker.is_alive(), "Mock server did not finish")
                self.assertEqual(failures, [], repr(failures))
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                self.assertNotIn(b"jabber:iq:register", received)
                self.assertNotIn(b"test-only-password", received)
                logs = process.stdout + process.stderr + "".join(
                    path.read_text(errors="replace") for path in (directory / "logs").glob("*")
                )
                expected = "TLS negotiation failed" if untrusted else "server did not offer STARTTLS"
                self.assertIn(expected, logs)

    def test_no_starttls_sends_no_registration_credentials(self) -> None:
        self.run_server(False)

    def test_untrusted_certificate_sends_no_registration_credentials(self) -> None:
        self.run_server(True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", type=pathlib.Path, default=RegistrationTLS.plugin)
    parser.add_argument("--weechat", default=RegistrationTLS.weechat)
    parser.add_argument("--openssl", default=RegistrationTLS.openssl)
    arguments, remaining = parser.parse_known_args()
    RegistrationTLS.plugin = arguments.plugin.resolve()
    RegistrationTLS.weechat = arguments.weechat
    RegistrationTLS.openssl = arguments.openssl
    unittest.main(argv=[__file__, *remaining])
