// SPDX-License-Identifier: GPL-2.0-only
/* mtprobe: brings an MT7902 bound to the register proxy up one step at a
 * time and says how far it got, so each risk is met alone:
 *   id   register access, ownership handshake, chip id
 *   dma  + WFSYS reset, WFDMA rings in AWE pages below 4 GiB
 *   fw   + ROM patch and RAM firmware over DMA, firmware's MAC address
 * Usage: mtprobe [id|dma|fw] [--fw-dir <dir>] [--log <file>] */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <timeapi.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "mt7902.h"

namespace {

/* The newest copy of the MediaTek Windows driver's firmware: the same files
 * linux-firmware ships under mediatek/. */
std::string find_firmware_dir() {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path best;
  fs::file_time_type best_time{};
  for (const auto &e : fs::directory_iterator(R"(C:\Windows\System32\DriverStore\FileRepository)", ec)) {
    if (!e.is_directory(ec) || e.path().filename().string().rfind("mtkwl", 0) != 0) continue;
    const fs::path ram = e.path() / "WIFI_RAM_CODE_MT7902_1.bin";
    if (!fs::exists(ram, ec) || !fs::exists(e.path() / "WIFI_MT7902_patch_mcu_1_1_hdr.bin", ec)) continue;
    const auto t = fs::last_write_time(ram, ec);
    if (best.empty() || t > best_time) {
      best = e.path();
      best_time = t;
    }
  }
  return best.string();
}

} // namespace

int main(int argc, char **argv) {
  std::string step = "fw", fw_dir, log_path;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if ((a == "id" || a == "dma" || a == "fw")) step = a;
    else if (a == "--fw-dir" && i + 1 < argc) fw_dir = argv[++i];
    else if (a == "--log" && i + 1 < argc) log_path = argv[++i];
    else {
      std::fprintf(stderr, "usage: mtprobe [id|dma|fw] [--fw-dir <dir>] [--log <file>]\n");
      return 2;
    }
  }
  timeBeginPeriod(1);
  std::ofstream log_file;
  if (!log_path.empty()) log_file.open(log_path, std::ios::app);
  const auto t0 = std::chrono::steady_clock::now();
  auto log = [&](const std::string &line) {
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "[%8.3f] ", s);
    std::printf("%s%s\n", stamp, line.c_str());
    std::fflush(stdout);
    if (log_file) log_file << stamp << line << std::endl;
  };

  Mt7902 dev;
  dev.set_logger(log);
  dev.set_event_handler([&](const uint8_t *data, size_t len) {
    char line[128];
    std::snprintf(line, sizeof(line), "unclaimed MCU frame, %zu bytes, rxd0 %08x", len,
                  len >= 4 ? *reinterpret_cast<const uint32_t *>(data) : 0u);
    log(line);
  });

  std::string error;
  int stage = 0;
  bool ok = dev.attach(error);
  if (ok) stage = 1;
  if (ok && step != "id") {
    ok = dev.reset_and_dma(error);
    if (ok) stage = 2;
  }
  if (ok && step == "fw") {
    if (fw_dir.empty()) fw_dir = find_firmware_dir();
    log("firmware from " + (fw_dir.empty() ? std::string("(none found)") : fw_dir));
    ok = !fw_dir.empty() && dev.load_firmware(fw_dir, error);
    if (ok) ok = dev.query_capabilities(error);
    if (ok) stage = 3;
  }
  dev.close();
  const char *names[] = {"nothing", "chip id", "DMA rings", "firmware"};
  if (ok) log(std::string("RESULT ok: reached ") + names[stage]);
  else log(std::string("RESULT failed after ") + names[stage] + ": " + error);
  timeEndPeriod(1);
  return ok ? 0 : 1;
}
