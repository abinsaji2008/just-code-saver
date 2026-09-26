# Kaggle Qwen Smart Patient Assistant

Saved from the working Kaggle/Postman setup.

## Model

- Qwen/Qwen3-4B-Instruct-2507
- Kaggle GPU tested: Tesla T4
- PyTorch tested: 2.14.0+cu130
- Transformers tested: 4.51.0
- Tokenizers tested: 0.21.1

## API behavior

The API returns a custom message array:

```json
"message": [
  {
    "role": "assistant",
    "content": "Sure, light 3 is on now."
  },
  {
    "role": "action",
    "content": "l3on"
  }
]
```

For normal questions:

```json
"message": [
  {
    "role": "assistant",
    "content": "Narendra Modi is the Prime Minister of India."
  },
  {
    "role": "action",
    "content": "none"
  }
]
```

## Allowed actions

- l1on
- l1off
- l2on
- l2off
- l3on
- l3off
- f1on
- f1off
- none

## Kaggle

Run `qwen_kaggle_api.py` in a Kaggle GPU notebook.

Keep the model, tokenizer, API server and Cloudflare tunnel in the same Kaggle notebook.

The API listens on:

- GET `/health`
- POST `/v1/chat/completions`

## Cloudflare

The working test used a Cloudflare Quick Tunnel.

The public `trycloudflare.com` URL is temporary and should not be stored in code.

Start a new tunnel after a Kaggle session/tunnel ends.

## Performance

During testing on a Tesla T4:

- 24 tokens: about 1.7 s generation
- 20 tokens: about 1.37 s generation

The current server defaults to 20 tokens and caps output at 64 tokens.

## Postman

Import `postman_collection.json` and set the `base_url` variable to the current Cloudflare URL.

## Device backend

The `action` field is intended for an external action router (ESP32/Firebase/etc.).

The model/API should not claim hardware execution succeeded unless the external backend actually confirms it.
