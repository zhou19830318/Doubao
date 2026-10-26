"""
Singing feature test for Doubao realtime voice API.
Uses the demo's RealtimeClient with enable_music=True.

Usage:
  python3 test_singing.py --audio <wav_file>
  python3 test_singing.py  (file input mode with dummy silence)

Tests:
  1. enable_music=True in extension
  2. Asks the model to sing a song via text
  3. Logs all events for analysis
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
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
    def __init__(self, client: RealtimeClient, audio_file: str = ""):
        self.client = client
        self.audio_file = audio_file
        self.running = True
        self.output_audio = bytearray()
        self.output_audio_path = Path("singing_test_output.pcm")
        self.event_log = []

    async def start(self) -> None:
        await self.client.connect()
        await self.client.session_create()
        print(f"Session created, dialog_id={self.client.dialog_id}")

        if self.audio_file:
            await self.send_audio_file()
        else:
            await self.send_singing_request()

        receiver = asyncio.create_task(self.receive_loop())
        try:
            await receiver
        except asyncio.CancelledError:
            pass
        finally:
            self.running = False
            await self.client.close()
            if self.output_audio:
                self.output_audio_path.write_bytes(self.output_audio)
                print(f"Saved output audio to {self.output_audio_path} ({len(self.output_audio)} bytes)")
            self.save_event_log()

    async def send_audio_file(self) -> None:
        path = Path(self.audio_file)
        data = path.read_bytes()
        if path.suffix.lower() == ".wav":
            data = data[44:]
        pos = 0
        while self.running and pos < len(data):
            chunk = data[pos : pos + 640]
            await self.client.input_audio_append(chunk)
            pos += len(chunk)
            await asyncio.sleep(0.02)

    async def send_singing_request(self) -> None:
        print("Sending singing request via speech_text_buffer...")
        speech_id = str(uuid.uuid4())
        await self.client.speech_text_append(speech_id, "请你唱一首歌，随便什么都好")
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
            print(f"[response.audio.started] tts_type={event.get('tts_type')}")

        elif event_type == TYPE_RESPONSE_OUTPUT_AUDIO_DELTA:
            delta = event.get("delta") or ""
            try:
                data = base64.b64decode(delta)
                self.output_audio.extend(data)
            except Exception as e:
                print(f"[audio.delta] base64 decode error: {e}")

        elif event_type == TYPE_RESPONSE_OUTPUT_AUDIO_DONE:
            print(f"[response.audio.done] status={event.get('status_code')}")

        elif event_type == TYPE_RESPONSE_FUNCTION_CALL_ARGUMENTS_DONE:
            print(f"[function_call] items={json.dumps(event.get('items'), ensure_ascii=False)[:500]}")
            # Auto-execute function calls
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

    def save_event_log(self):
        log_path = Path("singing_test_events.json")
        serializable = []
        for e in self.event_log:
            if isinstance(e, dict):
                serializable.append(e)
        log_path.write_text(json.dumps(serializable, ensure_ascii=False, indent=2))
        print(f"Event log saved to {log_path} ({len(serializable)} events)")


async def main():
    parser = argparse.ArgumentParser(description="Doubao singing feature test")
    parser.add_argument("--config", default="config.toml", help="Path to config.toml")
    parser.add_argument("--audio", default="", help="Audio file to send")
    args = parser.parse_args()

    cfg = load_config(args.config)

    # Override with hardcoded API key if config is empty
    if not cfg.api_key:
        cfg.api_key = os.environ.get("DOUBAO_API_KEY", "")

    cfg.asr_format = DEFAULT_PCM

    print(f"Config: api_key={cfg.api_key[:8]}... model={cfg.model} speaker={cfg.speaker}")
    print(f"Endpoint: {cfg.endpoint_url}")

    client = RealtimeClient(cfg)

    # Patch build_session_config to enable_music=True
    original_build = client.build_session_config

    def patched_build():
        session, extension = original_build()
        # Force enable_music=True for singing test
        if "dialog" in extension and "extra" in extension["dialog"]:
            extension["dialog"]["extra"]["enable_music"] = True
        print(f"Extension dialog.extra: {json.dumps(extension['dialog']['extra'], ensure_ascii=False)}")
        print(f"Session tools: {json.dumps(session.get('tools', []), ensure_ascii=False)[:200]}")
        return session, extension

    client.build_session_config = patched_build

    session = SingingTestSession(client, audio_file=args.audio)
    await session.start()


if __name__ == "__main__":
    asyncio.run(main())
