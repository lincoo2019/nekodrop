# SPDX-License-Identifier: MIT
"""Packet tests for the patched IPv6-only AWDL mDNS responder (#51)."""

import ipaddress
from pathlib import Path
import struct
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(sys.argv.pop(1)).resolve()))
import mdns


HOST = '0a1b2c3d4e5f.local'
ADDRESS = ipaddress.IPv6Address('fe80::5000:5eff:fe00:5301')
SERVICE = mdns.Service('_airdrop._tcp.local', '0a1b2c3d4e5f', HOST, ADDRESS, 8770,
                       'flags=1019')


def sections(packet):
    _, flags, nq, na, ns, nr = struct.unpack_from('!6H', packet)
    assert (flags, nq, ns) == (0x8400, 0, 0)
    offset = 12
    records = []
    for _ in range(na + nr):
        name, offset = mdns.decode_name(packet, offset)
        rtype, rclass, ttl, length = struct.unpack_from('!HHIH', packet, offset)
        offset += 10
        records.append((name, rtype, rclass, ttl, packet[offset:offset + length]))
        offset += length
    assert offset == len(packet)
    return records[:na], records[na:]


class AddressQueries(unittest.TestCase):
    def assert_nsec(self, record):
        self.assertEqual(record[:4], (HOST, 47, 0x8001, 120))
        name, offset = mdns.decode_name(record[4], 0)
        self.assertEqual(name, HOST)
        # Window 0, four bytes, only the AAAA bit (28). No A or NSEC bit.
        self.assertEqual(record[4][offset:], bytes.fromhex('00 04 00 00 00 08'))

    def test_a_query_answers_no_a_with_reachable_ipv6_additional(self):
        answers, additional = sections(SERVICE.address_response({1}))
        self.assertEqual(len(answers), 1)
        self.assert_nsec(answers[0])
        self.assertEqual(additional, [(HOST, 28, 0x8001, 120, ADDRESS.packed)])

    def test_aaaa_query_answers_aaaa_with_negative_additional(self):
        answers, additional = sections(SERVICE.address_response({28}))
        self.assertEqual(answers, [(HOST, 28, 0x8001, 120, ADDRESS.packed)])
        self.assertEqual(len(additional), 1)
        self.assert_nsec(additional[0])

    def test_combined_query_answers_both(self):
        answers, additional = sections(SERVICE.address_response({1, 28}))
        self.assertEqual([record[1] for record in answers], [28, 47])
        self.assert_nsec(answers[1])
        self.assertEqual(additional, [])

    def test_ipv6_only_announcement_and_goodbye(self):
        for alive in (True, False):
            answers, additional = sections(SERVICE.response(alive=alive))
            self.assertEqual([record[1] for record in answers], [12, 33, 16, 28, 47])
            self.assertEqual(additional, [])
            self.assertTrue(all(bool(record[3]) == alive for record in answers))
            if alive:
                self.assert_nsec(answers[-1])

    def test_typed_query_handles_case_qu_and_truncation(self):
        packet = mdns.query([(HOST.upper(), 1), (HOST, 28)])
        qu = packet[:-2] + b'\x80\x01'
        self.assertEqual(mdns.questions_in(qu), {(HOST, 1), (HOST, 28)})
        self.assertEqual(mdns.questions_in(qu[:-1]), set())
        self.assertEqual(mdns.questions_in(SERVICE.response()), set())

    def test_address_replies_do_not_replace_periodic_service_announcements(self):
        announcer = mdns.Announcer.__new__(mdns.Announcer)
        announcer.service = SERVICE
        announcer.stopping = threading.Event()
        sent = []
        announcer._send = sent.append

        def asked():
            if len(sent) == 1:
                return {(HOST, 1)}
            announcer.stopping.set()
            return set()

        announcer._asked = asked
        with patch.object(mdns.time, 'monotonic', side_effect=[0, 0.1, 2.05, 2.1]):
            announcer._run()
        self.assertEqual(sent, [SERVICE.response(), SERVICE.address_response({1}),
                                SERVICE.response()])

    def test_qu_query_sends_unicast_and_multicast_reply(self):
        packet = mdns.query([(HOST, 28)])
        qu = packet[:-2] + b'\x80\x01'
        qs, any_qu = mdns.questions_in_with_qu(qu)
        self.assertEqual(qs, {(HOST, 28)})
        self.assertTrue(any_qu)
        _, no_qu = mdns.questions_in_with_qu(packet)
        self.assertFalse(no_qu)

        announcer = mdns.Announcer.__new__(mdns.Announcer)
        announcer.service = SERVICE
        announcer.interface_index = 7
        announcer.stopping = threading.Event()
        mcast, ucast = [], []
        announcer._send = mcast.append
        announcer._send_to = lambda pkt, target: ucast.append((pkt, target))

        def asked():
            if len(mcast) == 1:
                announcer._last_qu_sender = ('fe80::1', 5353, 0, 7)
                return {('_airdrop._tcp.local', 12)}
            announcer.stopping.set()
            return set()

        announcer._asked = asked
        with patch.object(mdns.time, 'monotonic', side_effect=[0, 0.1, 0.2, 0.3]):
            announcer._run()
        self.assertEqual(len(ucast), 1)
        self.assertEqual(ucast[0], (SERVICE.response(), ('fe80::1', 5353, 0, 7)))
        self.assertEqual(mcast, [SERVICE.response(), SERVICE.response()])


if __name__ == '__main__':
    unittest.main()
