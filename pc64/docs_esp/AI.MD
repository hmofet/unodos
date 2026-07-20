# The AI assistant

Studio's right-hand column is a chat client for a cloud AI. It knows about
UnoC and the app API (that context is sent with every question), so it can
write and fix UnoDOS apps for you. It connects to **OpenAI (ChatGPT)**,
**Anthropic (Claude)** or **Google (Gemini)** over CA-validated HTTPS.

The column appears only on a wide desktop — raise the resolution in the
Control Panel if you don't see it.

## Setup

You need an API key from your provider. Type these in the assistant's input
line (each is a slash command):

    /provider anthropic          (or: openai, gemini)
    /model claude-sonnet-4-5     (optional — a sensible default is used)
    /key sk-ant-...              (your API key)
    /save                        (writes AI.CFG so it persists)

Then just type a question and press Enter. Commands:

| Command | Meaning |
|---------|---------|
| `/provider <name>` | openai, anthropic, or gemini |
| `/model <id>` | override the model |
| `/key <value>` | set the key for the current provider |
| `/save` | persist settings to `AI.CFG` |
| `/clear` | clear the conversation |

The **AI** menu's *Ask Assistant* attaches the current file to your next
message, so you can ask "why won't this build?" with the code included.

## Defaults

| Provider | Endpoint | Default model |
|----------|----------|---------------|
| OpenAI | api.openai.com | gpt-4o-mini |
| Anthropic | api.anthropic.com | claude-sonnet-4-5 |
| Gemini | generativelanguage.googleapis.com | gemini-2.0-flash |

## Privacy and security

- **Your key is stored in plain text** in `AI.CFG` on the disk. Anyone with
  the disk can read it — use a low-value, revocable key, not your main one.
- Your questions (and any attached file) are sent to the provider you chose.
- HTTPS certificates are validated against a bundled trust store, and the
  check uses the machine's clock — if the clock is wrong, connections fail.
- If the machine has no hardware random source (RDRAND), the assistant warns
  you: TLS on that machine is not cryptographically strong.

## Requirements

A working wired network link (the assistant brings the link up and resolves
the host by DNS). Wi-Fi and USB-tethered adapters may not be supported on
every machine.
