"""
server_all.py

Gabungan 3 layanan yang tadinya terpisah (server_ota.py + logReceive.py +
discovery_responder.py) jadi SATU script. Cukup jalankan:

    python server_all.py

dan ketiganya otomatis aktif bersamaan:
  1. HTTP server OTA (port 8000)      -- menyajikan file .bin + header
     X-Firmware-CRC32 / X-Firmware-Version, dipakai master_ota_task (ESP32-S)
     & vTaskUpdateManager untuk cek/download firmware.
  2. UDP log receiver (port 5005)     -- menampilkan log yang dikirim
     udp_logger.c dari ESP32, dengan auto-clear terminal saat ESP32 restart.
  3. UDP discovery responder (port 50000) -- membalas broadcast
     "PKM_DISCOVER_V1" dari net_discovery.c dengan "PKM_HERE_V1", supaya
     ESP32 bisa menemukan IP server ini secara otomatis (tidak perlu
     hardcode IP di firmware, walau IP hotspot berubah-ubah).

Ketiganya jalan di thread terpisah (HTTP server & discovery di background,
daemon=True), sementara log receiver jalan di thread utama supaya Ctrl+C
langsung menghentikan semuanya dengan bersih.
"""

import os
import socket
import threading
import zlib
from http.server import HTTPServer, SimpleHTTPRequestHandler

# ============================================================================
# KONFIGURASI
# ============================================================================

# Format Hex, contoh: 0x00010003 -- HARUS maksimal 8 digit hex (uint32_t)!
FIRMWARE_VERSION_HEX = "0x000100035"

OTA_HTTP_PORT = 8000
LOG_UDP_PORT = 5005
DISCOVERY_UDP_PORT = 50000

DISCOVERY_MAGIC = b"PKM_DISCOVER_V1"
DISCOVERY_REPLY = b"PKM_HERE_V1"

# Kata kunci pemicu yang menandakan ESP32 baru saja restart, dipakai log
# receiver untuk auto-clear terminal.
REBOOT_MARKERS = [
    "rst:0x",             # Log khas dari ESP32 ROM Bootloader saat Reset
    "boot: ESP-IDF",       # Log awal dari bootloader ESP-IDF
    "I (0) cpu_start:",    # Log awal dari core CPU
    "SPIWP:",              # Log header bootloader awal
]


def clear_terminal():
    """Clear terminal history sesuai Sistem Operasi (Windows / Linux / Mac)"""
    os.system("cls" if os.name == "nt" else "clear")


def check_firmware_version_hex():
    """Peringatkan di awal kalau FIRMWARE_VERSION_HEX tidak valid sebagai
    uint32_t -- overflow di sisi ESP32 (strtoul) bikin slave C3 dikira
    SELALU beda versi, jadi selalu di-flash ulang tiap boot."""
    try:
        value = int(FIRMWARE_VERSION_HEX, 16)
    except ValueError:
        print(f"⚠️  WARNING: FIRMWARE_VERSION_HEX ({FIRMWARE_VERSION_HEX!r}) "
              f"bukan angka hex yang valid!")
        return
    if value > 0xFFFFFFFF:
        print(f"⚠️  WARNING: FIRMWARE_VERSION_HEX ({FIRMWARE_VERSION_HEX}) "
              f"lebih dari 8 digit hex / melebihi batas uint32_t! ESP32 akan "
              f"overflow saat parsing (strtoul) -- OTA slave (C3) kemungkinan "
              f"akan SELALU dianggap beda versi & selalu di-flash ulang. "
              f"Perbaiki jadi maksimal 8 digit, mis. 0x00010003.")


# ============================================================================
# 1. HTTP SERVER OTA (port 8000)
# ============================================================================

def get_esp_compatible_crc32(file_path):
    # ESP-IDF esp_crc32_le mulai dari 0, bukan 0xFFFFFFFF.
    # Disimulasikan per chunk 512 byte, sama seperti master_ota_task di ESP32.
    crc = 0
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(512)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


class OTAHTTPRequestHandler(SimpleHTTPRequestHandler):
    def translate_path(self, path):
        # Cek apakah file yang diminta ada di dalam folder 'build' relatif
        build_path = os.path.join(os.getcwd(), "build", path.lstrip("/"))
        if os.path.exists(build_path):
            return build_path
        # Jika tidak ada di build, fallback ke direktori standar
        return super().translate_path(path)

    def end_headers(self):
        filepath = self.translate_path(self.path)

        # Jika file yang diakses berakhiran .bin dan ada di disk
        if os.path.isfile(filepath) and filepath.lower().endswith(".bin"):
            file_size = os.path.getsize(filepath)
            crc32_val = get_esp_compatible_crc32(filepath)

            self.send_header("X-Firmware-CRC32", f"{crc32_val:08X}")
            self.send_header("X-Firmware-Version", FIRMWARE_VERSION_HEX)
            print(f"[OTA SERVER] File: {os.path.basename(filepath)} | "
                  f"Size: {file_size} bytes | CRC32: 0x{crc32_val:08X}")

        super().end_headers()

def run_ota_http_server():
    server = HTTPServer(("0.0.0.0", OTA_HTTP_PORT), OTAHTTPRequestHandler)
    print(f"[OTA SERVER] berjalan di port {OTA_HTTP_PORT}...")
    server.serve_forever()


# ============================================================================
# 2. DISCOVERY RESPONDER (port 50000)
# ============================================================================

def run_discovery_responder():
    disc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    disc_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    disc_sock.bind(("0.0.0.0", DISCOVERY_UDP_PORT))

    while True:
        try:
            data, addr = disc_sock.recvfrom(1024)
        except OSError:
            continue

        if data.startswith(DISCOVERY_MAGIC):
            disc_sock.sendto(DISCOVERY_REPLY, addr)
            print(f"🔎 [DISCOVERY] Balas request dari {addr[0]}:{addr[1]}")


# ============================================================================
# 3. UDP LOG RECEIVER (port 5005) -- jalan di thread utama
# ============================================================================

def run_log_receiver():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", LOG_UDP_PORT))

    while True:
        data, addr = sock.recvfrom(2048)
        log_text = data.decode("utf-8", errors="ignore")

        if any(marker in log_text for marker in REBOOT_MARKERS):
            clear_terminal()
            print(f"🔄 [SYSTEM] ESP32 ({addr[0]}) terdeteksi RESTART! "
                  f"Membersihkan history...\n")

        print(f"[{addr[0]}] {log_text}", end="")


# ============================================================================
# MAIN
# ============================================================================

if __name__ == "__main__":
    check_firmware_version_hex()
    clear_terminal()

    print("=" * 60)
    print(f"📡 Log UDP           : port {LOG_UDP_PORT}")
    print(f"🌐 OTA HTTP server   : port {OTA_HTTP_PORT}")
    print(f"🔎 Discovery responder: port {DISCOVERY_UDP_PORT}")
    print("=" * 60)
    print("Tekan Ctrl+C untuk menghentikan semuanya.\n")

    threading.Thread(target=run_ota_http_server, daemon=True).start()
    threading.Thread(target=run_discovery_responder, daemon=True).start()

    try:
        run_log_receiver()  # blocking, di thread utama -> Ctrl+C langsung kena di sini
    except KeyboardInterrupt:
        print("\nServer dihentikan.")