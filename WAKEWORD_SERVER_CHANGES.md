# Modifiche Wakeword Server per Supporto Voice PE

Il wakeword server e gia quasi completamente device-agnostic. Le modifiche
sono minime.

---

## 1. Cosa Cambia

### 1.1 server.py: Aggiungere `volume_change` handler

Unica modifica logica necessaria. Il message type `volume_change` dal
Voice PE va relayato all'orchestrator, identico a come gia funziona per
`state` e `speaker_stop`.

```python
# In _handle_text(), aggiungere dopo il blocco "speaker_stop":

elif msg_type == "volume_change":
    # Relay volume change to orchestrator
    if conn.relay and conn.relay.is_connected:
        await conn.relay.send_text(raw)
    logger.debug(f"[{conn.device_id}] volume_change relayed")
```

Questo e identico al pattern gia usato per `state` e `speaker_stop`:
ricevi dal device, relay al VPS. Nessuna logica locale.

### 1.2 server.py: Tracciare device_type (opzionale)

Per logging e diagnostica, estrarre il tipo di device dal messaggio hello:

```python
# In DeviceConnection.__init__:
self.device_type: str = "unknown"

# In _handle_text(), nel blocco "hello":
if msg_type == "hello":
    conn.firmware_version = msg.get("fw", "unknown")
    # Determina tipo device dal firmware version
    fw = conn.firmware_version.lower()
    if "voicepe" in fw:
        conn.device_type = "VoicePE"
    else:
        conn.device_type = "AtomS3R"
    logger.info(f"[{conn.device_id}] Hello (fw={conn.firmware_version}, "
                f"type={conn.device_type})")
```

### 1.3 server.py: Aggiornare /api/devices per device_type

```python
# In list_devices():
result.append({
    "device_id": did,
    "state": conn.state.value,
    "firmware": conn.firmware_version,
    "device_type": conn.device_type,      # AGGIUNTO
    "threshold": conn.wakeword.threshold,
    "relay_connected": conn.relay.is_connected if conn.relay else False,
})
```

---

## 2. Cosa NON Cambia

| Modulo | Stato |
|--------|-------|
| `wakeword.py` | Invariato — openWakeWord e gia device-agnostic |
| `multiroom.py` | Invariato — cooldown per device_id, non per tipo |
| `relay.py` | Invariato — puro relay bidirezionale |
| `config.py` | Invariato — nessuna config device-specific |
| `Dockerfile` | Invariato — stesse dipendenze |

Il relay gia forwarda qualsiasi messaggio JSON testuale non riconosciuto
(blocco `else` in `_handle_text`). Quindi anche senza aggiungere
esplicitamente il handler `volume_change`, il messaggio verrebbe comunque
relayato. L'handler esplicito serve solo per logging pulito e per evitare
che cada nel catch-all.

---

## 3. Considerazione Wake Word Mode

Quando il Voice PE usa `wakeword_mode: local` (micro_wake_word on-device):

1. Il device NON fa streaming continuo in stato IDLE
2. Il device invia direttamente `audio_start` dopo la detection locale
3. Il wakeword server non esegue openWakeWord per quel device
4. Il server apre il relay e passa in modalita STREAMING

Questo funziona GIA con il codice attuale del wakeword server, perche:
- Il device invia `audio_start` come messaggio JSON
- `_handle_text` lo gestisce (riga 184-191): cambia stato a STREAMING,
  relay a VPS
- Il wakeword.process_audio() viene chiamato solo in stato IDLE, e il
  device non manda audio binario in IDLE (perche la detection e locale)

**Unica nota:** In modalita locale, il device potrebbe inviare
`audio_start` senza che il server abbia mai ricevuto un wake. Va verificato
che la transizione IDLE → STREAMING (saltando WAKING) funzioni. Potrebbe
servire un piccolo fix:

```python
# In _handle_text, blocco "audio_start":
elif msg_type == "audio_start":
    # Supporta sia wake server-side (WAKING → STREAMING)
    # che wake on-device (IDLE → STREAMING)
    if conn.state in (DeviceState.WAKING, DeviceState.IDLE):
        conn.state = DeviceState.STREAMING
        logger.info(f"[{conn.device_id}] State → STREAMING")
        # Assicura relay aperto
        if not conn.relay or not conn.relay.is_connected:
            await _open_relay(conn)
        if conn.relay and conn.relay.is_connected:
            await conn.relay.send_text(raw)
```

Attualmente accetta solo `WAKING` — aggiungere `IDLE` per supportare la
wake word on-device.

---

## 4. Riassunto

| Modifica | File | Effort |
|----------|------|--------|
| Handler volume_change | server.py | 5 righe |
| Tracking device_type | server.py | 8 righe |
| API devices response | server.py | 1 riga |
| Fix IDLE → STREAMING | server.py | 5 righe |
| Commenti/docstring | server.py | 3 righe |

**Totale: ~20 righe di codice.** Il wakeword server e gia pronto.

---

## 5. Checklist

- [ ] Aggiungere handler `volume_change` in `_handle_text`
- [ ] Aggiungere `device_type` a DeviceConnection
- [ ] Estrarre device_type dal hello message
- [ ] Aggiornare `/api/devices` response
- [ ] Fix transizione IDLE → STREAMING per wake word on-device
- [ ] Aggiornare docstring modulo (riga 3)
- [ ] Testare con AtomS3R (regressione zero)
- [ ] Testare con Voice PE
