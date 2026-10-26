# OpenClaw Local Model Configuration

## Adding Custom Model Providers
OpenClaw supports custom OpenAI-compatible model providers via `models.providers` in `openclaw.json`.

### Config Structure
```json
{
  "models": {
    "providers": {
      "<provider-name>": {
        "baseUrl": "http://<host>:<port>/v1",
        "api": "openai-completions",
        "apiKey": "<any-string-or-env-var>",
        "models": [
          {
            "id": "<model-id>",
            "name": "<display-name>",
            "reasoning": false,
            "input": ["text"],
            "cost": {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0},
            "contextWindow": 8192,
            "maxTokens": 4096
          }
        ]
      }
    }
  }
}
```

### Supported API Types
- `openai-completions` — OpenAI-compatible (vLLM, LM Studio, llama.cpp, etc.)
- `openai-responses` — OpenAI responses API
- `anthropic-messages` — Anthropic API
- `google-generative-ai` — Google API
- `ollama` — Native Ollama API
- `bedrock-converse-stream` — AWS Bedrock
- `github-copilot` — GitHub Copilot

### Model Reference Format
Models are referenced as `<provider>/<model-id>`, e.g., `lmstudio/smollm3-3b`.

### Primary vs Fallback
```json
{
  "agents": {
    "defaults": {
      "model": {
        "primary": "anthropic/claude-sonnet-4-6",
        "fallbacks": ["lmstudio/smollm3-3b"]
      },
      "models": {
        "lmstudio/smollm3-3b": {}
      }
    }
  }
}
```

### Per-Agent Override
Individual agents can have their own model configuration:
```json
{
  "agents": {
    "list": [
      {
        "id": "my-agent",
        "model": {
          "primary": "lmstudio/smollm3-3b"
        }
      }
    ]
  }
}
```

### Verification
```bash
openclaw models    # Lists all configured models, auth status, fallbacks
openclaw tui       # Interactive terminal to test models
```

### Current Setup (HeyClawy Project)
- **Primary**: `anthropic/claude-sonnet-4-6` (Claude Sonnet)
- **Fallback**: `lmstudio/smollm3-3b` (Local SmolLM3 3B at `<LM_STUDIO_HOST>:<LM_STUDIO_PORT>`)
- **Provider**: LM Studio API at `http://<LM_STUDIO_HOST>:<LM_STUDIO_PORT>/v1`
- **Cost**: $0 (local model)

### Source Code References
- Provider schema: `src/config/zod-schema.core.ts` (lines 58-91)
- API types: `src/config/types.models.ts` (lines 1-8)
- Model resolution: `src/agents/agent-scope.ts` (lines 135-176)
- vLLM setup wizard: `src/commands/vllm-setup.ts` (example structure)
