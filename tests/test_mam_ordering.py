#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Race live PMs and carbons against archive history in isolated WeeChat."""

from __future__ import annotations

import argparse
import datetime
import pathlib
import shutil
import socket
import subprocess
import tempfile
import threading
import unittest
import xml.etree.ElementTree as ET
from xml.sax.saxutils import quoteattr


class MamOrdering(unittest.TestCase):
    plugin = pathlib.Path(__file__).resolve().parents[1] / "xmpp.so"
    weechat = shutil.which("weechat-headless")

    def test_live_pm_and_carbon_wait_for_history(self) -> None:
        if not self.weechat:
            self.skipTest("weechat-headless is required")
        failures = []
        completed = threading.Event()
        with socket.socket() as listener, tempfile.TemporaryDirectory(prefix="xepher-mam-") as temp:
            try:
                listener.bind(("127.0.0.1", 5222))
            except OSError as error:
                self.skipTest(f"Loopback port 5222 unavailable: {error}")
            listener.listen(1)
            listener.settimeout(10)

            def serve() -> None:
                try:
                    with listener.accept()[0] as conn:
                        conn.settimeout(10)
                        parser = ET.XMLPullParser(("start", "end"))
                        depth = 0
                        authenticated = False
                        seed_sent = False

                        def send(xml: str) -> None:
                            conn.sendall(xml.encode())

                        def body(identifier: str, text: str, sender: str = "peer@127.0.0.1/phone") -> str:
                            return (f"<message xmlns='jabber:client' from='{sender}' to='probe@127.0.0.1' "
                                    f"type='chat' id='{identifier}'><body>{text}</body>"
                                    f"<stanza-id xmlns='urn:xmpp:sid:0' by='probe@127.0.0.1' id='{identifier}'/>"
                                    "</message>")

                        def archive(query: ET.Element, identifier: str, text: str) -> None:
                            stamp = (datetime.datetime.now(datetime.timezone.utc)
                                     - datetime.timedelta(minutes=5)).strftime("%Y-%m-%dT%H:%M:%SZ")
                            send("<message from='probe@127.0.0.1'><result xmlns='urn:xmpp:mam:2' "
                                 f"queryid={quoteattr(query.get('queryid', ''))} id='{identifier}'>"
                                 "<forwarded xmlns='urn:xmpp:forward:0'>"
                                 f"<delay xmlns='urn:xmpp:delay' stamp='{stamp}'/>"
                                 f"{body(identifier, text)}</forwarded></result></message>")

                        def finish(iq_id: str) -> None:
                            send(f"<iq type='result' id={quoteattr(iq_id)}><fin xmlns='urn:xmpp:mam:2' "
                                 "complete='true'/></iq>")

                        while chunk := conn.recv(65536):
                            parser.feed(chunk)
                            for event, element in parser.read_events():
                                if event == "start":
                                    depth += 1
                                    if depth == 1:
                                        send("<stream:stream xmlns='jabber:client' "
                                             "xmlns:stream='http://etherx.jabber.org/streams' "
                                             "from='127.0.0.1' id='fixture' version='1.0'>")
                                        if authenticated:
                                            send("<stream:features><bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'/>"
                                                 "</stream:features>")
                                        else:
                                            send("<stream:features><mechanisms xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>"
                                                 "<mechanism>PLAIN</mechanism></mechanisms></stream:features>")
                                    continue
                                depth -= 1
                                if depth != 1:
                                    continue
                                if element.tag == "{urn:ietf:params:xml:ns:xmpp-sasl}auth":
                                    send("<success xmlns='urn:ietf:params:xml:ns:xmpp-sasl'/>")
                                    authenticated = True
                                    parser = ET.XMLPullParser(("start", "end"))
                                    depth = 0
                                elif element.tag == "{jabber:client}iq":
                                    iq_id = element.get("id", "")
                                    if element.find("{urn:ietf:params:xml:ns:xmpp-bind}bind") is not None:
                                        send(f"<iq type='result' id={quoteattr(iq_id)}>"
                                             "<bind xmlns='urn:ietf:params:xml:ns:xmpp-bind'>"
                                             "<jid>probe@127.0.0.1/fixture</jid></bind></iq>")
                                    elif (query := element.find("{urn:xmpp:mam:2}query")) is not None:
                                        peer_filter = query.find(".//{jabber:x:data}field[@var='with']")
                                        if peer_filter is None:
                                            if not seed_sent:
                                                archive(query, "seed", "SEED_HISTORY")
                                                seed_sent = True
                                            finish(iq_id)
                                        else:
                                            send(body("live", "LIVE_NEWER"))
                                            send(body("overlap", "LIVE_ARCHIVE_OVERLAP"))
                                            send("<message from='probe@127.0.0.1'><received xmlns='urn:xmpp:carbons:2'>"
                                                 "<forwarded xmlns='urn:xmpp:forward:0'>"
                                                 f"{body('carbon', 'CARBON_NEWER')}</forwarded></received></message>")
                                            archive(query, "seed", "SEED_HISTORY")
                                            archive(query, "older", "HISTORY_OLDER")
                                            archive(query, "overlap", "LIVE_ARCHIVE_OVERLAP")
                                            finish(iq_id)
                                            completed.set()
                                    elif element.get("type") in ("get", "set"):
                                        send(f"<iq type='result' id={quoteattr(iq_id)}>"
                                             "<query xmlns='jabber:iq:roster'/></iq>")
                                element.clear()
                except (ConnectionResetError, BrokenPipeError):
                    if not completed.is_set():
                        failures.append("Client disconnected before completing the fixture")
                except Exception as error:
                    failures.append(repr(error))

            worker = threading.Thread(target=serve, daemon=True)
            worker.start()
            process = subprocess.run(
                [self.weechat, "--stdout", "-a", "-d", temp, "-P", "buflist,logger", "-r",
                 f"/plugin load {self.plugin};/account add probe probe@127.0.0.1 fixture-password;"
                 "/set xmpp.account.probe.tls disable;/account connect probe;/wait 6 /quit"],
                capture_output=True, text=True, timeout=15)
            worker.join(timeout=11)
            output = process.stdout + process.stderr
            self.assertFalse(worker.is_alive(), output)
            self.assertEqual(failures, [], output)
            self.assertTrue(completed.is_set(), output)
            self.assertEqual(process.returncode, 0, output)
            logs = list((pathlib.Path(temp) / "logs").glob("*peer@127.0.0.1*"))
            self.assertEqual(len(logs), 1, output)
            history = logs[0].read_text()
            for marker in ("SEED_HISTORY", "HISTORY_OLDER", "LIVE_ARCHIVE_OVERLAP",
                           "LIVE_NEWER", "CARBON_NEWER"):
                self.assertEqual(history.count(marker), 1, history)
            self.assertLess(history.index("SEED_HISTORY"), history.index("HISTORY_OLDER"), history)
            self.assertLess(history.index("HISTORY_OLDER"), history.index("LIVE_ARCHIVE_OVERLAP"), history)
            self.assertLess(history.index("HISTORY_OLDER"), history.index("LIVE_NEWER"), history)
            self.assertLess(history.index("LIVE_NEWER"), history.index("CARBON_NEWER"), history)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", type=pathlib.Path, default=MamOrdering.plugin)
    parser.add_argument("--weechat", default=MamOrdering.weechat)
    options, remaining = parser.parse_known_args()
    MamOrdering.plugin = options.plugin.resolve()
    MamOrdering.weechat = options.weechat
    unittest.main(argv=[__file__, *remaining])
