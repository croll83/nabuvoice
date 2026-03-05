# NabuVoice — JARVIS Voice PE Firmware

Firmware ESPHome custom per Nabu Casa Voice PE (ESP32-S3) integrato nel sistema JARVIS.

## Prerequisiti

- Python 3.11+
- ESPHome CLI: `pip install esphome`
- File `secrets.yaml` nella root del progetto (vedi sotto)

## secrets.yaml

Creare `secrets.yaml` con:

```yaml
jarvis_server_url: "ws://<wakeword-server-ip>:8200/ws/audio"
jarvis_device_token: "<token>"
jarvis_orchestrator_url: "http://<orchestrator-ip>:5000"

wifi_ssid: "<ssid>"
wifi_password: "<password>"

api_encryption_key: "<key>"
ota_password: "<password>"
```

## Build & Flash

```bash
# Compila
esphome compile jarvis-voice-pe.yaml

# Flash via USB (prima volta)
esphome upload jarvis-voice-pe.yaml --device /dev/ttyUSB0

# Flash via OTA (aggiornamenti successivi)
esphome upload jarvis-voice-pe.yaml --device jarvis-voice-XXXX.local

# Compila + flash + log in un comando
esphome run jarvis-voice-pe.yaml --device jarvis-voice-XXXX.local

# Solo log
esphome logs jarvis-voice-pe.yaml --device jarvis-voice-XXXX.local
```

## Struttura

```
jarvis-voice-pe.yaml                     # Firmware YAML principale
secrets.yaml                             # Credenziali (non in git)
esphome/components/jarvis_ws_audio/      # Componente custom
  __init__.py                            # Codegen ESPHome
  jarvis_ws_audio.h                      # Header C++
  jarvis_ws_audio.cpp                    # Implementazione
```
