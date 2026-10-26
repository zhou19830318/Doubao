"""
Singing test for Doubao O2.0 using official binary protocol and credentials.
"""
from __future__ import annotations

import asyncio
import gzip
import json
import os
import struct
import uuid
from pathlib import Path

import websockets

# Official binary protocol
PROTOCOL_VERSION = 0b0001
DEFAULT_HEADER_SIZE = 0b0001
CLIENT_FULL_REQUEST = 0b0001
CLIENT_AUDIO_ONLY_REQUEST = 0b0010
SERVER_FULL_RESPONSE = 0b1001
SERVER_ACK = 0b1011
SERVER_ERROR_RESPONSE = 0b1111
MSG_WITH_EVENT = 0b0100
JSON_SERIALIZATION = 0b0001
NO_SERIALIZATION = 0b0000
NO_COMPRESSION = 0b0000
GZIP_COMPRESSION = 0b0001


def generate_header(
    message_type=CLIENT_FULL_REQUEST,
    message_type_specific_flags=MSG_WITH_EVENT,
    serial_method=JSON_SERIALIZATION,
    compression_type=GZIP_COMPRESSION,
):
    header = bytearray()
    header_size = 1
    header.append((PROTOCOL_VERSION << 4) | header_size)
    header.append((message_type << 4) | message_type_specific_flags)
    header.append((serial_method << 4) | compression_type)
    header.append(0x00)
    return header


def parse_response(res):
    if isinstance(res, str):
        return {}
    protocol_version = res[0] >> 4
    header_size = res[0] & 0x0f
    message_type = res[1] >> 4
    message_type_specific_flags = res[1] & 0x0f
    serialization_method = res[2] >> 4
    message_compression = res[2] & 0x0f
    payload = res[header_size * 4:]
    result = {}
    payload_msg = None
    start = 0
    if message_type == SERVER_FULL_RESPONSE or message_type == SERVER_ACK:
        result['message_type'] = 'SERVER_FULL_RESPONSE'
        if message_type == SERVER_ACK:
            result['message_type'] = 'SERVER_ACK'
        if message_type_specific_flags & 0b0010 > 0:  # NEG_SEQUENCE
            result['seq'] = int.from_bytes(payload[:4], "big", signed=False)
            start += 4
        if message_type_specific_flags & MSG_WITH_EVENT > 0:
            result['event'] = int.from_bytes(payload[:4], "big", signed=False)
            start += 4
        payload = payload[start:]
        session_id_size = int.from_bytes(payload[:4], "big", signed=True)
        session_id = payload[4:session_id_size+4]
        result['session_id'] = str(session_id)
        payload = payload[4 + session_id_size:]
        payload_size = int.from_bytes(payload[:4], "big", signed=False)
        payload_msg = payload[4:]
    elif message_type == SERVER_ERROR_RESPONSE:
        code = int.from_bytes(payload[:4], "big", signed=False)
        result['code'] = code
        payload_size = int.from_bytes(payload[4:8], "big", signed=False)
        payload_msg = payload[8:]
    if payload_msg is None:
        return result
    if message_compression == GZIP_COMPRESSION:
        payload_msg = gzip.decompress(payload_msg)
    if serialization_method == JSON_SERIALIZATION:
        payload_msg = json.loads(str(payload_msg, "utf-8"))
    elif serialization_method != NO_SERIALIZATION:
        payload_msg = str(payload_msg, "utf-8")
    result['payload_msg'] = payload_msg
    return result


class SingingTestSession:
    def __init__(self):
        self.ws = None
        self.running = True
        self.output_audio = bytearray()
        self.output_audio_path = Path("singing_o20.wav")
        self.event_log = []
        self.tts_type = None
        self.error_info = None
        self.session_id = str(uuid.uuid4())

    async def start(self) -> None:
        # Credentials from realtime_demo config
        headers = {
            "X-Api-App-ID": os.getenv("DOUBAO_O2_APP_ID", ""),
            "X-Api-Access-Key": os.getenv("DOUBAO_O2_ACCESS_KEY", ""),
            "X-Api-Resource-Id": "volc.speech.dialog",
            "X-Api-App-Key": "PlgvMymc7f3tQnJ6",
            "X-Api-Connect-Id": str(uuid.uuid4()),
        }
        
        print(f"Session ID: {self.session_id}")
        print(f"Connecting to wss://openspeech.bytedance.com/api/v3/realtime/dialogue")
        
        try:
            self.ws = await websockets.connect(
                "wss://openspeech.bytedance.com/api/v3/realtime/dialogue",
                additional_headers=headers,
                ping_interval=None,
            )
            print(f"Connected!")
            
            # Send StartConnection
            await self.send_start_connection()
            
            # Send StartSession with enable_music=True
            await self.send_start_session()
            
            # Send singing request via ChatTextQuery (event 501)
            await self.send_chat_text_query("唱一首小星星")
            
            # Receive loop
            await self.receive_loop()
            
        except Exception as e:
            print(f"Error: {e}")
            self.error_info = {"message": str(e)}
        finally:
            if self.ws:
                await self.ws.close()
            self.save_audio_as_wav()
            self.print_summary()

    async def send_start_connection(self) -> None:
        header = generate_header()
        payload = gzip.compress(b"{}")
        msg = bytearray(header)
        msg.extend(int(1).to_bytes(4, 'big'))  # event: StartConnection
        msg.extend(len(payload).to_bytes(4, 'big'))
        msg.extend(payload)
        await self.ws.send(msg)
        
        response = await self.ws.recv()
        result = parse_response(response)
        print(f"StartConnection response: {result}")

    async def send_start_session(self) -> None:
        # StartSession with enable_music=True for singing
        start_session_req = {
            "asr": {
                "extra": {}
            },
            "tts": {
                "speaker": "zh_female_xiaohe_jupiter_bigtts",
                "audio_config": {
                    "channel": 1,
                    "format": "pcm_s16le",
                    "sample_rate": 24000
                }
            },
            "dialog": {
                "extra": {
                    "input_mod": "text",
                    "enable_music": True,
                    "enable_loudness_norm": True,
                    "recv_timeout": 30
                }
            },
            "model": "1.2.1.1"
        }
        
        print(f"\nStartSession payload:")
        print(json.dumps(start_session_req, indent=2, ensure_ascii=False))
        
        header = generate_header()
        payload = gzip.compress(json.dumps(start_session_req).encode())
        msg = bytearray(header)
        msg.extend(int(100).to_bytes(4, 'big'))  # event: StartSession
        msg.extend(len(self.session_id).to_bytes(4, 'big'))
        msg.extend(self.session_id.encode())
        msg.extend(len(payload).to_bytes(4, 'big'))
        msg.extend(payload)
        await self.ws.send(msg)
        
        response = await self.ws.recv()
        result = parse_response(response)
        print(f"\nStartSession response: {result}")

    async def send_chat_text_query(self, text: str) -> None:
        """Send ChatTextQuery (event 501) for singing request."""
        payload = {"content": text}
        print(f"\nChatTextQuery payload: {payload}")
        
        header = generate_header()
        payload_bytes = gzip.compress(json.dumps(payload).encode())
        msg = bytearray(header)
        msg.extend(int(501).to_bytes(4, 'big'))  # event: ChatTextQuery
        msg.extend(len(self.session_id).to_bytes(4, 'big'))
        msg.extend(self.session_id.encode())
        msg.extend(len(payload_bytes).to_bytes(4, 'big'))
        msg.extend(payload_bytes)
        await self.ws.send(msg)

    async def receive_loop(self) -> None:
        """Receive server responses."""
        while self.running:
            try:
                response = await asyncio.wait_for(self.ws.recv(), timeout=60)
                result = parse_response(response)
                
                if not result:
                    continue
                    
                event = result.get('event', 0)
                payload_msg = result.get('payload_msg', {})
                
                print(f"\n[recv] event={event}")
                
                if isinstance(payload_msg, dict):
                    # Text event
                    event_type = payload_msg.get('type', '')
                    print(f"  type: {event_type}")
                    
                    if event_type == 'SessionStarted':
                        print(f"  Session started!")
                        
                    elif event_type == 'ASRResponse':
                        text = payload_msg.get('text', '')
                        print(f"  ASR: {text}")
                        
                    elif event_type == 'TTSResponse':
                        self.tts_type = payload_msg.get('tts_type', '')
                        print(f"  TTS type: {self.tts_type}")
                        
                    elif event_type == 'TTSEnded':
                        print("  TTS ended")
                        self.running = False
                        
                    elif event_type == 'ChatTextQueryResponse':
                        content = payload_msg.get('content', '')
                        print(f"  Response: {content[:100]}")
                        
                    elif event_type == 'Error':
                        error_msg = payload_msg.get('message', '')
                        print(f"  ERROR: {error_msg}")
                        self.error_info = payload_msg
                        self.running = False
                        
                elif isinstance(payload_msg, bytes):
                    # Audio data
                    self.output_audio.extend(payload_msg)
                    print(f"  Audio: {len(payload_msg)} bytes (total: {len(self.output_audio)})")
                    
            except asyncio.TimeoutError:
                print("Timeout")
                break
            except websockets.ConnectionClosed as e:
                print(f"Connection closed: {e}")
                break

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

        print(f"\nSaved audio: {self.output_audio_path} ({data_size} bytes, {data_size/byte_rate:.1f}s)")

    def print_summary(self):
        print("\n" + "="*60)
        print("API INTERACTION SUMMARY (O2.0 Official)")
        print("="*60)
        print(f"Endpoint: wss://openspeech.bytedance.com/api/v3/realtime/dialogue")
        print(f"Model: 1.2.1.1")
        print(f"Voice: zh_female_xiaohe_jupiter_bigtts")
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
            print(f"\n❌ Error")


async def main():
    session = SingingTestSession()
    await session.start()


if __name__ == "__main__":
    asyncio.run(main())
