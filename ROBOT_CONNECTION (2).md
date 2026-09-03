# Panduan Koneksi Robot ke ML Server SocaSob

Dokumentasi ini menjelaskan cara sebuah robot (seperti ESP32-CAM atau klien/script lain) terhubung dan mengirimkan data *frame* (gambar) beserta status jarak ke ML Server SocaSob yang telah di-deploy.

---

## 🌐 URL Production Server

| Service / Protokol | URL Public (HTTPS/WSS) | Local Network (IP Server) |
| :--- | :--- | :--- |
| **Socket.IO Base URL** | `https://socasob-ml.hallojanu.xyz` | `http://192.168.100.28:5000` |
| **Raw WebSocket URL** | `wss://socasob-ml.hallojanu.xyz/ws` | `ws://192.168.100.28:5000/ws` |
| **HTTP POST Frame API** | `https://socasob-ml.hallojanu.xyz/api/frame` | `http://192.168.100.28:5000/api/frame` |

---

## 1. Melalui Socket.IO (Direkomendasikan untuk Klien Python / Node.js)

Gunakan metode ini jika robot/klien menggunakan library Socket.IO.

- **URL Koneksi**: `https://socasob-ml.hallojanu.xyz` *(Transports: WebSocket & Polling)*
- **Event Name**: `robot-frame`
- **Payload Structure**:
  ```json
  {
      "robot_id": "ID_ROBOT_TERDAFTAR",
      "frame": "<bytes JPEG>",
      "distance_json": {
          "distance": "Dekat", 
          "confidence": 95
      }
  }
  ```
  *(Catatan: Nilai `distance` berupa `"Dekat"` atau `"Jauh"`, dan `robot_id` harus sudah terdaftar di backend)*.

### Contoh Code Python:
```python
import socketio

sio = socketio.Client()

# Connect ke ML Server yang sudah dideploy
sio.connect("https://socasob-ml.hallojanu.xyz", transports=["polling", "websocket"])

# Baca gambar menjadi bytes
with open("gambar.jpg", "rb") as f:
    frame_bytes = f.read()

payload = {
    "robot_id": "fadfa566",  # Sesuaikan dengan ID Robot terdaftar
    "frame": frame_bytes,
    "distance_json": {
        "distance": "Dekat",
        "confidence": 95
    }
}

sio.emit("robot-frame", payload)
```

---

## 2. Melalui Raw WebSocket (Untuk ESP32-CAM)

Untuk mikrokontroler dengan memori terbatas seperti ESP32-CAM yang menggunakan WebSocket murni tanpa library Socket.IO.

- **URL Koneksi**: 
  - Domain Public: `wss://socasob-ml.hallojanu.xyz/ws` (atau `ws://socasob-ml.hallojanu.xyz/ws`)
  - Local IP (ESP32 biasanya di jaringan lokal): `ws://192.168.100.28:5000/ws`
- **Handshake Awal**: 
  Saat terhubung, server ML akan langsung mengirim string balasan `READY` dan `OK`. 
  Jika ESP32 mengirim pesan teks `"ping"`, server akan membalas dengan `"pong"`.
- **Pengiriman Frame**:
  Kirim data **binary (bytes)** yang menyertakan header ID robot dan JPEG bytes (format packet yang di-decode oleh `decode_websocket_packet`).

---

## 3. Melalui HTTP POST (Untuk Testing / cURL / Postman)

Gunakan endpoint ini untuk mengirim frame secara instan via request HTTP POST.

- **Endpoint URL**: `https://socasob-ml.hallojanu.xyz/api/frame`

### A. Multipart Form-Data (File Upload)
- `robot_id`: `fadfa566`
- `distance`: `Dekat` / `Jauh`
- `confidence`: `95`
- `frame`: `(File JPEG/PNG)`

**Contoh cURL:**
```bash
curl -X POST https://socasob-ml.hallojanu.xyz/api/frame \
  -F "robot_id=fadfa566" \
  -F "distance=Dekat" \
  -F "confidence=95" \
  -F "frame=@/path/to/image.jpg"
```

### B. JSON Body (Base64 Image)
```json
{
  "robot_id": "fadfa566",
  "distance_json": {
    "distance": "Dekat",
    "confidence": 95
  },
  "frame_base64": "data:image/jpeg;base64,/9j/4AAQSkZJRg..."
}
```

---

## ⚠️ Penting & Keamanan

Semua frame yang masuk akan melalui verifikasi `is_robot_registered(robot_id)`. Pastikan `robot_id` (misal `"fadfa566"`) sudah terdaftar dan statusnya aktif di backend dashboard (`https://be-socasob.hallojanu.xyz`), jika tidak maka server ML akan menolak frame tersebut.
