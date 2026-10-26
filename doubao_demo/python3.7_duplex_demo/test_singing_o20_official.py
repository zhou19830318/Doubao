"""
Singing test for Doubao O2.0 (model 1.2.1.1) using correct endpoint and auth.
Based on official documentation: 豆包语音_端到端实时语音大模型API接入文档
"""
from __future__ import annotations

import asyncio
import base64
import json
import struct
import uuid
from pathlib import Path

import websockets

# Official endpoint for O2.0 (model 1.2.1.1)
ENDPOINT = "wss://openspeech.bytedance.com/api/v3/realtime/dialogue"

# Required headers for O2.0
HEADERS = {
    "X-Api-App-ID": "123456789",  # Need to get from Volcengine console
    "X-Api-AccessKey": "your-accesskey",  # Need to get from Volcengine console
    "X-ApiResource-Id": "volc.speech.dialog",
    "X-Api-App-Key": "PlgvMymc7f3tQnJ6",
}

# Model and voice
MODEL = "1.2.1.1"
VOICE = "zh_female_xiaohe_jupiter_bigtts"


class SingingTestSession:
    def __init__(self):
        self.ws = None
        self.running = True
        self.output_audio = bytearray()
        self.output_audio_path = Path("singing_o20_official.wav")
        self.event_log = []
        self.tts_type = None
        self.error_info = None
        self.session_id = str(uuid.uuid4())
        self.event_seq = 0

    async def start(self) -> None:
        print(f"Connecting to {ENDPOINT}")
        print(f"Headers: {json.dumps(HEADERS, indent=2)}")
        
        try:
            self.ws = await websockets.connect(
                ENDPOINT,
                additional_headers=HEADERS,
                ping_interval=30,
                ping_timeout=10,
            )
            print("Connected!")
            
            await self.send_start_session()
            await self.send_singing_request()
            
            receiver = asyncio.create_task(self.receive_loop())
            try:
                await receiver
            except asyncio.CancelledError:
                pass
            finally:
                self.running = False
                if self.ws:
                    await self.ws.close()
                self.save_audio_as_wav()
                self.save_event_log()
                self.print_summary()
                
        except Exception as e:
            print(f"Connection error: {e}")
            self.error_info = {"message": str(e)}
            self.print_summary()

    def new_event_id(self) -> str:
        self.event_seq += 1
        return f"event_{self.event_seq}"

    async def send_start_session(self) -> None:
        """Send StartSession event to initialize the session."""
        event = {
            "event": "StartSession",
            "session_id": self.session_id,
            "payload": {
                "tts": {
                    "audio_config": {
                        "channel": 1,
                        "format": "pcm_s16le",
                        "sample_rate": 24000,
                    },
                    "speaker": VOICE,
                },
                "asr": {
                    "extra": {}
                },
                "dialog": {
                    "extra": {
                        "input_mod": "text",
                        "enable_music": True,
                        "enable_loudness_norm": True,
                    }
                },
                "model": MODEL,
            }
        }
        
        print(f"\nSending StartSession:")
        print(json.dumps(event, indent=2, ensure_ascii=False))
        
        await self.ws.send(json.dumps(event))

    async def send_singing_request(self) -> None:
        """Send singing request via TaskRequest event."""
        event = {
            "event": "TaskRequest",
            "session_id": self.session_id,
            "payload": {
                "text": "唱一首小星星"
            }
        }
        
        print(f"\nSending TaskRequest:")
        print(json.dumps(event, indent=2, ensure_ascii=False))
        
        await self.ws.send(json.dumps(event))

    async def receive_loop(self) -> None:
        while self.running:
            try:
                message = await asyncio.wait_for(self.ws.recv(), timeout=60)
                
                if isinstance(message, bytes):
                    # Binary audio data
                    self.output_audio.extend(message)
                    print(f"[audio] received {len(message)} bytes")
                else:
                    # JSON text event
                    event = json.loads(message)
                    done = await self.handle_event(event)
                    if done:
                        break
                        
            except asyncio.TimeoutError:
                print("Timeout waiting for event, stopping.")
                break
            except websockets.ConnectionClosed as e:
                print(f"Connection closed: {e}")
                break

    async def handle_event(self, event: dict) -> bool:
        event_type = event.get("event", "")
        self.event_log.append(event)
        
        print(f"\n[recv] event={event_type}")
        print(f"  payload: {json.dumps(event.get('payload', {}), ensure_ascii=False)[:200]}")
        
        if event_type == "SessionStarted":
            print(f"  session_id={event.get('session_id')}")
            
        elif event_type == "ASRInfo":
            pass  # ASR info
            
        elif event_type == "ASRResponse":
            transcript = event.get("payload", {}).get("text", "")
            print(f"  transcript: {transcript}")
            
        elif event_type == "ASREnded":
            pass  # ASR ended
            
        elif event_type == "TTSResponse":
            self.tts_type = event.get("payload", {}).get("tts_type", "")
            print(f"  tts_type: {self.tts_type}")
            
        elif event_type == "TTSEnded":
            print("  TTS ended")
            return True
            
        elif event_type == "TaskRequest":
            pass  # Task request acknowledged
            
        elif event_type == "Error":
            error = event.get("payload", {})
            self.error_info = error
            print(f"  ERROR: {error}")
            return True
            
        elif event_type == "SessionFinished":
            print("  Session finished")
            return True
            
        return False

    def save_audio_as_wav(self) -> None:
        if not self.output_audio:
            print("\nNo audio data to save")
            return

        sample_rate = 24000
        bits_per_sample = 16
        channels = 1
        byte_rate = sample_rate * channels * bits_per_sample // 8
        block_align = channels * bits_per_sample // 8
        data_size = len(self.output_audio)

        with open(self.output_audio_path, "wb") as f:
            f.write(b"RIFF")
            f.write(struct.pack("<I", 36 + data_size))
            f.write(b"WAVE")
            f.write(b"fmt ")
            f.write(struct.pack("<I", 16))
            f.write(struct.pack("<H", 1))
            f.write(struct.pack("<H", channels))
            f.write(struct.pack("<I", sample_rate))
            f.write(struct.pack("<I", byte_rate))
            f.write(struct.pack("<H", block_align))
            f.write(struct.pack("<H", bits_per_sample))
            f.write(b"data")
            f.write(struct.pack("<I", data_size))
            f.write(self.output_audio)

        print(f"Saved audio: {self.output_audio_path} ({data_size} bytes, {data_size/byte_rate:.1f}s)")

    def save_event_log(self):
        log_path = Path("singing_o20_official_events.json")
        log_path.write_text(json.dumps(self.event_log, ensure_ascii=False, indent=2))

    def print_summary(self):
        print("\n" + "="*60)
        print("API INTERACTION SUMMARY (O2.0 Official)")
        print("="*60)
        print(f"Endpoint: {ENDPOINT}")
        print(f"Model: {MODEL}")
        print(f"Voice: {VOICE}")
        print(f"Session ID: {self.session_id}")
        print(f"Request: '唱一首小星星'")
        print(f"Input Mode: text")
        print(f"enable_music: True")
        print(f"TTS Type: {self.tts_type}")
        print(f"Audio Output: {self.output_audio_path}")
        print(f"Audio Size: {len(self.output_audio)} bytes")
        if self.error_info:
            print(f"Error: {json.dumps(self.error_info, ensure_ascii=False)}")
        print("="*60)
        
        if self.tts_type == "sing":
            print("\n✅ SINGING MODE ACTIVATED!")
        elif self.tts_type == "chat_tts_text":
            print("\n⚠️  Regular TTS mode (not singing)")
        elif self.error_info:
            print(f"\n❌ Error: {self.error_info.get('message', 'Unknown error')}")


async def main():
    session = SingingTestSession()
    await session.start()


if __name__ == "__main__":
    asyncio.run(main())
