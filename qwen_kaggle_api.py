import os
import json
import time
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

os.environ["TOKENIZERS_PARALLELISM"] = "false"

import torch
from transformers import AutoTokenizer, AutoModelForCausalLM

MODEL_ID = "Qwen/Qwen3-4B-Instruct-2507"
HOST = "0.0.0.0"
PORT = 8000

# Fast-answer tuning used during Kaggle testing.
DEFAULT_MAX_TOKENS = 20
MAX_ALLOWED_TOKENS = 64

ALLOWED_ACTIONS = {
    "l1on",
    "l1off",
    "l2on",
    "l2off",
    "l3on",
    "l3off",
    "f1on",
    "f1off",
    "none",
}

SYSTEM_PROMPT = """You are a smart patient assistant.

Return exactly two lines:
REPLY: <short natural response>
ACTION: <one of l1on,l1off,l2on,l2off,l3on,l3off,f1on,f1off,none>

For device requests, use the matching action.
For normal questions, use none.

Available devices:
L1 = Light 1
L2 = Light 2
L3 = Light 3
F1 = Fan 1

Examples:
Turn on light 1 -> ACTION: l1on
Turn off light 1 -> ACTION: l1off
Turn on light 2 -> ACTION: l2on
Turn off light 2 -> ACTION: l2off
Turn on light 3 -> ACTION: l3on
Turn off light 3 -> ACTION: l3off
Turn on the fan -> ACTION: f1on
Turn off the fan -> ACTION: f1off

Keep replies concise.
Never invent a device action.
Never claim an action succeeded unless the backend actually executes it.
"""

print("Loading tokenizer...")
tokenizer = AutoTokenizer.from_pretrained(MODEL_ID)

print("Loading model...")
model = AutoModelForCausalLM.from_pretrained(
    MODEL_ID,
    torch_dtype=torch.float16,
    device_map="auto",
)

model.eval()
print("Model loaded:", MODEL_ID)
print("CUDA:", torch.cuda.is_available())
if torch.cuda.is_available():
    print("GPU:", torch.cuda.get_device_name(0))

generation_lock = threading.Lock()


def build_messages(messages):
    """Ensure the patient-assistant system rules are present."""
    if not messages:
        return [{"role": "system", "content": SYSTEM_PROMPT}]

    if messages[0].get("role") == "system":
        return messages

    return [{"role": "system", "content": SYSTEM_PROMPT}, *messages]


def parse_reply_and_action(answer):
    """Convert model text into the custom assistant/action message array."""
    reply = ""
    action = "none"

    for line in answer.splitlines():
        line = line.strip()

        if line.upper().startswith("REPLY:"):
            reply = line.split(":", 1)[1].strip()

        elif line.upper().startswith("ACTION:"):
            action = line.split(":", 1)[1].strip().lower()

    if action not in ALLOWED_ACTIONS:
        action = "none"

    if not reply:
        # Fallback if the model did not follow the two-line format.
        reply = answer.strip()

    return [
        {
            "role": "assistant",
            "content": reply,
        },
        {
            "role": "action",
            "content": action,
        },
    ]


class APIHandler(BaseHTTPRequestHandler):

    def send_json(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")

        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self.send_json({
                "status": "ok",
                "model": MODEL_ID,
            })
            return

        self.send_json({"error": "Not found"}, 404)

    def do_POST(self):
        if self.path != "/v1/chat/completions":
            self.send_json({"error": "Not found"}, 404)
            return

        request_start = time.perf_counter()

        try:
            content_length = int(self.headers.get("Content-Length", 0))
            raw_body = self.rfile.read(content_length)
            data = json.loads(raw_body)

            messages = data.get("messages", [])
            if not messages:
                self.send_json({"error": "messages is required"}, 400)
                return

            # Keep the current fast configuration while allowing callers to request
            # a little more output, up to the hard safety limit.
            requested_max_tokens = int(
                data.get("max_tokens", DEFAULT_MAX_TOKENS)
            )
            max_tokens = max(
                1,
                min(requested_max_tokens, MAX_ALLOWED_TOKENS),
            )

            prompt_messages = build_messages(messages)

            # Tokenization
            t0 = time.perf_counter()

            prompt = tokenizer.apply_chat_template(
                prompt_messages,
                tokenize=False,
                add_generation_prompt=True,
            )

            inputs = tokenizer(
                prompt,
                return_tensors="pt",
            ).to(model.device)

            tokenize_time = time.perf_counter() - t0

            # Generation
            t1 = time.perf_counter()

            with generation_lock:
                with torch.inference_mode():
                    outputs = model.generate(
                        **inputs,
                        max_new_tokens=max_tokens,
                        do_sample=False,
                        use_cache=True,
                    )

            generation_time = time.perf_counter() - t1

            # Decode
            t2 = time.perf_counter()

            generated_tokens = (
                outputs[0][inputs["input_ids"].shape[1]:]
            )

            answer = tokenizer.decode(
                generated_tokens,
                skip_special_tokens=True,
            ).strip()

            decode_time = time.perf_counter() - t2

            message = parse_reply_and_action(answer)

            total_time = time.perf_counter() - request_start

            print(
                f"Tokenize: {tokenize_time:.3f}s | "
                f"Generate: {generation_time:.3f}s | "
                f"Decode: {decode_time:.3f}s | "
                f"TOTAL: {total_time:.3f}s"
            )

            response = {
                "id": "kaggle-qwen",
                "object": "chat.completion",
                "created": int(time.time()),
                "model": MODEL_ID,
                "choices": [
                    {
                        "index": 0,
                        "message": message,
                        "finish_reason": "stop",
                    }
                ],
                "usage": {
                    "prompt_tokens": int(inputs["input_ids"].shape[1]),
                    "completion_tokens": int(len(generated_tokens)),
                    "total_tokens": int(
                        inputs["input_ids"].shape[1]
                        + len(generated_tokens)
                    ),
                },
                "timing": {
                    "tokenize_seconds": round(tokenize_time, 3),
                    "generation_seconds": round(generation_time, 3),
                    "decode_seconds": round(decode_time, 3),
                    "total_seconds": round(total_time, 3),
                },
            }

            self.send_json(response)

        except Exception as exc:
            self.send_json({
                "error": str(exc),
            }, 500)

    def log_message(self, format, *args):
        pass


# Start server.
try:
    server.shutdown()
    server.server_close()
except Exception:
    pass

server = ThreadingHTTPServer((HOST, PORT), APIHandler)

server_thread = threading.Thread(
    target=server.serve_forever,
    daemon=True,
)
server_thread.start()

print(f"API running at http://127.0.0.1:{PORT}")
print(f"Health: http://127.0.0.1:{PORT}/health")
print(f"Chat:   http://127.0.0.1:{PORT}/v1/chat/completions")
