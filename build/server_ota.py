import os
import zlib
from http.server import HTTPServer, SimpleHTTPRequestHandler

# Konfigurasi Versi Firmware (Format Hex, contoh: 0x00000001)
FIRMWARE_VERSION_HEX = "0x000100033" 
PORT = 8000

def get_esp_compatible_crc32(file_path):
    # ESP-IDF esp_crc32_le mulai dari 0, bukan 0xFFFFFFFF
    # Kita simulasikan per chunk 512 byte:
    crc = 0
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(512)
            if not chunk:
                break
            # Perhitungan bertahap ala ESP-IDF di Python
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF

class OTAHTTPRequestHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        filepath = self.translate_path(self.path)
        
        # Jika file yang diakses berakhiran .bin dan ada di disk
        if os.path.isfile(filepath) and filepath.lower().endswith('.bin'):
            # Ambil ukuran file untuk log
            file_size = os.path.getsize(filepath)
            
            # GUNAKAN FUNGSI CHUNKED CRC32 YANG BENAR
            crc32_val = get_esp_compatible_crc32(filepath)
            
            # Sisipkan Custom Header ke Response HTTP
            self.send_header('X-Firmware-CRC32', f'{crc32_val:08X}')
            self.send_header('X-Firmware-Version', FIRMWARE_VERSION_HEX)
            print(f"[OTA SERVER] File: {os.path.basename(filepath)} | Size: {file_size} bytes | CRC32: 0x{crc32_val:08X}")
            
        super().end_headers()
        
if __name__ == '__main__':
    server = HTTPServer(('0.0.0.0', PORT), OTAHTTPRequestHandler)
    print(f"Server OTA berjalan di port {PORT}... Tekan Ctrl+C untuk berhenti.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServer dihentikan.")
        server.server_close()