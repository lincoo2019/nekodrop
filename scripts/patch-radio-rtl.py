# Applies nekodrop's changes to winawdl's RTL8852CE radio daemon sources
# (radio/winawdl_radio.cpp) before they are built.
# Usage: python patch-radio-rtl.py <winawdl folder>
#
# - The radio reports the iPhone's 5 GHz AWDL channel (#32).  nekodrop starts
#   every card on channel 6 and moves to the phone's 44 or 149 when the radio
#   says "event: peer-channel N", as the Intel and MediaTek radios do.  This
#   radio never said it, so the RTL8852CE stayed on 2.4 GHz channel 6 at
#   20 MHz, where the phone spends only a few slots: 1 MB to an iPhone took
#   about 30 s and often failed.  winawdl itself runs this card on channel 44
#   (9-14 MB/s sending).  The channel comes from the sequence in the phone's
#   AWDL frames (TLV 18), by the rule of iwxradio/src/awdl_policy.h
#   peer_channel(); the receive thread notes it, the pipe thread logs it.
#   2.1.0 still never moved: the radio now also logs each new sequence it
#   reads (at most 20 a run), so the next report shows what the phone names.
#
# Idempotent; exits with an error when the source no longer matches.
import pathlib
import sys

NL = chr(10)
radio = pathlib.Path(sys.argv[1]) / 'radio' / 'winawdl_radio.cpp'
text = radio.read_text(encoding='utf-8')
MARKER = 'nekodrop (#32)'

HELPER = NL.join([
    '/* nekodrop (#32): the phone\'s 5 GHz AWDL channel, 44 or 149 (0 if neither),',
    ' * from the channel sequence (TLV 18) of an AWDL action frame. The same rule',
    ' * as nekodrop\'s iwxradio/src/awdl_policy.h peer_channel(): slots 6, 7, 14',
    ' * and 15 are the phone\'s own Wi-Fi, and 44 wins over 149. The receive',
    ' * thread notes a change, the pipe thread logs it for the service. */',
    'namespace nekodrop {',
    'std::atomic<unsigned> peer_channel_news{0};',
    'unsigned peer_channel_told = 0; /* receive thread only */',
    '/* The last sequence seen, for the log (2.1.0 never moved off 6 and its log',
    ' * could not say why, #32): 16 channel numbers packed in two words. */',
    'std::atomic<uint64_t> seq_lo{0}, seq_hi{0};',
    'std::atomic<bool> seq_news{false};',
    'uint64_t seq_told_lo = 0, seq_told_hi = 0; /* receive thread only */',
    'unsigned seq_logged = 0; /* pipe thread only */',
    '',
    'unsigned peer_channel(std::span<const uint8_t> f) {',
    '  if (f.size() < 40 || f[0] != 0xd0 || f[24] != 0x7f || f[25] != 0x00 ||',
    '      f[26] != 0x17 || f[27] != 0xf2 || f[28] != 0x08)',
    '    return 0;',
    '  uint8_t seq[16] = {};',
    '  bool found = false;',
    '  for (size_t i = 40; i + 3 <= f.size();) {',
    '    const size_t value = size_t(f[i + 1]) | size_t(f[i + 2]) << 8;',
    '    if (i + 3 + value > f.size()) return 0;',
    '    if (f[i] == 18) {',
    '      if (value < 6 + 32) return 0;',
    '      for (size_t n = 0; n < 16; ++n) seq[n] = f[i + 3 + 6 + 2 * n];',
    '      found = true;',
    '      break;',
    '    }',
    '    i += 3 + value;',
    '  }',
    '  if (!found) return 0;',
    '  uint64_t lo = 0, hi = 0;',
    '  for (size_t n = 0; n < 8; ++n) {',
    '    lo |= uint64_t(seq[n]) << (8 * n);',
    '    hi |= uint64_t(seq[n + 8]) << (8 * n);',
    '  }',
    '  if (lo != seq_told_lo || hi != seq_told_hi) {',
    '    seq_told_lo = lo;',
    '    seq_told_hi = hi;',
    '    seq_lo = lo;',
    '    seq_hi = hi;',
    '    seq_news = true;',
    '  }',
    '  const auto names = [&seq](unsigned ch, bool wifi_too) {',
    '    const auto at = [&seq](size_t n) { return unsigned(seq[n]); };',
    '    if (!wifi_too && (at(6) == ch || at(7) == ch || at(14) == ch || at(15) == ch))',
    '      return false;',
    '    for (size_t n = 0; n < 16; ++n)',
    '      if ((n & 7) < 6 && at(n) == ch) return true;',
    '    return false;',
    '  };',
    '  const bool names149 = names(149, false);',
    '  if (names(44, !names149)) return 44;',
    '  return names149 ? 149u : 0u;',
    '}',
    '} // namespace nekodrop',
    '',
    '',
])

RX_OLD = '  if (packet.RxAtrib.crc_err) return;' + NL + '  note_station(frame, session);' + NL
RX_NEW = NL.join([
    '  if (packet.RxAtrib.crc_err) return;',
    '  const auto body = (packet.RxAtrib.fcs_present && frame.size() >= 44) ? frame.first(frame.size() - 4) : frame;',
    '  if (const unsigned ch = nekodrop::peer_channel(body);',
    '      ch && ch != nekodrop::peer_channel_told) { /* nekodrop (#32) */',
    '    nekodrop::peer_channel_told = ch;',
    '    nekodrop::peer_channel_news = ch;',
    '  }',
    '  note_station(frame, session);',
    '',
])

LOOP_OLD = '    messages++;' + NL + '    untold++;' + NL
LOOP_NEW = NL.join([
    '    messages++;',
    '    untold++;',
    '    /* Must be warn: winawdl_radio raises logger level to Warn during serve_client (#32). */',
    '    if (const unsigned ch = nekodrop::peer_channel_news.exchange(0)) /* nekodrop (#32) */',
    '      logger->warn("radio: event: peer-channel {}", ch);',
    '    if (nekodrop::seq_news.exchange(false) && nekodrop::seq_logged < 20) { /* nekodrop (#32) */',
    '      nekodrop::seq_logged++;',
    '      const uint64_t lo = nekodrop::seq_lo, hi = nekodrop::seq_hi;',
    '      std::string seq;',
    '      for (unsigned n = 0; n < 16; ++n)',
    '        seq += (n ? " " : "") + std::to_string(unsigned(((n < 8 ? lo : hi) >> (8 * (n & 7))) & 0xff));',
    '      logger->warn("radio: peer sequence {}", seq);',
    '    }',
    '',
])

FWD = 'void forward_received(const Packet &packet, PipeServer &pipe, Session &session,'

if MARKER in text:
    print('winawdl_radio.cpp: already patched')
    sys.exit(0)
for what, old in (('forward_received', FWD), ('the receive check', RX_OLD), ('the pipe loop', LOOP_OLD)):
    if text.count(old) != 1:
        sys.exit(f'winawdl_radio.cpp: {what} moved; update patch-radio-rtl.py')
text = text.replace(FWD, HELPER + FWD).replace(RX_OLD, RX_NEW).replace(LOOP_OLD, LOOP_NEW)
radio.write_text(text, encoding='utf-8')
print('winawdl_radio.cpp: the radio reports the phone\'s 5 GHz AWDL channel')
