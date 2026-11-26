// SPDX-License-Identifier: GPL-2.0-only
/* See mt7902.h. Function names in comments are the mt76 ones each part
 * follows, so a reader can check the port against Linux. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "mt7902.h"

using namespace mt;

namespace {

constexpr uint64_t kDmaLimit = 1ull << 32; /* mt7921: dma_set_mask(32) */
constexpr size_t kDmaPages = 640;
constexpr size_t kDmaMinPages = 420;
constexpr unsigned kDrvOwnRetries = 10;
constexpr size_t kScatterChunk = 4096;
constexpr uint32_t kRxBufLen = 2048;
/* RegOp kinds (proxy.h). */
constexpr uint8_t kOpRead = 0, kOpWrite = 1, kOpClear = 3;

std::string format(const char *fmt, ...) {
  char line[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  return line;
}

uint32_t be32(uint32_t v) { return _byteswap_ulong(v); }

using Clock = std::chrono::steady_clock;

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}

volatile Desc *desc_at(const DmaBuf &ring, unsigned i) {
  return reinterpret_cast<volatile Desc *>(ring.va) + i;
}

uint32_t ring_reg(bool tx, unsigned hw, unsigned field) {
  return (tx ? kTxRingBase : kRxRingBase) + hw * kRingRegStride + field * 4;
}
enum { kRingDescBase, kRingSize, kRingCpuIdx, kRingDmaIdx };

/* __mt7921_reg_addr's fixed windows: chip address, BAR offset, size. */
struct RegMap {
  uint32_t phys, maps, size;
};
constexpr RegMap kFixedMap[] = {
    {0x820d0000, 0x30000, 0x10000}, {0x820ed000, 0x24800, 0x00800}, {0x820e4000, 0x21000, 0x00400},
    {0x820e7000, 0x21e00, 0x00200}, {0x820eb000, 0x24200, 0x00400}, {0x820e2000, 0x20800, 0x00400},
    {0x820e3000, 0x20c00, 0x00400}, {0x820e5000, 0x21400, 0x00800}, {0x00400000, 0x80000, 0x10000},
    {0x00410000, 0x90000, 0x10000}, {0x40000000, 0x70000, 0x10000}, {0x54000000, 0x02000, 0x01000},
    {0x55000000, 0x03000, 0x01000}, {0x58000000, 0x06000, 0x01000}, {0x59000000, 0x07000, 0x01000},
    {0x7c000000, 0xf0000, 0x10000}, {0x7c020000, 0xd0000, 0x10000}, {0x7c060000, 0xe0000, 0x10000},
    {0x80020000, 0xb0000, 0x10000}, {0x81020000, 0xc0000, 0x10000}, {0x820c0000, 0x08000, 0x04000},
    {0x820c8000, 0x0c000, 0x02000}, {0x820cc000, 0x0e000, 0x01000}, {0x820cd000, 0x0f000, 0x01000},
    {0x74030000, 0x10000, 0x10000}, {0x820ce000, 0x21c00, 0x00200}, {0x820cf000, 0x22000, 0x01000},
    {0x820e0000, 0x20000, 0x00400}, {0x820e1000, 0x20400, 0x00200}, {0x820e9000, 0x23400, 0x00200},
    {0x820ea000, 0x24000, 0x00200}, {0x820ec000, 0x24600, 0x00200}, {0x820f0000, 0xa0000, 0x00400},
    {0x820f1000, 0xa0600, 0x00200}, {0x820f2000, 0xa0800, 0x00400}, {0x820f3000, 0xa0c00, 0x00400},
    {0x820f4000, 0xa1000, 0x00400}, {0x820f5000, 0xa1400, 0x00800}, {0x820f7000, 0xa1e00, 0x00200},
    {0x820f9000, 0xa3400, 0x00200}, {0x820fa000, 0xa4000, 0x00200}, {0x820fb000, 0xa4200, 0x00400},
    {0x820fc000, 0xa4600, 0x00200}, {0x820fd000, 0xa4800, 0x00800},
};

} // namespace

Mt7902::~Mt7902() { close(); }

void Mt7902::log(const std::string &line) const {
  if (_log) _log(line);
}

/* ---- Registers ---- */

uint32_t Mt7902::map_l1(uint32_t addr) { /* mt7921_reg_map_l1 */
  const uint32_t offset = addr & 0xffff, base = addr >> 16;
  const uint32_t cur = _proxy.read32(kHifRemapL1);
  std::vector<RegOp> ops{{kHifRemapL1, (cur & ~0xffffu) | base, kOpWrite},
                         {kHifRemapL1, 0, kOpRead}}; /* read pushes the write */
  _proxy.batch(ops);
  return kHifRemapBaseL1 + offset;
}

uint32_t Mt7902::reg_addr(uint32_t addr) { /* __mt7921_reg_addr */
  if (addr < 0x100000) return addr;
  for (const RegMap &m : kFixedMap) {
    if (addr < m.phys) continue;
    const uint32_t ofs = addr - m.phys;
    if (ofs > m.size) continue;
    return m.maps + ofs;
  }
  if ((addr >= 0x18000000 && addr < 0x18c00000) || (addr >= 0x70000000 && addr < 0x78000000) ||
      (addr >= 0x7c000000 && addr < 0x7c400000))
    return map_l1(addr);
  log(format("mt7902: unsupported register address %08x", addr));
  return 0;
}

uint32_t Mt7902::rr(uint32_t addr) { return _proxy.read32(reg_addr(addr)); }
void Mt7902::wr(uint32_t addr, uint32_t value) { _proxy.write32(reg_addr(addr), value); }

uint32_t Mt7902::rmw(uint32_t addr, uint32_t mask, uint32_t value) {
  const uint32_t at = reg_addr(addr);
  const uint32_t v = (_proxy.read32(at) & ~mask) | value;
  _proxy.write32(at, v);
  return v;
}

uint32_t Mt7902::l1_rr(uint32_t addr) { return _proxy.read32(map_l1(addr)); }
void Mt7902::l1_wr(uint32_t addr, uint32_t value) { _proxy.write32(map_l1(addr), value); }

bool Mt7902::poll(uint32_t addr, uint32_t mask, uint32_t value, unsigned timeout_ms) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if ((rr(addr) & mask) == value) return true;
    if (_proxy.failed() || Clock::now() > deadline) return false;
    Sleep(1);
  }
}

/* ---- Bring-up ---- */

bool Mt7902::attach(std::string &error) {
  if (!_proxy.open(error)) return false;
  if (_proxy.vendor() != 0x14c3) {
    error = format("the register proxy holds %04x:%04x, not a MediaTek card", _proxy.vendor(),
                   _proxy.device());
    return false;
  }
  log(format("mt7902: register proxy open on %04x:%04x, BAR %u KiB", _proxy.vendor(),
             _proxy.device(), _proxy.bar_len() / 1024));
  if (_proxy.bar_len() < 0x100000) {
    error = "the card's register window is smaller than 1 MiB";
    return false;
  }
  /* However this process ends, the card stops writing into our pages. */
  _proxy.set_close_ops({{kGloCfg, kGloTxDmaEn | kGloRxDmaEn, kOpClear}});

  /* mt792xe_mcu_fw_pmctrl, then __mt792xe_mcu_drv_pmctrl: hand the chip to
   * the firmware and take it back, which wakes it from whatever state the
   * Windows driver left. */
  unsigned i = 0;
  for (; i < kDrvOwnRetries; i++) {
    wr(kConnOnLpctl, kLpcrHostSetOwn);
    if (poll(kConnOnLpctl, kLpcrHostOwnSync, kLpcrHostOwnSync, 50)) break;
  }
  if (i == kDrvOwnRetries) log("mt7902: firmware own did not settle (continuing)");
  for (i = 0; i < kDrvOwnRetries; i++) {
    wr(kConnOnLpctl, kLpcrHostClrOwn);
    if (poll(kConnOnLpctl, kLpcrHostOwnSync, 0, 50)) break;
  }
  if (i == kDrvOwnRetries) {
    /* A firmware that asserted keeps the chip: reset the Wi-Fi subsystem
     * (mt792x_wfsys_reset, as mt792x_reset does) and ask again. */
    log(format("mt7902: driver own refused (CONN_ON_LPCTL %08x): resetting WFSYS", rr(kConnOnLpctl)));
    clear(kWfsysSwRst, kWfsysSwRstB);
    Sleep(50);
    set(kWfsysSwRst, kWfsysSwRstB);
    poll(kWfsysSwRst, kWfsysSwInitDone, kWfsysSwInitDone, 500);
    for (i = 0; i < kDrvOwnRetries; i++) {
      wr(kConnOnLpctl, kLpcrHostSetOwn);
      poll(kConnOnLpctl, kLpcrHostOwnSync, kLpcrHostOwnSync, 50);
      wr(kConnOnLpctl, kLpcrHostClrOwn);
      if (poll(kConnOnLpctl, kLpcrHostOwnSync, 0, 50)) break;
    }
  }
  if (i == kDrvOwnRetries) {
    error = format("driver own failed (CONN_ON_LPCTL %08x)", rr(kConnOnLpctl));
    return false;
  }
  const uint32_t chip = l1_rr(kHwChipId), rev = l1_rr(kHwRev);
  _chip = static_cast<uint16_t>(chip);
  _rev = static_cast<uint8_t>(rev);
  log(format("mt7902: ASIC %04x revision %02x (CHIPID %08x, REV %08x)", _chip, _rev, chip, rev));
  if (_chip != 0x7902) {
    error = format("chip id %04x is not an MT7902", _chip);
    return false;
  }
  return !_proxy.failed();
}

bool Mt7902::dma_disable(bool force) { /* mt792x_dma_disable */
  clear(kGloCfg, kGloTxDmaEn | kGloRxDmaEn | kGloDispBasePtrChainEn | kGloOmitTxInfo |
                     kGloOmitRxInfo | kGloOmitRxInfoPfet2);
  if (!poll(kGloCfg, kGloTxDmaBusy | kGloRxDmaBusy, 0, 100)) return false;
  clear(kGloCfgExt0, kGloCfgExt0TxDmashdlEn);
  set(kDmashdlSwControl, kDmashdlBypass);
  if (force) {
    clear(kWfdma0Rst, kWfdma0RstDmashdlAllRst | kWfdma0RstLogicRst);
    set(kWfdma0Rst, kWfdma0RstDmashdlAllRst | kWfdma0RstLogicRst);
  }
  _dma_on = false;
  return true;
}

void Mt7902::dma_enable() { /* mt792x_dma_enable, MT7902 prefetch */
  const uint32_t rx_pf[][2] = {{0, 0x0000}, {1, 0x0040}, {2, 0x0080}, {3, 0x00c0}};
  const uint32_t tx_pf[][2] = {{0, 0x0100}, {1, 0x0140}, {2, 0x0180},  {3, 0x01c0}, {4, 0x0200},
                               {5, 0x0240}, {6, 0x0280}, {15, 0x02c0}, {16, 0x0300}};
  for (const auto &p : rx_pf) wr(rx_ring_ext_ctrl(p[0]), prefetch(p[1], 0x4));
  for (const auto &p : tx_pf) wr(tx_ring_ext_ctrl(p[0]), prefetch(p[1], 0x4));
  wr(kRstDtxPtr, ~0u);
  wr(kPriDlyIntCfg0, 0);
  set(kGloCfg, kGloTxWbDdone | kGloFifoLittleEndian | kGloClkGatDis | kGloOmitTxInfo |
                   (3u << kGloDmaSizeShift) | kGloFifoDisCheck | kGloRxWbDdone |
                   kGloDispBasePtrChainEn | kGloOmitRxInfoPfet2);
  set(kGloCfg, kGloTxDmaEn | kGloRxDmaEn);
  set(kWfdmaDummyCr, kWfdmaNeedReinit);
  /* No interrupts: nobody here would take them. The rings are polled. */
  wr(kHostIntEna, 0);
  _dma_on = true;
}

bool Mt7902::alloc_tx(TxRing &r, unsigned hw, unsigned n, unsigned nbufs, std::string &error) {
  r.hw = hw;
  r.n = n;
  r.desc = _pool.alloc(n * sizeof(Desc), 4096);
  for (unsigned i = 0; r.desc && i < nbufs; i++) {
    DmaBuf b = _pool.alloc_page();
    if (!b) break;
    r.bufs.push_back(b);
  }
  if (!r.desc || r.bufs.size() != nbufs) {
    error = format("out of DMA pages for TX ring %u", hw);
    return false;
  }
  reset_tx(r);
  return true;
}

bool Mt7902::alloc_rx(RxRing &r, unsigned hw, unsigned n, uint32_t buf_len, std::string &error) {
  r.hw = hw;
  r.n = n;
  r.buf_len = buf_len;
  r.desc = _pool.alloc(n * sizeof(Desc), 4096);
  for (unsigned i = 0; r.desc && i < n; i++) {
    DmaBuf b = _pool.alloc(buf_len, buf_len);
    if (!b) break;
    r.bufs.push_back(b);
  }
  if (!r.desc || r.bufs.size() != n) {
    error = format("out of DMA pages for RX ring %u", hw);
    return false;
  }
  reset_rx(r);
  return true;
}

void Mt7902::reset_tx(TxRing &r) { /* mt76_dma_queue_reset + mt76_dma_sync_idx */
  for (unsigned i = 0; i < r.n; i++) {
    volatile Desc *d = desc_at(r.desc, i);
    d->buf0 = 0;
    d->buf1 = 0;
    d->info = 0;
    d->ctrl = kDescDmaDone;
  }
  wr(ring_reg(true, r.hw, kRingCpuIdx), 0);
  wr(ring_reg(true, r.hw, kRingDmaIdx), 0);
  wr(ring_reg(true, r.hw, kRingSize), r.n);
  wr(ring_reg(true, r.hw, kRingDescBase), static_cast<uint32_t>(r.desc.pa));
  r.head = rr(ring_reg(true, r.hw, kRingDmaIdx)) % r.n;
}

void Mt7902::reset_rx(RxRing &r) { /* queue reset + mt76_dma_rx_fill */
  for (unsigned i = 0; i < r.n; i++) {
    volatile Desc *d = desc_at(r.desc, i);
    d->buf0 = static_cast<uint32_t>(r.bufs[i].pa);
    d->buf1 = 0;
    d->info = 0;
    d->ctrl = r.buf_len << kDescSdLen0Shift;
  }
  wr(ring_reg(false, r.hw, kRingDmaIdx), 0);
  wr(ring_reg(false, r.hw, kRingSize), r.n);
  wr(ring_reg(false, r.hw, kRingDescBase), static_cast<uint32_t>(r.desc.pa));
  r.tail = 0;
  /* The card owns [dma_idx, cpu_idx): all but one descriptor. */
  wr(ring_reg(false, r.hw, kRingCpuIdx), r.n - 1);
}

bool Mt7902::reset_and_dma(std::string &error) {
  /* mt792x_wfsys_reset_default */
  clear(kWfsysSwRst, kWfsysSwRstB);
  Sleep(50);
  set(kWfsysSwRst, kWfsysSwRstB);
  if (!poll(kWfsysSwRst, kWfsysSwInitDone, kWfsysSwInitDone, 500)) {
    error = "WFSYS reset did not finish";
    return false;
  }
  wr(kHostIntEna, 0);
  wr(kPcieMacIntEnable, 0);

  if (!_pool.pages_locked() && !_pool.init(kDmaPages, kDmaMinPages, error, kDmaLimit)) {
    error = "DMA memory below 4 GiB: " + error;
    return false;
  }
  log(format("mt7902: %zu DMA pages below 4 GiB", _pool.pages_locked()));

  /* mt7921_dma_init with the MT7902 layout */
  if (!dma_disable(true)) {
    error = "WFDMA did not go idle";
    return false;
  }
  if (!alloc_tx(_tx_data, kTxRingData, 256, 32, error)) return false;
  wr(tx_ring_ext_ctrl(0), 0x4);
  if (!alloc_tx(_tx_mcu, kTxRingMcuWm, 256, 16, error)) return false;
  if (!alloc_tx(_tx_fwdl, kTxRingFwdl, 128, 16, error)) return false;
  if (!alloc_rx(_rx_mcu, kRxRingMcu, 256, kRxBufLen, error)) return false;
  if (!alloc_rx(_rx_data, kRxRingData, 256, kRxBufLen, error)) return false;
  /* TX tokens: one page per frame in flight (TXD+TXP, then the frame). */
  _tx_slots.clear();
  _tx_free_slots.clear();
  for (uint16_t i = 0; i < 96; i++) {
    DmaBuf p = _pool.alloc_page();
    if (!p) break;
    _tx_slots.push_back({p});
    _tx_free_slots.push_back(i);
  }
  if (_tx_slots.size() < 32) {
    error = "out of DMA pages for TX slots";
    return false;
  }
  _pool.trim();
  dma_enable();
  log(format("mt7902: WFDMA on (GLO_CFG %08x), %zu DMA pages in use", rr(kGloCfg),
             _pool.pages_used()));
  return !_proxy.failed();
}

/* ---- Rings ---- */

bool Mt7902::tx_raw(TxRing &r, const void *data, size_t len, unsigned timeout_ms) {
  if (len > DmaPool::kPageBytes) return false;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const unsigned done = rr(ring_reg(true, r.hw, kRingDmaIdx)) % r.n;
    const unsigned outstanding = (r.head + r.n - done) % r.n;
    if (outstanding + 1 < r.bufs.size()) break;
    if (_proxy.failed() || Clock::now() > deadline) {
      log(format("mt7902: TX ring %u stuck (cpu %u, dma %u)", r.hw, r.head, done));
      return false;
    }
    Sleep(0);
  }
  const DmaBuf &b = r.bufs[r.head % r.bufs.size()];
  std::memcpy(b.va, data, len);
  volatile Desc *d = desc_at(r.desc, r.head);
  d->buf0 = static_cast<uint32_t>(b.pa);
  d->buf1 = 0;
  d->info = 0;
  d->ctrl = static_cast<uint32_t>(len) << kDescSdLen0Shift | kDescLastSec0;
  r.head = (r.head + 1) % r.n;
  MemoryBarrier();
  wr(ring_reg(true, r.hw, kRingCpuIdx), r.head);
  return !_proxy.failed();
}

bool Mt7902::rx_take(RxRing &r, std::vector<uint8_t> &out) {
  out.clear();
  unsigned waits = 0;
  for (;;) {
    volatile Desc *d = desc_at(r.desc, r.tail);
    const uint32_t ctrl = d->ctrl;
    if (!(ctrl & kDescDmaDone)) {
      if (out.empty()) return false;
      /* The rest of a frame in several segments is on its way. */
      if (++waits > 100000) {
        log(format("mt7902: RX ring %u: frame cut short after %zu bytes", r.hw, out.size()));
        return true;
      }
      Sleep(0);
      continue;
    }
    MemoryBarrier();
    const uint32_t len = (ctrl >> kDescSdLen0Shift) & kDescSdLen0Mask;
    const DmaBuf &b = r.bufs[r.tail];
    out.insert(out.end(), b.va, b.va + std::min<uint32_t>(len, r.buf_len));
    const bool last = (ctrl & kDescLastSec0) != 0;
    d->buf0 = static_cast<uint32_t>(b.pa);
    d->buf1 = 0;
    d->info = 0;
    d->ctrl = r.buf_len << kDescSdLen0Shift;
    MemoryBarrier();
    wr(ring_reg(false, r.hw, kRingCpuIdx), r.tail);
    r.tail = (r.tail + 1) % r.n;
    if (last) return true;
  }
}

/* ---- MCU ---- */

bool Mt7902::mcu_send(const Command &cmd, const void *data, size_t len, bool wait,
                      std::vector<uint8_t> *answer, unsigned timeout_ms, bool log_timeout) {
  /* mt76_connac2_mcu_fill_message */
  _seq = static_cast<uint8_t>((_seq + 1) & 0xf);
  if (!_seq) _seq = 1;
  std::vector<uint8_t> msg;
  if (cmd.uni) {
    McuUniTxd u{};
    msg.resize(sizeof(u) + len);
    if (msg.size() > DmaPool::kPageBytes) return false;
    u.txd[0] = static_cast<uint32_t>(msg.size()) | kTxTypeCmd << 23 | kTxMcuPortRxQ0 << 25;
    u.txd[1] = kTxd1LongFormat | kHdrFormatCmd << 16;
    u.len = static_cast<uint16_t>(msg.size() - sizeof(u.txd));
    u.cid = cmd.cid;
    u.pkt_type = kMcuPktId;
    u.seq = _seq;
    u.s2d_index = kMcuS2dH2n;
    u.option = cmd.query ? kMcuCmdUniQueryAck : kMcuCmdUniExtAck;
    std::memcpy(msg.data(), &u, sizeof(u));
    if (len) std::memcpy(msg.data() + sizeof(u), data, len);
  }
  if (!cmd.uni) msg.resize(sizeof(McuTxd) + len);
  if (msg.size() > DmaPool::kPageBytes) return false;
  McuTxd t{};
  t.txd[0] = static_cast<uint32_t>(msg.size()) | kTxTypeCmd << 23 | kTxMcuPortRxQ0 << 25;
  t.txd[1] = kTxd1LongFormat | kHdrFormatCmd << 16;
  t.len = static_cast<uint16_t>(msg.size() - sizeof(t.txd));
  t.pq_id = static_cast<uint16_t>(kTxPortIdxMcu << 15 | kTxMcuPortRxQ0 << 10);
  t.cid = cmd.cid;
  t.pkt_type = kMcuPktId;
  t.seq = _seq;
  t.ext_cid = cmd.ext_cid;
  if (cmd.ext_cid || cmd.ce) {
    t.set_query = cmd.query ? kMcuQQuery : kMcuQSet;
    t.ext_cid_ack = cmd.ext_cid ? 1 : 0;
  } else {
    t.set_query = kMcuQNa;
  }
  t.s2d_index = kMcuS2dH2n;
  if (!cmd.uni) {
    std::memcpy(msg.data(), &t, sizeof(t));
    if (len) std::memcpy(msg.data() + sizeof(t), data, len);
  }
  if (!tx_raw(_tx_mcu, msg.data(), msg.size())) return false;
  if (!wait) return true;

  const uint8_t seq = _seq;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  std::vector<uint8_t> frame;
  unsigned idle = 0;
  while (Clock::now() < deadline) {
    if (!rx_take(_rx_mcu, frame)) {
      if (++idle > 64) Sleep(1);
      continue;
    }
    idle = 0;
    if (frame.size() >= sizeof(McuRxd)) {
      const auto *rxd = reinterpret_cast<const McuRxd *>(frame.data());
      if ((rxd->rxd[0] >> kRxd0PktTypeShift) == kPktTypeRxEvent && rxd->seq == seq) {
        if (answer) *answer = std::move(frame);
        return true;
      }
    }
    handle_rx(frame);
  }
  if (!log_timeout) return false;
  log(format("mt7902: no answer to MCU command %s%02x/%02x (seq %u); WM ring cpu %u dma %u, event ring cpu %u dma %u tail %u",
             cmd.uni ? "UNI " : "", cmd.cid, cmd.ext_cid, seq, rr(ring_reg(true, _tx_mcu.hw, kRingCpuIdx)),
             rr(ring_reg(true, _tx_mcu.hw, kRingDmaIdx)), rr(ring_reg(false, _rx_mcu.hw, kRingCpuIdx)),
             rr(ring_reg(false, _rx_mcu.hw, kRingDmaIdx)), _rx_mcu.tail));
  return false;
}

void Mt7902::poll_events() {
  std::vector<uint8_t> frame;
  while (rx_take(_rx_mcu, frame))
    handle_rx(frame);
}

int Mt7902::patch_sem(bool get) { /* mt76_connac_mcu_patch_sem_ctrl */
  const uint32_t op = get ? kPatchSemGet : kPatchSemRelease;
  std::vector<uint8_t> answer;
  Command c;
  c.cid = kMcuCmdPatchSemControl;
  if (!mcu_send(c, &op, sizeof(op), true, &answer)) return -1;
  /* mt7921_mcu_parse_response: the status byte follows the RXD less 4. */
  return answer.size() > sizeof(McuRxd) - 4 ? answer[sizeof(McuRxd) - 4] : -1;
}

bool Mt7902::init_download(uint32_t addr, uint32_t len, uint32_t mode) {
  const uint32_t req[3] = {addr, len, mode};
  Command c;
  c.cid = (addr == kMcuPatchAddress || addr == 0x900000) ? kMcuCmdPatchStartReq
                                                          : kMcuCmdTargetAddressLenReq;
  return mcu_send(c, req, sizeof(req), true);
}

bool Mt7902::send_scatter(const uint8_t *data, size_t len) { /* __mt76_mcu_send_firmware */
  while (len) {
    const size_t n = std::min(len, kScatterChunk);
    if (!tx_raw(_tx_fwdl, data, n)) return false;
    data += n;
    len -= n;
  }
  return true;
}

bool Mt7902::send_patch(const std::vector<uint8_t> &fw, std::string &error) {
  /* mt76_connac2_load_patch */
  const int sem = patch_sem(true);
  if (sem == kPatchIsDl) {
    log("mt7902: ROM patch already in place");
    return true;
  }
  if (sem != kPatchNotDlSemSuccess) {
    error = format("patch semaphore refused (%d)", sem);
    return false;
  }
  bool ok = false;
  do {
    if (fw.size() < sizeof(PatchHdr)) {
      error = "ROM patch file too short";
      break;
    }
    const auto *hdr = reinterpret_cast<const PatchHdr *>(fw.data());
    const uint32_t n_region = be32(hdr->desc.n_region);
    if (n_region > (fw.size() - sizeof(PatchHdr)) / sizeof(PatchSec)) {
      error = "ROM patch header names too many sections";
      break;
    }
    log(format("mt7902: ROM patch %.16s, platform %.4s, HW/SW %08x, %u sections", hdr->build_date,
               hdr->platform, be32(hdr->hw_sw_ver), n_region));
    bool sections_ok = true;
    for (uint32_t i = 0; i < n_region && sections_ok; i++) {
      const auto *sec = reinterpret_cast<const PatchSec *>(fw.data() + sizeof(PatchHdr) + i * sizeof(PatchSec));
      const uint32_t addr = be32(sec->addr), len = be32(sec->len), offs = be32(sec->offs);
      const uint32_t info = be32(sec->sec_key_idx);
      if ((be32(sec->type) & kPatchSecTypeMask) != kPatchSecTypeInfo || offs > fw.size() ||
          len > fw.size() - offs) {
        error = format("ROM patch section %u is malformed", i);
        sections_ok = false;
        break;
      }
      uint32_t mode = kDlModeNeedRsp; /* mt76_connac2_get_data_mode */
      if (info != kPatchSecNotSupport) {
        switch (info >> kPatchSecEncTypeShift) {
        case kPatchSecEncTypePlain:
          break;
        case kPatchSecEncTypeAes:
          mode |= kDlModeEncrypt | (((info & kPatchSecEncAesKeyMask) << kDlModeKeyIdxShift) & 0x6) |
                  kDlModeResetSecIv;
          break;
        case kPatchSecEncTypeScramble:
          mode |= kDlModeEncrypt | kDlConfigEncryModeSel | kDlModeResetSecIv;
          break;
        default:
          log("mt7902: unknown patch encryption type");
        }
      }
      log(format("mt7902: patch section %u: %u bytes to %08x, mode %08x", i, len, addr, mode));
      if (!init_download(addr, len, mode)) {
        error = format("download request for patch section %u failed", i);
        sections_ok = false;
      } else if (!send_scatter(fw.data() + offs, len)) {
        error = format("sending patch section %u failed", i);
        sections_ok = false;
      }
    }
    if (!sections_ok) break;
    const uint32_t finish = 0; /* check_crc 0 */
    Command c;
    c.cid = kMcuCmdPatchFinishReq;
    if (!mcu_send(c, &finish, sizeof(finish), true)) {
      error = "the firmware did not take the patch";
      break;
    }
    ok = true;
  } while (false);
  const int rel = patch_sem(false);
  if (rel != kPatchRelSemSuccess) {
    if (ok) error = format("patch semaphore release failed (%d)", rel);
    return false;
  }
  return ok;
}

bool Mt7902::send_ram(const std::vector<uint8_t> &fw, std::string &error) {
  /* mt76_connac2_load_ram / mt76_connac_mcu_send_ram_firmware (WM only) */
  if (fw.size() < sizeof(FwTrailer)) {
    error = "RAM firmware file too short";
    return false;
  }
  const auto *tr = reinterpret_cast<const FwTrailer *>(fw.data() + fw.size() - sizeof(FwTrailer));
  const size_t regions_at = fw.size() - sizeof(FwTrailer) - tr->n_region * sizeof(FwRegion);
  if (tr->n_region * sizeof(FwRegion) > fw.size() - sizeof(FwTrailer)) {
    error = "RAM firmware trailer names too many regions";
    return false;
  }
  _fw_version = format("%.10s-%.15s", tr->fw_ver, tr->build_date);
  log(format("mt7902: WM firmware %s, chip %02x eco %02x, %u regions", _fw_version.c_str(),
             tr->chip_id, tr->eco_code, tr->n_region));
  size_t offset = 0;
  uint32_t override = 0;
  for (unsigned i = 0; i < tr->n_region; i++) {
    const auto *region = reinterpret_cast<const FwRegion *>(fw.data() + regions_at + i * sizeof(FwRegion));
    const uint32_t len = region->len, addr = region->addr;
    if (offset + len > regions_at) {
      error = format("RAM firmware region %u runs past the data", i);
      return false;
    }
    if (!(region->feature_set & kFwFeatureNonDl)) {
      if (region->feature_set & kFwFeatureOverrideAddr) override = addr;
      uint32_t mode = kDlModeNeedRsp; /* mt76_connac_mcu_gen_dl_mode */
      if (region->feature_set & kFwFeatureSetEncrypt) mode |= kDlModeEncrypt | kDlModeResetSecIv;
      if (region->feature_set & kFwFeatureEncryMode) mode |= kDlConfigEncryModeSel;
      mode |= ((region->feature_set >> kFwFeatureSetKeyIdxShift) & 3u) << kDlModeKeyIdxShift;
      log(format("mt7902: firmware region %u: %u bytes to %08x, mode %08x", i, len, addr, mode));
      if (!init_download(addr, len, mode)) {
        error = format("download request for firmware region %u failed", i);
        return false;
      }
      if (!send_scatter(fw.data() + offset, len)) {
        error = format("sending firmware region %u failed", i);
        return false;
      }
    }
    offset += len;
  }
  const uint32_t start[2] = {override ? kFwStartOverride : 0u, override};
  Command c;
  c.cid = kMcuCmdFwStartReq;
  if (!mcu_send(c, start, sizeof(start), true)) {
    error = "the firmware did not start";
    return false;
  }
  return true;
}

bool Mt7902::load_firmware(const std::string &firmware_dir, std::string &error) {
  /* mt7921e_mcu_init: driver own through the L1 window, no L0s */
  l1_wr(kTopLpcrHostBand0, kTopLpcrHostDrvOwn);
  bool owned = false;
  for (int i = 0; i < 500 && !owned; i++) {
    owned = !(l1_rr(kTopLpcrHostBand0) & kTopLpcrHostFwOwn);
    if (!owned) Sleep(1);
  }
  if (!owned) {
    error = "timeout for driver own";
    return false;
  }
  rmw(kPcieMacPm, kPcieMacPmL0sDis, kPcieMacPmL0sDis);
  /* __mt7921_init_hardware: normal operation mode before the download */
  wr(kSwdefMode, 0);

  std::vector<uint8_t> patch, ram;
  const std::string patch_path = firmware_dir + "\\WIFI_MT7902_patch_mcu_1_1_hdr.bin";
  const std::string ram_path = firmware_dir + "\\WIFI_RAM_CODE_MT7902_1.bin";
  if (!read_file(patch_path, patch) || !read_file(ram_path, ram)) {
    error = "cannot read the MT7902 firmware in " + firmware_dir;
    return false;
  }

  /* mt792x_load_firmware */
  const uint8_t power[4] = {1, 0, 0, 0};
  Command restart;
  restart.cid = kMcuCmdNicPowerCtrl;
  mcu_send(restart, power, sizeof(power), false);
  if (!poll(kConnOnMisc, kTopMiscFwState, kTopMisc2FwPwrOn, 1000))
    log(format("mt7902: MCU is not ready for firmware download (MISC %08x)", rr(kConnOnMisc)));
  if (!send_patch(patch, error)) return false;
  if (!send_ram(ram, error)) return false;
  if (!poll(kConnOnMisc, kTopMisc2FwN9Rdy, kTopMisc2FwN9Rdy, 1500)) {
    error = format("timeout for initializing firmware (MISC %08x)", rr(kConnOnMisc));
    return false;
  }
  log("mt7902: firmware running");
  return true;
}

bool Mt7902::query_capabilities(std::string &error) { /* mt7921_mcu_get_nic_capability */
  Command c;
  c.cid = kMcuCeCmdGetNicCapab;
  c.ce = true;
  std::vector<uint8_t> a;
  if (!mcu_send(c, nullptr, 0, true, &a)) {
    error = "no answer to GET_NIC_CAPAB";
    return false;
  }
  size_t at = sizeof(McuRxd);
  if (a.size() < at + 4) {
    error = "short GET_NIC_CAPAB answer";
    return false;
  }
  const unsigned n = a[at] | a[at + 1] << 8;
  at += 4;
  for (unsigned i = 0; i < n && at + 8 <= a.size(); i++) {
    uint32_t type, len;
    std::memcpy(&type, &a[at], 4);
    std::memcpy(&len, &a[at + 4], 4);
    at += 8;
    if (len > a.size() - at) break;
    if (type == kNicCapMacAddr && len >= 6) std::memcpy(_mac, &a[at], 6);
    at += len;
  }
  log(format("mt7902: MAC address %02x:%02x:%02x:%02x:%02x:%02x (%u capability elements)", _mac[0],
             _mac[1], _mac[2], _mac[3], _mac[4], _mac[5], n));
  return true;
}

bool Mt7902::open(const std::string &firmware_dir, std::string &error) {
  return attach(error) && reset_and_dma(error) && load_firmware(firmware_dir, error) &&
         query_capabilities(error);
}

void Mt7902::close() {
  if (!_proxy.is_open()) return;
  if (_dma_on) dma_disable(true);
  /* Back to the firmware, which may then sleep. */
  wr(kConnOnLpctl, kLpcrHostSetOwn);
}
