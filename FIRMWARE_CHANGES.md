# Modifiche Firmware Voice PE per JARVIS

Partendo dal YAML ufficiale `home-assistant-voice.yaml`, queste sono le
modifiche necessarie per integrare il Voice PE nel sistema JARVIS.

L'approccio: **rimuovere** `voice_assistant` (che parla con HA) e usare il
custom component `jarvis_ws_audio` (che parla direttamente con il wakeword
server via WebSocket, identico all'architettura AtomS3R).

---

## 1. Struttura File

```
nabuvoice/
├── jarvis-voice-pe.yaml                      # Fork del firmware
├── esphome/
│   └── components/
│       └── jarvis_ws_audio/
│           ├── __init__.py                   # Registrazione ESPHome
│           ├── jarvis_ws_audio.h             # Header C++
│           └── jarvis_ws_audio.cpp           # Implementazione C++
├── FEASIBILITY.md
├── ARCHITECTURE.md
├── FIRMWARE_CHANGES.md                       # (questo file)
├── ORCHESTRATOR_CHANGES.md
└── WAKEWORD_SERVER_CHANGES.md
```

---

## 2. Modifiche al YAML

### 2.1 Aggiungere External Component

```yaml
external_components:
  - source:
      type: local
      path: esphome/components

jarvis_ws_audio:
  id: jarvis_audio
  server_url: "ws://wakeword-server.local:8200/ws/audio"
  device_token: !secret jarvis_device_token
  microphone: i2s_mics
  wakeword_mode: local    # "local" o "server"
  firmware_version: "1.0.0-voicepe"
```

### 2.2 Rimuovere voice_assistant

**RIMUOVERE** l'intero blocco `voice_assistant:` (~100 righe). Questo
componente parla con HA tramite API nativa — noi non ne abbiamo bisogno
perche il custom component parla direttamente con il wakeword server.

```yaml
# RIMUOVERE TUTTO QUESTO:
# voice_assistant:
#   id: voice_assistant_id
#   microphone: ...
#   noise_suppression_level: ...
#   auto_gain: ...
#   volume_multiplier: ...
#   on_start: ...
#   on_stt_end: ...
#   on_error: ...
#   on_end: ...
```

### 2.3 Rimuovere api: e ota: (opzionale)

Il Voice PE stock usa l'API nativa HA per comunicare. Nel nostro caso:
- `api:` si puo tenere per OTA updates e monitoring da HA, ma NON e nel
  path audio. Consiglio di tenerlo per comodita.
- `ota:` tenere per aggiornamenti firmware via rete.

```yaml
# TENERE: utile per OTA e monitoring, non nel path audio
api:
  id: api_server
  on_client_connected: ...

ota:
  - platform: esphome
    id: ota_esphome
```

### 2.4 Modificare micro_wake_word

**PRIMA (originale):** 3 modelli, triggera voice_assistant
```yaml
micro_wake_word:
  models:
    - model: okay_nabu
    - model: hey_jarvis
    - model: hey_mycroft
  on_wake_word_detected:
    - voice_assistant.start:  # <-- triggera HA pipeline
```

**DOPO:** Solo hey_jarvis, triggera jarvis_ws_audio
```yaml
micro_wake_word:
  id: mww
  microphone:
    microphone: i2s_mics
    channels: 1
    gain_factor: 4
  stop_after_detection: false
  models:
    - model: hey_jarvis
      id: hey_jarvis
    # Stop word per interrompere risposte lunghe
    - model: https://github.com/kahrendt/microWakeWord/releases/download/stop/stop.json
      id: stop
      internal: true
  vad:
  on_wake_word_detected:
    - if:
        condition:
          lambda: return x == "hey_jarvis";
        then:
          # Suona beep
          - script.execute:
              id: play_sound
              priority: true
              sound_file: "wake_word_detected_sound"
          # Avvia sessione audio via WebSocket
          - jarvis_ws_audio.start_session: jarvis_audio
          # LED: stato "listening"
          - script.execute:
              id: control_leds
    - if:
        condition:
          lambda: return x == "stop";
        then:
          # Stop la sessione o lo speaker
          - jarvis_ws_audio.speaker_stop: jarvis_audio
```

### 2.5 Modificare Center Button (GPIO0)

**Single Press → avvia conversazione (come AtomS3R)**
```yaml
# PRIMA: voice_assistant.start
# DOPO: jarvis_ws_audio.start_session
- timing:
    - ON for at most 1s
    - OFF for at least 0.25s
  then:
    - if:
        condition:
          lambda: return !id(init_in_progress) && !id(color_changed) && !id(group_volume_changed);
        then:
          - script.execute:
              id: play_sound
              priority: true
              sound_file: "center_button_press_sound"
          # Avvia sessione audio JARVIS (non HA voice_assistant)
          - jarvis_ws_audio.start_session: jarvis_audio
```

**Double Press → stop Echo speaker (era triple-tap su AtomS3R)**
```yaml
- timing:
    - ON for at most 1s
    - OFF for at most 0.25s
    - ON for at most 1s
    - OFF for at least 0.25s
  then:
    - if:
        condition:
          lambda: return !id(init_in_progress) && !id(color_changed) && !id(group_volume_changed);
        then:
          - script.execute:
              id: play_sound
              priority: false
              sound_file: "center_button_double_press_sound"
          # Stop Echo speaker via orchestrator
          - jarvis_ws_audio.speaker_stop: jarvis_audio
          # Mantieni evento per debug
          - event.trigger:
              id: button_press_event
              event_type: "double_press"
```

### 2.6 Modificare Hardware Mute Switch (GPIO3) → DND

**PRIMA:** Solo muta/unmuta microfoni locali
**DOPO:** Muta microfoni + notifica DND all'orchestrator via WS

```yaml
- platform: gpio
  id: hardware_mute_switch
  internal: true
  pin: GPIO3
  on_press:
    - if:
        condition:
          - switch.is_off: master_mute_switch
        then:
          - script.execute:
              id: play_sound
              priority: false
              sound_file: "mute_switch_on_sound"
    # AGGIUNTO: Notifica DND via WebSocket
    - jarvis_ws_audio.send_dnd:
        id: jarvis_audio
        enabled: true
  on_release:
    - script.execute:
        id: play_sound
        priority: false
        sound_file: "mute_switch_off_sound"
    - microphone.unmute:
    # AGGIUNTO: Rimuovi DND via WebSocket
    - jarvis_ws_audio.send_dnd:
        id: jarvis_audio
        enabled: false
```

### 2.7 Modificare Rotary Encoder → Volume Echo via WS

**PRIMA:** Controlla volume speaker interno del Voice PE
**DOPO:** Invia volume_change al wakeword server → orchestrator → HA → Echo

```yaml
# control_volume script: sostituire media_player.volume_up/down con WS
- id: control_volume
  mode: restart
  parameters:
    increase_volume: bool
  then:
    - delay: 16ms
    # Invia al server via WebSocket (orchestrator gestira l'Echo)
    - lambda: |-
        auto dir = increase_volume ? "up" : "down";
        id(jarvis_audio).send_volume_change(dir);
    # LED feedback: mostra livello volume per 1s
    - script.execute: control_leds
    - delay: 1s
    - lambda: id(dial_touched) = false;
    - sensor.rotary_encoder.set_value:
        id: dial
        value: 0
    - script.execute: control_leds
```

### 2.8 Aggiornare LED Ring per Stati JARVIS

Il firmware stock ha gia animazioni LED per tutti gli stati voice_assistant.
Dobbiamo rewirare i trigger dai callback voice_assistant ai callback del
nostro custom component.

**Mappatura stati JARVIS → LED effects:**

| Stato JARVIS (via WS) | LED Effect (gia esistente) |
|------------------------|---------------------------|
| idle                   | "Idle" (breathing azzurro) |
| wake_detected          | "Wake Word" (flash bianco) |
| listening (streaming)  | "Listening" (pulsante blu) |
| busy (processing)      | "Thinking" (rotazione) |
| tts_playing            | "Replying" (onda verde) |
| error                  | "Error" (flash rosso) |
| dnd                    | "Muted" (LED ring rosso fisso) |

Il custom component imposta variabili globali che `control_leds` legge:
```yaml
globals:
  # AGGIUNGERE: stato corrente per LED
  - id: jarvis_state
    type: std::string
    initial_value: '"idle"'
```

Il script `control_leds` (gia esistente) va modificato per leggere
`jarvis_state` oltre ai flag esistenti.

### 2.9 Wake Word Sensitivity

```yaml
select:
  - platform: template
    name: "Wake word sensitivity"
    optimistic: true
    initial_option: Moderately sensitive   # Default piu alto (XMOS audio)
    restore_value: true
    entity_category: config
    options:
      - Slightly sensitive
      - Moderately sensitive
      - Very sensitive
    on_value:
      lambda: |-
        if (x == "Slightly sensitive") {
          id(hey_jarvis).set_probability_cutoff(247);
        } else if (x == "Moderately sensitive") {
          id(hey_jarvis).set_probability_cutoff(235);
        } else if (x == "Very sensitive") {
          id(hey_jarvis).set_probability_cutoff(212);
        }
```

---

## 3. Componente Custom C++ (jarvis_ws_audio)

Gia creato in `esphome/components/jarvis_ws_audio/`. Port 1:1 del codice
AtomS3R con le seguenti differenze:

| Aspetto | AtomS3R | Voice PE |
|---------|---------|----------|
| Framework | ESP-IDF puro | ESPHome Component |
| Audio input | jarvis_audio ring buffer | ESPHome microphone callback |
| Opus | libopus statico | libopus da PlatformIO/build |
| WS client | esp_websocket_client | esp_websocket_client (identico) |
| Wake word | callback da jarvis_audio | callback da micro_wake_word |
| Azioni UI | Funzioni C dirette | YAML automation actions |

### API pubblica del componente

```cpp
// Queste sono chiamabili da YAML tramite le action classes
void start_session();           // → {"type":"audio_start"} + streaming
void stop_session();            // → {"type":"audio_end"}
void send_speaker_stop();       // → {"type":"speaker_stop"}
void send_dnd(bool enabled);    // → {"type":"state","state":"dnd|idle"}
void send_volume_change(dir);   // → {"type":"volume_change","direction":"up|down"}
void send_state(state);         // → {"type":"state","state":"..."}
```

### Callback dal server (gestiti internamente)

```
wake_detected → imposta wake_detected_pending_ (processato in loop)
trigger_listen → imposta trigger_listen_pending_ (multi-turn)
tts_done → imposta tts_done_pending_ (fine riproduzione)
config_update → imposta config_update_pending_ (nuova sensitivity)
ready → conferma sessione attiva
speech_end → server ha rilevato fine parlato
```

---

## 4. Cose da NON Toccare nel Firmware

Il firmware stock ha molte funzionalita che restano invariate:

- **XMOS DSP configuration** (voice_kit component) → tenere default
- **Media player** (speaker AIC3204) → tenere per beep/suoni locali
- **LED ring** (WS2812 12x) → tenere tutte le animazioni, rewirare trigger
- **OTA** → tenere per aggiornamenti
- **WiFi** → tenere configurazione
- **I2S microphone** → tenere, usato sia da micro_wake_word che dal nostro component
- **Rotary encoder hardware** → tenere, solo cambiare action nel script
- **Button hardware** → tenere multi-click, solo cambiare actions
- **Mute switch hardware** → tenere, aggiungere DND notification
- **Sound files** → tenere tutti i suoni di feedback
- **Init sequence** → tenere (boot sound, XMOS setup, etc.)

---

## 5. XMOS DSP Configuration

Default consigliato per JARVIS:

```yaml
voice_kit:
  id: voice_kit_component
  i2c_id: internal_i2c
  reset_pin: GPIO4
  firmware:
    url: https://github.com/esphome/voice-kit-xmos-firmware/releases/download/v1.3.1/ffva_v1.3.1_upgrade.bin
    version: "1.3.1"
    md5: 964635c5bf125529dab14a2472a15401
```

Per ambienti rumorosi (cucina con TV vicina):
- Channel 0: AEC (cancella suono Echo)
- Channel 1: NS (Noise Suppression)

---

## 6. Naming Convention

```yaml
esphome:
  name: jarvis-voice
  friendly_name: "JARVIS Voice"
  name_add_mac_suffix: true   # → jarvis-voice-a1b2c3
```

Un singolo YAML per tutti i device. La configurazione per-stanza (quale Echo
associare, quale room) e gestita dall'orchestrator tramite il database
voice_devices, NON nel firmware.

---

## 7. Build e Flash

```bash
# Prima compilazione (USB):
esphome compile jarvis-voice-pe.yaml
esphome upload jarvis-voice-pe.yaml --device /dev/ttyUSB0

# Aggiornamenti successivi (OTA):
esphome upload jarvis-voice-pe.yaml --device jarvis-voice-XXXX.local
```

**Nota libopus:** Il componente ha bisogno di libopus compilata per ESP32-S3.
Opzioni:
1. PlatformIO lib_deps con build da sorgente
2. Pre-compilata come .a statica nella cartella del componente
3. ESP-IDF component registry (se disponibile)

La stessa identica libreria usata sull'AtomS3R funziona, stesso chipset.

---

## 8. Checklist Implementazione Firmware

- [ ] Fork YAML ufficiale → `jarvis-voice-pe.yaml`
- [ ] Rimuovere blocco `voice_assistant:` completo
- [ ] Aggiungere `external_components` + `jarvis_ws_audio:` config
- [ ] Modificare `micro_wake_word` → solo hey_jarvis, trigger jarvis_ws_audio
- [ ] Modificare center button single press → `jarvis_ws_audio.start_session`
- [ ] Modificare center button double press → `jarvis_ws_audio.speaker_stop`
- [ ] Aggiungere DND su hardware_mute_switch → `jarvis_ws_audio.send_dnd`
- [ ] Modificare `control_volume` → `jarvis_ws_audio.send_volume_change`
- [ ] Aggiungere globale `jarvis_state` per LED control
- [ ] Rewirare `control_leds` per leggere `jarvis_state`
- [ ] Compilare e testare con libopus
- [ ] Testare connessione WS al wakeword server
- [ ] Testare flusso completo: wake → listen → STT → TTS
