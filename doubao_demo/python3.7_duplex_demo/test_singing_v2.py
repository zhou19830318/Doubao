"""
Singing test for Doubao realtime voice API.
Enables music mode, sends singing request, saves audio as WAV file.

Usage:
  python3 test_singing_v2.py
"""
from __future__ import annotations

import asyncio
import base64
import json
import os
import struct
import uuid
from pathlib import Path

from config import DEFAULT_PCM, load_config
from realtime_client import (
    TYPE_ERROR,
    TYPE_RESPONSE_OUTPUT_AUDIO_DELTA,
    TYPE_RESPONSE_OUTPUT_AUDIO_DONE,
    TYPE_RESPONSE_OUTPUT_AUDIO_STARTED,
    TYPE_RESPONSE_OUTPUT_TEXT_DELTA,
    TYPE_RESPONSE_OUTPUT_TEXT_DONE,
    TYPE_SESSION_CREATED,
    TYPE_RESPONSE_DONE,
    TYPE_RESPONSE_FUNCTION_CALL_ARGUMENTS_DONE,
    TYPE_TRANSCRIPTION_STARTED,
    TYPE_TRANSCRIPTION_COMPLETED,
    TYPE_TRANSCRIPTION_DELTA,
    RealtimeClient,
)


class SingingTestSession:
    def __init__(self, client: RealtimeClient):
        self.client = client
        self.running = True
        self.output_audio = bytearray()
        self.output_audio_path = Path("singing_output.wav")
        self.event_log = []
        self.api_params = {}
        self.tts_type = None

    async def start(self) -> None:
        await self.client.connect()
        await self.client.session_create()
        print(f"Session created, dialog_id={self.client.dialog_id}")

        await self.send_singing_request()

        receiver = asyncio.create_task(self.receive_loop())
        try:
            await receiver
        except asyncio.CancelledError:
            pass
        finally:
            self.running = False
            await self.client.close()
            self.save_audio_as_wav()
            self.save_event_log()
            self.print_api_summary()

    async def send_singing_request(self) -> None:
        print("Sending singing request via speech_text_buffer...")
        speech_id = str(uuid.uuid4())
        await self.client.speech_text_append(speech_id, "唱一首小星星")
        await self.client.speech_text_commit(speech_id, "")

    async def receive_loop(self) -> None:
        while self.running:
            try:
                event = await asyncio.wait_for(self.client.recv_event(), timeout=60)
            except asyncio.TimeoutError:
                print("Timeout waiting for event, stopping.")
                break
            done = await self.handle_event(event)
            if done:
                break

    async def handle_event(self, event: dict) -> bool:
        event_type = event.get("type")
        self.event_log.append(event)

        if event_type == TYPE_SESSION_CREATED:
            print(f"[session.created] dialog_id={event.get('session', {}).get('id')}")

        elif event_type == TYPE_TRANSCRIPTION_STARTED:
            print("[transcription.started]")

        elif event_type == TYPE_TRANSCRIPTION_DELTA:
            print(f"[transcription.delta] delta={event.get('delta')}")

        elif event_type == TYPE_TRANSCRIPTION_COMPLETED:
            print(f"[transcription.completed] transcript={event.get('transcript')}")

        elif event_type == TYPE_RESPONSE_OUTPUT_TEXT_DELTA:
            print(f"[response.text.delta] delta={event.get('delta')}")

        elif event_type == TYPE_RESPONSE_OUTPUT_TEXT_DONE:
            print(f"[response.text.done] text={event.get('text')}")

        elif event_type == TYPE_RESPONSE_OUTPUT_AUDIO_STARTED:
            self.tts_type = event.get("tts_type")
            print(f"[response.audio.started] tts_type={self.tts_type}")

        elif event_type == TYPE_RESPONSE_OUTPUT_AUDIO_DELTA:
            delta = event.get("delta") or ""
            try:
                data = base64.b64decode(delta)
                self.output_audio.extend(data)
            except Exception as e:
                print(f"[audio.delta] base64 decode error: {e}")

        elif event_type == TYPE_RESPONSE_OUTPUT_AUDIO_DONE:
            print(f"[response.audio.done] status={event.get('status_code')}")
            print(f"  Total audio bytes: {len(self.output_audio)}")

        elif event_type == TYPE_RESPONSE_FUNCTION_CALL_ARGUMENTS_DONE:
            print(f"[function_call] items={json.dumps(event.get('items'), ensure_ascii=False)[:500]}")
            from tools import FunctionCallItem, run_tool
            raw_items = event.get("items") or []
            calls = [
                FunctionCallItem(
                    call_id=str(item.get("call_id", "")),
                    name=str(item.get("name", "")),
                    arguments=str(item.get("arguments", "")),
                )
                for item in raw_items
            ]
            outputs = []
            for call in calls:
                try:
                    output = await run_tool(call, self.client.cfg.search_api_key if hasattr(self.client.cfg, 'search_api_key') else "")
                    outputs.append(output)
                except Exception as e:
                    print(f"[function_call] tool error: {e}")
                    outputs.append(f"{call.name} failed")
            response_items = []
            for call, output in zip(calls, outputs):
                response_items.append({
                    "type": "message",
                    "role": "tool",
                    "call_id": call.call_id,
                    "content": [{"type": "input_text", "text": output}],
                })
            await self.client.conversation_item_create(response_items)

        elif event_type == TYPE_RESPONSE_DONE:
            print(f"[response.done]")
            resp = event.get("response", {})
            usage = resp.get("usage", {})
            print(f"  usage: input_tokens={usage.get('input_tokens',0)} output_tokens={usage.get('output_tokens',0)} "
                  f"total_tokens={usage.get('total_tokens',0)}")
            return True

        elif event_type == TYPE_ERROR:
            print(f"[ERROR] {json.dumps(event.get('error'), ensure_ascii=False)}")
            return True

        return False

    def save_audio_as_wav(self) -> None:
        if not self.output_audio:
            print("No audio data to save")
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

        print(f"Saved audio to {self.output_audio_path} ({data_size} bytes, {data_size/byte_rate:.1f}s)")

    def save_event_log(self):
        log_path = Path("singing_test_events_v2.json")
        serializable = []
        for e in self.event_log:
            if isinstance(e, dict):
                serializable.append(e)
        log_path.write_text(json.dumps(serializable, ensure_ascii=False, indent=2))
        print(f"Event log saved to {log_path} ({len(serializable)} events)")

    def print_api_summary(self):
        print("\n" + "="*60)
        print("API INTERACTION SUMMARY")
        print("="*60)
        print(f"Model: {self.client.cfg.model}")
        print(f"Speaker: {self.client.cfg.speaker}")
        print(f"Endpoint: {self.client.cfg.endpoint_url}")
        print(f"TTS Type: {self.tts_type}")
        print(f"Audio Output: {self.output_audio_path}")
        print(f"Audio Size: {len(self.output_audio)} bytes")
        print("="*60)


async def main():
    cfg = load_config("config.toml")

    if not cfg.api_key:
        cfg.api_key = os.environ.get("DOUBAO_API_KEY", "")

    cfg.asr_format = DEFAULT_PCM

    print(f"Config: api_key={cfg.api_key[:8]}... model={cfg.model} speaker={cfg.speaker}")
    print(f"Endpoint: {cfg.endpoint_url}")

    client = RealtimeClient(cfg)

    original_build = client.build_session_config

    def patched_build():
        session, extension = original_build()
        if "dialog" in extension and "extra" in extension["dialog"]:
            extension["dialog"]["extra"]["enable_music"] = True
        print(f"Extension dialog.extra: {json.dumps(extension['dialog']['extra'], ensure_ascii=False)}")
        return session, extension

    client.build_session_config = patched_build

    session = SingingTestSession(client)
    await session.start()


if __name__ == "__main__":
    asyncio.run(main())
