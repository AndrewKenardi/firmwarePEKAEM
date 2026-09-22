# pip install websockets
import asyncio, websockets

URL = "wss://socasob-ml.hallojanu.xyz/ws"
ROBOT_ID = b"dummyrobot01"

async def listen(ws):
    try:
        async for msg in ws:
            print("  <- server:", msg)
    except websockets.ConnectionClosed as e:
        print("  koneksi ditutup:", e.code, e.reason)

async def main():
    jpeg = open("test.jpg", "rb").read()
    print("ukuran jpeg:", len(jpeg), "byte, head:", jpeg[:2].hex(), "tail:", jpeg[-2:].hex())
    async with websockets.connect(URL) as ws:
        task = asyncio.create_task(listen(ws))
        for is_dekat in (0, 1, 0):
            pkt = bytes([len(ROBOT_ID)]) + ROBOT_ID + bytes([is_dekat]) + jpeg
            print(f"kirim 20 frame, is_dekat={is_dekat}")
            for _ in range(20):
                await ws.send(pkt)
                await asyncio.sleep(0.1)
            await asyncio.sleep(2)
        await ws.close()
        await task

asyncio.run(main())