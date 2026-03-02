# Voice PE per JARVIS - Studio di Fattibilita

## Verdetto: FATTIBILE - Comprali

Il Voice PE puo replicare tutte le funzioni degli AtomS3R con qualita audio
nettamente superiore. L'architettura resta orchestrator-centrica: il device
parla direttamente al wakeword server via WebSocket, come l'AtomS3R.

---

## Confronto Hardware

| Feature | AtomS3R | Voice PE |
|---------|---------|----------|
| MCU | ESP32-S3 8MB Flash / 8MB PSRAM | ESP32-S3 16MB Flash / 8MB PSRAM |
| Microfono | 1x PDM, no DSP | 2x long-range beamforming + XMOS DSP |
| Audio processing | Nessuno (software only) | Hardware AGC, NS, AEC, IC (XMOS) |
| Wake word on-device | microWakeWord TFLite (scarso) | micro_wake_word (con audio XMOS = dovrebbe andare molto meglio) |
| Speaker | ES8311 + NS4150B amp (mono) | AIC3204 DAC + amp (stereo, 48kHz) |
| Display | TFT 128x128 (ora, temp, stato) | LED ring 12x WS2812 RGB |
| Pulsante | 1x GPIO41 | 1x center button GPIO0 + multi-click nativo |
| DND control | Long-press button (toggle) | Switch hardware mic mute (fisico ON/OFF) |
| Volume | Nessuno | Ghiera rotary encoder |
| Firmware base | Custom ESP-IDF C++ | ESPHome YAML + custom C++ component |

---

## Mapping Funzioni

### 1. DND via switch hardware mic
**Facile.** Lo switch GPIO3 ha stato persistente ON/OFF. Il custom component
invia `{"type":"state","state":"dnd"}` via WebSocket (protocollo GIA esistente).
LED ring mostra rosso quando mutato (gia implementato nel firmware stock).

### 2. Ghiera per volume Echo
**Fattibile.** Nuovo messaggio WS `{"type":"volume_change","direction":"up/down"}`.
Wakeword server o orchestrator chiama `media_player.volume_set` sull'Echo.
LED ring mostra livello volume (effetto "Volume Display" gia nel firmware).

### 3. Pulsante start/stop
**Facile.** Single press → `audio_start` via WS (protocollo esistente).
Double press → `speaker_stop` via WS (protocollo esistente, era triple-tap su AtomS3R).

---

## Architettura

```
Voice PE ──[WS persistente]──> Wakeword Server ──[relay]──> Orchestrator
                                (LAN locale)                 (LAN o VPS)
```

Identica all'AtomS3R. Il device NON parla con HA. L'orchestrator resta il
centro di tutto. HA e usato SOLO dall'orchestrator come attuatore (servizi
media_player, light, etc.).

### Wake word: strategia duale

1. **Partire con micro_wake_word on-device** - audio XMOS e molto piu pulito
   del PDM dell'AtomS3R, potrebbe finalmente funzionare bene
2. **Se non va** - switch a server-side openWakeWord (streaming continuo al
   wakeword server, stessa architettura di oggi sugli AtomS3R)
3. Il custom component supporta ENTRAMBE le modalita (flag di configurazione)

### Implementazione tecnica

**Custom ESPHome external component `jarvis_ws_audio`** (~600-800 righe C++):
- Port 1:1 della logica del `jarvis_ws_audio.c` dell'AtomS3R
- Stesse librerie: `esp_websocket_client` (ESP-IDF), `libopus`
- Stesso protocollo WS: hello, audio_start, opus binary, state, speaker_stop
- Integrazione con `micro_wake_word` di ESPHome per wake on-device
- Integrazione con `microphone` di ESPHome per accesso audio I2S
- UN solo messaggio nuovo: `volume_change` per la ghiera

### Cosa cambia nell'orchestrator

Generalizzare i ~37 punti con `source == "AtomS3R"` hardcoded:
- Aggiungere `device_type` nel DB (voice_devices table)
- `source in ("AtomS3R","VoicePE")` → meglio `source_channel == "voice"`
- `extract_location_from_device()` deve supportare naming diverso
- Nuovo messaggio `volume_change` nel ws_audio_handler

### Cosa cambia nel wakeword server

Quasi niente:
- Supportare `volume_change` message (relay o gestione diretta)
- Il resto del protocollo e identico

---

## Rischi

| Rischio | Prob. | Mitigazione |
|---------|-------|-------------|
| micro_wake_word scarso anche con XMOS | Media | Fallback a openWakeWord server-side, zero lock-in |
| Custom ESPHome component complesso | Media | Port 1:1 del codice AtomS3R, stesse librerie |
| Opus encoding su ESP32-S3 | Bassa | Stesso chipset, gia provato su AtomS3R |
| Volume dial latency (device→WS→orch→HA→Echo) | Bassa | ~100-200ms, accettabile per volume |

---

## Effort

| Componente | Effort | Note |
|------------|--------|------|
| Custom ESPHome component | Medio-Alto | ~600-800 righe C++, port del codice AtomS3R |
| Firmware YAML modifications | Basso | Rimuovere voice_assistant, aggiungere component |
| Orchestrator generalizzazione | Basso | Search & replace source checks, aggiungere device_type |
| Wakeword server | Minimo | Supportare volume_change |
| Testing | Medio | Wake word accuracy, latenza, volume control |
