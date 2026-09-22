import os
import socket
import threading

# Bind ke 0.0.0.0 (semua interface jaringan) di Port 5005
UDP_IP = "0.0.0.0"
UDP_PORT = 5005

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))

# ============================================================================
# DISCOVERY RESPONDER (dulunya discovery_responder.py terpisah, sekarang
# digabung di sini supaya cukup jalankan SATU script saja).
#
# ESP32 (net_discovery.c) broadcast UDP "PKM_DISCOVER_V1" ke port di bawah
# saat mencari IP server (dipakai untuk logging & OTA, supaya tidak perlu
# hardcode IP walau hotspot ganti-ganti). Thread ini mendengarkan di port
# terpisah dan membalas "PKM_HERE_V1" ke pengirimnya.
# ============================================================================
DISCOVERY_PORT = 50000
DISCOVERY_MAGIC = b"PKM_DISCOVER_V1"
DISCOVERY_REPLY = b"PKM_HERE_V1"


def discovery_responder_loop():
    disc_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    disc_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    disc_sock.bind(("0.0.0.0", DISCOVERY_PORT))

    while True:
        try:
            data, addr = disc_sock.recvfrom(1024)
        except OSError:
            continue

        if data.startswith(DISCOVERY_MAGIC):
            disc_sock.sendto(DISCOVERY_REPLY, addr)
            print(f"🔎 [DISCOVERY] Balas request dari {addr[0]}:{addr[1]}")


def clear_terminal():
    """Clear terminal history sesuai Sistem Operasi (Windows / Linux / Mac)"""
    os.system("cls" if os.name == "nt" else "clear")


# Bersihkan terminal saat script pertama kali dijalankan
clear_terminal()

# Thread discovery jalan di background selama script log ini hidup --
# daemon=True supaya otomatis ikut mati kalau script utama di-Ctrl+C.
threading.Thread(target=discovery_responder_loop, daemon=True).start()

print(f"📡 Menunggu log UDP dari ESP32 di port {UDP_PORT}...")
print(f"🔎 Discovery responder aktif di port {DISCOVERY_PORT} (untuk logging & OTA dinamis).\n")

# Daftar kata kunci pemicu yang menandakan ESP32 baru saja restart
REBOOT_MARKERS = [
    "rst:0x",  # Log khas dari ESP32 ROM Bootloader saat Reset
    "boot: ESP-IDF",  # Log awal dari bootloader ESP-IDF
    "I (0) cpu_start:",  # Log awal dari core CPU
    "SPIWP:",  # Log header bootloader awal
]

while True:
    data, addr = sock.recvfrom(2048)
    log_text = data.decode("utf-8", errors="ignore")

    # Cek apakah log memuat salah satu kata kunci reset/booting
    if any(marker in log_text for marker in REBOOT_MARKERS):
        clear_terminal()
        print(
            f"🔄 [SYSTEM] ESP32 ({addr[0]}) terdeteksi RESTART! Membersihkan history...\n"
        )

    # Tampilkan log dari ESP32
    print(f"[{addr[0]}] {log_text}", end="")    