import os
import socket

# Bind ke 0.0.0.0 (semua interface jaringan) di Port 5005
UDP_IP = "0.0.0.0"
UDP_PORT = 5005

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((UDP_IP, UDP_PORT))


def clear_terminal():
    """Clear terminal history sesuai Sistem Operasi (Windows / Linux / Mac)"""
    os.system("cls" if os.name == "nt" else "clear")


# Bersihkan terminal saat script pertama kali dijalankan
clear_terminal()
print(f"📡 Menunggu log UDP dari ESP32 di port {UDP_PORT}...\n")

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