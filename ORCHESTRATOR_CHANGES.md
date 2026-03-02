# Modifiche Orchestrator per Supporto Voice PE

L'orchestrator ha ~30 punti con riferimenti hardcoded ad "AtomS3R".
Per supportare il Voice PE serve generalizzare il concetto di "voice device".

**Principio:** Ovunque c'e `source == "AtomS3R"` deve diventare
`source_channel == "voice"` o `source in VOICE_SOURCES`. Il comportamento
e identico per entrambi i device — cambia solo l'hardware, non il protocollo.

---

## 1. Nuova Costante Globale

Aggiungere in `config.py`:

```python
# Voice device sources (canale voce)
VOICE_SOURCES = {"AtomS3R", "VoicePE", "VirtualMic"}
```

---

## 2. File per File

### 2.1 main.py (~20 punti)

**Pattern dominante:** `source == "AtomS3R"` → `source in VOICE_SOURCES`
oppure `source in ("AtomS3R", "VirtualMic")` → `source in VOICE_SOURCES`

| Riga  | Attuale | Nuovo |
|-------|---------|-------|
| ~1122 | `source == "AtomS3R"` (speaker context) | `source in VOICE_SOURCES` |
| ~1213 | `source in ("AtomS3R", "VirtualMic")` (tone instruction) | `source in VOICE_SOURCES` |
| ~1476 | `build_speaker_context(audio_bytes, "AtomS3R")` | `build_speaker_context(audio_bytes, source)` |
| ~1481 | `"source": "AtomS3R"` (hardcoded context) | `"source": source` (da parametro) |
| ~2032 | `build_speaker_context(audio_bytes, "AtomS3R")` | `build_speaker_context(audio_bytes, source)` |
| ~2071 | `"source": "AtomS3R"` (ws context) | `"source": device_type` (dal hello msg) |
| ~2162 | `device_id_value == "VIRTUALMICBROWSER"` | Invariato (caso speciale browser) |
| ~2222 | `build_speaker_context(audio_bytes, "AtomS3R")` | `build_speaker_context(audio_bytes, source)` |
| ~2226 | `"VirtualMic" if is_virtual_mic else "AtomS3R"` | `"VirtualMic" if is_virtual_mic else device_type` |
| ~2364 | `source in ("AtomS3R", "VirtualMic")` (streaming TTS) | `source in VOICE_SOURCES` |
| ~2456 | `source == "AtomS3R"` (follow-up) | `source in VOICE_SOURCES` |
| ~2475 | `source in ("AtomS3R", "VirtualMic")` | `source in VOICE_SOURCES` |
| ~2990 | `source in ("AtomS3R", "VirtualMic")` (clarification) | `source in VOICE_SOURCES` |
| ~3005 | `source in ("AtomS3R", "VirtualMic")` (security) | `source in VOICE_SOURCES` |
| ~3110 | `source in ("AtomS3R", "VirtualMic")` (quick feedback) | `source in VOICE_SOURCES` |

**Funzione `extract_location_from_device()` (riga ~984):**

Attualmente parsa `atoms3r_wagmi_salotto → wagmi`. Per i Voice PE il
device_id sara il MAC address (come nel DB), non una stringa parsabile.
La funzione resta come fallback per device legacy, ma il path principale
diventa il lookup nel database voice_devices.

```python
def extract_location_from_device(device_id: str) -> Optional[str]:
    """
    Estrae location da device_id.
    1. Prima cerca nel DB voice_devices (path principale)
    2. Fallback: parsing legacy atoms3r_wagmi_salotto -> wagmi
    """
    if not device_id:
        return None

    # Path principale: DB lookup
    device_config = get_device_speaker_config(device_id.upper().strip())
    if device_config and device_config.get("location_id"):
        return device_config["location_id"]

    # Fallback legacy: atoms3r_location_room format
    parts = device_id.lower().split('_')
    if len(parts) >= 2:
        loc = get_location(parts[1])
        if loc:
            return parts[1]
    return None
```

**Source nel WebSocket handler:**

Attualmente il ws_audio_handler hardcoda `"source": "AtomS3R"` nel context.
Deve invece:
1. Estrarre `device_type` dal messaggio `hello` (campo `fw` gia contiene
   info, oppure aggiungere campo `device_type`)
2. Usare `device_type` come source nel context

```python
# Nel hello handler (ws_audio_handler.py):
if msg_type == "hello":
    fw = ctrl.get("fw", "unknown")
    # Determina device_type dal firmware version
    if "voicepe" in fw.lower():
        device_type = "VoicePE"
    else:
        device_type = "AtomS3R"
    conn.device_type = device_type
```

### 2.2 ws_audio_handler.py (~5 punti)

| Riga | Modifica |
|------|----------|
| ~2 | Docstring: "AtomS3R devices" → "voice devices" |
| ~198 | Commento: "AtomS3R" → "voice device" |
| ~342 | Commento: "AtomS3R" → "voice device" |
| ~hello handler | Estrarre device_type, salvare su conn |
| ~context build | Usare `conn.device_type` come source |

**Aggiungere handler per `volume_change`:**

```python
elif msg_type == "volume_change":
    direction = ctrl.get("direction", "up")
    logger.info(f"Device {device_id}: volume_change {direction}")
    await handle_volume_change(device_id, direction)
```

**Nuova funzione `handle_volume_change()`:**

```python
async def handle_volume_change(device_id: str, direction: str):
    """Gestisce volume_change dal Voice PE → regola Echo speaker."""
    device_config = get_device_speaker_config(device_id)
    if not device_config:
        logger.warning(f"volume_change: device {device_id} non configurato")
        return

    location_id = device_config.get("location_id")
    output_speaker = device_config.get("output_speaker")
    if not output_speaker or not location_id:
        logger.warning(f"volume_change: no speaker configured for {device_id}")
        return

    # Fetch volume corrente
    states = await multi_ha.get_states_bulk(location_id, [output_speaker])
    speaker_state = states.get(output_speaker, {})
    current_vol = speaker_state.get("attributes", {}).get("volume_level", 0.3)

    # Step
    step = 0.05
    new_vol = current_vol + step if direction == "up" else current_vol - step
    new_vol = max(0.0, min(1.0, new_vol))

    await multi_ha.call_service(
        location_id, "media_player", "volume_set",
        {"entity_id": output_speaker, "volume_level": new_vol}
    )
    logger.info(f"volume_change: {output_speaker} {current_vol:.2f} → {new_vol:.2f}")
```

### 2.3 security.py (~1 punto)

```python
# Riga ~178: source == "AtomS3R" → source in VOICE_SOURCES
if source in VOICE_SOURCES:
    return False  # Voice channel is trusted (speaker ID verified)
```

### 2.4 speaker_suppress.py (~solo commenti)

Il codice e gia generico (lavora con device_id dal DB). Solo i commenti
menzionano "AtomS3R" — aggiornarli a "voice device".

```python
# Riga 3: "stanza dell'AtomS3R" → "stanza del voice device"
# Riga 74: "device_id dell'AtomS3R" → "device_id del voice device"
```

### 2.5 device_api.py (~solo commenti)

```python
# Riga 3: "Endpoints per AtomS3R" → "Endpoints per voice devices"
# Riga 43: "ENDPOINTS PER ATOMS3R" → "ENDPOINTS PER VOICE DEVICES"
# Riga 50: "dall'AtomS3R" → "dal voice device"
```

L'endpoint `/api/device/config` e `/api/device/heartbeat` funzionano gia
con qualsiasi device_id. Nessuna modifica logica necessaria.

### 2.6 database.py (~1 modifica + commenti)

**Aggiungere colonna `device_type` alla tabella `voice_devices`:**

```sql
-- Aggiungere alla CREATE TABLE:
device_type TEXT DEFAULT 'AtomS3R'
```

**Migrazione per DB esistenti:**

```python
try:
    c.execute("ALTER TABLE voice_devices ADD COLUMN device_type TEXT DEFAULT 'AtomS3R'")
except sqlite3.OperationalError:
    pass  # Colonna gia esistente
```

**Aggiornare funzioni:**
- `upsert_voice_device()` → accettare `device_type` param
- `get_device_speaker_config()` → includere `device_type` nel risultato
- Auto-discover nel `get_device_config` endpoint → determinare type dal firmware

### 2.7 voice_recognition.py, user_api.py, config.py (~solo commenti)

Solo commenti/docstring menzionano "AtomS3R". Nessuna modifica logica.

---

## 3. Nuovo Endpoint: volume_change Handler

Il volume_change arriva via WS (non via REST), quindi va gestito in
`ws_audio_handler.py` come descritto sopra. Non serve un endpoint REST
separato.

Flusso: `Voice PE → WS → wakeword server → WS relay → orchestrator
ws_audio_handler → multi_ha.call_service(media_player.volume_set)`

---

## 4. Riassunto Effort

| File | Modifiche logica | Modifiche commenti | Effort |
|------|-----------------|-------------------|--------|
| main.py | ~15 search-replace | ~5 commenti | Basso |
| ws_audio_handler.py | device_type + volume_change | ~3 commenti | Medio |
| security.py | 1 riga | 0 | Minimo |
| database.py | 1 colonna + 2 funzioni | ~2 commenti | Basso |
| device_api.py | 0 | ~3 commenti | Minimo |
| speaker_suppress.py | 0 | ~3 commenti | Minimo |
| config.py | 1 costante VOICE_SOURCES | ~1 commento | Minimo |
| voice_recognition.py | 0 | ~2 commenti | Minimo |
| user_api.py | 0 | ~2 commenti | Minimo |

**Totale: ~2 ore di lavoro.** La maggior parte e search-replace meccanico.
L'unico pezzo nuovo e la funzione `handle_volume_change()`.

---

## 5. Checklist

- [ ] Aggiungere `VOICE_SOURCES` in config.py
- [ ] Search-replace `source == "AtomS3R"` in main.py (~15 punti)
- [ ] Aggiungere device_type extraction nel hello handler
- [ ] Aggiungere volume_change handler in ws_audio_handler.py
- [ ] Migrare DB: aggiungere colonna device_type
- [ ] Aggiornare extract_location_from_device con DB lookup
- [ ] Aggiornare commenti/docstring in tutti i file
- [ ] Testare con AtomS3R esistenti (regressione zero)
- [ ] Testare con Voice PE
