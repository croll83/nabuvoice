# Architettura Voice PE + JARVIS

## 1. Architettura Target

```
┌─────────────────────────────────────────────────────────────┐
│                      Voice PE Device                         │
│                                                              │
│  ┌──────────┐   ┌──────────┐   ┌─────────────────────────┐ │
│  │ 2x Mic   │──>│ XMOS DSP │──>│ I2S → ESP32-S3          │ │
│  │ beamform │   │ NS/AGC/  │   │                          │ │
│  └──────────┘   │ AEC      │   │  ┌──────────────────┐   │ │
│                 └──────────┘   │  │ micro_wake_word  │   │ │
│                                │  │ "hey_jarvis"     │   │ │
│  ┌──────────┐                  │  └────────┬─────────┘   │ │
│  │ Rotary   │──> volume_change │           │ on detect   │ │
│  │ Encoder  │   via WS         │  ┌────────▼─────────┐   │ │
│  └──────────┘                  │  │ jarvis_ws_audio   │   │ │
│  ┌──────────┐                  │  │ (custom component)│   │ │
│  │ Center   │──> audio_start / │  │                   │   │ │
│  │ Button   │   speaker_stop   │  │ Opus encode      │   │ │
│  └──────────┘   via WS         │  │ WS persistent    │   │ │
│  ┌──────────┐                  │  │ Same protocol    │   │ │
│  │ HW Mute  │──> state:dnd    │  │ as AtomS3R       │   │ │
│  │ Switch   │   via WS         │  └────────┬─────────┘   │ │
│  └──────────┘                  │           │              │ │
│  ┌──────────┐                  └───────────┼──────────────┘ │
│  │ LED Ring │ <── state feedback           │                │
│  │ 12x RGB  │                              │                │
│  └──────────┘                              │                │
└────────────────────────────────┬────────────┘────────────────┘
                                 │ WebSocket (Opus + JSON)
                                 │ ws://server:8200/ws/audio
                                 │
┌────────────────────────────────▼────────────────────────────┐
│                    Wakeword Server (LAN)                      │
│                                                              │
│  Ruolo: relay locale + management                            │
│  - Riceve WS da device                                       │
│  - Se mode=server: openWakeWord detection                    │
│  - Se mode=local: puro relay (niente wake detection)         │
│  - Apre relay a orchestrator su audio_start                  │
│  - Gestisce volume_change (→ orchestrator o → HA diretto)    │
│  - Multi-room cooldown                                       │
│  - REST API per trigger_listen, config push                  │
│                                                              │
└────────────────────────────────┬────────────────────────────┘
                                 │ WebSocket relay
                                 │ ws://orchestrator:5000/ws/audio
                                 │
┌────────────────────────────────▼────────────────────────────┐
│                  JARVIS Orchestrator (:5000)                  │
│                                                              │
│  Identico a oggi, con device_type generalizzato:             │
│  - Opus decode → Silero VAD → speech detection               │
│  - RNNoise denoise → Faster-Whisper STT                      │
│  - Resemblyzer speaker ID                                    │
│  - Qwen pre-routing → OpenClaw/Gemini                        │
│  - TTS via Alexa Media Player                                │
│  - Speaker suppress/restore                                  │
│  - volume_change → HA media_player.volume_set                │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

## 2. Protocollo WebSocket (invariato + 1 messaggio nuovo)

### Device → Server (identico AtomS3R)
```
{"type": "hello", "fw": "1.0.0-voicepe", "device_id": "AABBCCDDEEFF"}
{"type": "audio_start"}
{"type": "audio_end"}
{"type": "state", "state": "idle|listening|busy|dnd|error"}
{"type": "speaker_stop"}
{"type": "pong"}
[binary: Opus frames 320 samples = 20ms @ 16kHz]
```

### Device → Server (NUOVO)
```
{"type": "volume_change", "direction": "up|down"}
```

### Server → Device (identico AtomS3R)
```
{"type": "welcome", "server_time": 1234567890}
{"type": "ready", "session_id": "abc123"}
{"type": "speech_end"}
{"type": "trigger_listen", "silent": true|false}
{"type": "tts_done"}
{"type": "config_update", "wake_word_sensitivity": 0.85}
{"type": "wake_detected"}
{"type": "ping"}
```

## 3. Flusso: Comando Vocale (mode: local wake word)

```
1. XMOS DSP → beamforming + NS + AGC (hardware, ~0ms)
2. micro_wake_word detecta "jarvis" on-device (~100ms)
3. jarvis_ws_audio:
   a. Invia audio_start al wakeword server
   b. Inizia streaming Opus
4. Wakeword server:
   a. Apre relay verso orchestrator
   b. Relay audio_start + Opus frames
5. Orchestrator:
   a. Opus decode → Silero VAD → speech end
   b. RNNoise → Whisper STT → "accendi la luce"
   c. Resemblyzer speaker ID
   d. Qwen → DOMOTICA_CERTA → HA light.turn_on
   e. TTS "Acceso" via Alexa
   f. tts_done → wakeword server → device
6. Device: state → idle

Latenza stimata: ~700ms (pari a AtomS3R, audio migliore)
```

## 4. Flusso: Wake Word Server-Side (fallback)

```
1. XMOS DSP → beamforming + NS + AGC (hardware)
2. jarvis_ws_audio: streaming Opus continuo al wakeword server
3. Wakeword server: openWakeWord detecta "jarvis"
4. Server → device: {"type": "wake_detected"}
5. Device: suona beep, invia audio_start
6. Da qui: identico al flusso locale (punto 4 sopra)
```

## 5. Flusso: DND (switch mute hardware)

```
1. Utente sposta switch mute → ON (GPIO3)
2. ESPHome: muta microfoni hardware
3. jarvis_ws_audio: invia {"type":"state","state":"dnd"}
4. Wakeword server: relay → orchestrator
5. Orchestrator: registra DND, muta wakeword per device

6. Utente sposta switch → OFF
7. ESPHome: riattiva microfoni
8. jarvis_ws_audio: invia {"type":"state","state":"idle"}
9. Orchestrator: rimuove DND
```

## 6. Flusso: Volume Echo (ghiera)

```
1. Utente ruota ghiera clockwise
2. ESPHome: dial on_clockwise callback
3. jarvis_ws_audio: invia {"type":"volume_change","direction":"up"}
4. Wakeword server: relay → orchestrator (o gestisce diretto)
5. Orchestrator: lookup device → room → echo speaker
   → HA media_player.volume_set(echo_entity, +0.05)
6. LED ring: mostra livello volume per 1s
```

## 7. Flusso: Stop Echo (doppia pressione)

```
1. Utente preme center button 2 volte
2. ESPHome: on_multi_click double_press
3. jarvis_ws_audio: invia {"type":"speaker_stop"}
4. Wakeword server: relay → orchestrator
5. Orchestrator: mute_speaker_for_stop (identico al triple-tap AtomS3R)
6. LED ring: flash rosso
```

## 8. Coesistenza AtomS3R + Voice PE

```
                     ┌──────────────┐
                     │  Orchestrator │
                     └──┬───────┬───┘
                        │       │
           WS relay     │       │  WS relay
           (Opus)       │       │  (Opus)
                        │       │
                ┌───────┘       └───────┐
                │                       │
        ┌───────▼───────┐      ┌───────▼───────┐
        │ Wakeword Srv  │      │ Wakeword Srv  │
        │ (location A)  │      │ (location B)  │
        └───────┬───────┘      └───────┬───────┘
                │                       │
        ┌───────▼───────┐      ┌───────▼───────┐
        │ AtomS3R       │      │ Voice PE      │
        │ (legacy)      │      │ (new)         │
        └───────────────┘      └───────────────┘
```

Zero conflitti. Stesso protocollo, stessi server, device diversi.
L'orchestrator distingue via `device_type` nel DB, non via codice hardcoded.

## 9. Componente ESPHome Custom

```
nabuvoice/
└── esphome/
    └── components/
        └── jarvis_ws_audio/
            ├── __init__.py          # Registrazione ESPHome + YAML schema
            ├── jarvis_ws_audio.h    # Header (API pubblica)
            └── jarvis_ws_audio.cpp  # Implementazione (port da AtomS3R)
```

Configurazione YAML nel firmware:
```yaml
external_components:
  - source:
      type: local
      path: esphome/components

jarvis_ws_audio:
  server_url: "ws://wakeword-server:8200/ws/audio"
  device_token: !secret jarvis_device_token
  microphone: i2s_mics
  wakeword_mode: local    # "local" o "server"
  firmware_version: "1.0.0-voicepe"
```
