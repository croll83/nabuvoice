"""
JARVIS WebSocket Audio — ESPHome external component.

Persistent WebSocket connection to JARVIS wakeword server / orchestrator.
Port of the AtomS3R jarvis_ws_audio module to ESPHome framework.

Supports two wake word modes:
  - "local":  micro_wake_word detects on-device, then stream audio
  - "server": continuous Opus streaming, server-side openWakeWord detection

YAML configuration:
  jarvis_ws_audio:
    server_url: "ws://wakeword-server:8200/ws/audio"
    device_token: "xxx"
    microphone: i2s_mics
    wakeword_mode: local  # or "server"
    firmware_version: "1.0.0-voicepe"
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_MICROPHONE
from esphome.components import microphone
from esphome.components.esp32 import add_idf_component

DEPENDENCIES = ["network", "microphone"]
AUTO_LOAD = ["microphone"]
CODEOWNERS = ["@jarvis"]

CONF_SERVER_URL = "server_url"
CONF_DEVICE_TOKEN = "device_token"
CONF_WAKEWORD_MODE = "wakeword_mode"
CONF_FIRMWARE_VERSION = "firmware_version"

jarvis_ws_audio_ns = cg.esphome_ns.namespace("jarvis_ws_audio")
JarvisWsAudio = jarvis_ws_audio_ns.class_("JarvisWsAudio", cg.Component)

# Actions exposed to YAML automations
StartSessionAction = jarvis_ws_audio_ns.class_("StartSessionAction", automation.Action)
StopSessionAction = jarvis_ws_audio_ns.class_("StopSessionAction", automation.Action)
SendSpeakerStopAction = jarvis_ws_audio_ns.class_("SendSpeakerStopAction", automation.Action)
SendDndAction = jarvis_ws_audio_ns.class_("SendDndAction", automation.Action)
SendVolumeChangeAction = jarvis_ws_audio_ns.class_("SendVolumeChangeAction", automation.Action)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(JarvisWsAudio),
    cv.Required(CONF_SERVER_URL): cv.string,
    cv.Optional(CONF_DEVICE_TOKEN, default=""): cv.string,
    cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
    cv.Optional(CONF_WAKEWORD_MODE, default="local"): cv.one_of("local", "server", lower=True),
    cv.Optional(CONF_FIRMWARE_VERSION, default="1.0.0-voicepe"): cv.string,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_server_url(config[CONF_SERVER_URL]))
    cg.add(var.set_device_token(config[CONF_DEVICE_TOKEN]))
    cg.add(var.set_firmware_version(config[CONF_FIRMWARE_VERSION]))

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))

    if config[CONF_WAKEWORD_MODE] == "server":
        cg.add(var.set_server_wakeword_mode(True))
    else:
        cg.add(var.set_server_wakeword_mode(False))

    # Add required ESP-IDF components (moved to esp-protocols repo in IDF 5.x)
    add_idf_component(
        name="esp_websocket_client",
        repo="https://github.com/espressif/esp-protocols.git",
        path="components/esp_websocket_client",
        ref="websocket-v1.6.1",
    )
    # Opus codec library (78/esp-opus — ESP Component Registry v1.0.5)
    add_idf_component(
        name="esp-opus",
        repo="https://github.com/78/esp-opus.git",
        ref="5854a9f7de06ab3505b8fe6e8943db581c2cbe70",
    )
    cg.add_build_flag("-DUSE_JARVIS_WS_AUDIO")


# --- Automation Actions ---

JARVIS_WS_ACTION_SCHEMA = automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(JarvisWsAudio),
})

@automation.register_action("jarvis_ws_audio.start_session", StartSessionAction, JARVIS_WS_ACTION_SCHEMA)
async def start_session_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)

@automation.register_action("jarvis_ws_audio.stop_session", StopSessionAction, JARVIS_WS_ACTION_SCHEMA)
async def stop_session_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)

@automation.register_action("jarvis_ws_audio.speaker_stop", SendSpeakerStopAction, JARVIS_WS_ACTION_SCHEMA)
async def speaker_stop_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)

SEND_DND_SCHEMA = automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(JarvisWsAudio),
    cv.Required("enabled"): cv.templatable(cv.boolean),
})

@automation.register_action("jarvis_ws_audio.send_dnd", SendDndAction, SEND_DND_SCHEMA)
async def send_dnd_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    tmpl = await cg.templatable(config["enabled"], args, bool)
    cg.add(var.set_enabled(tmpl))
    return var

VOLUME_CHANGE_SCHEMA = automation.maybe_simple_id({
    cv.GenerateID(): cv.use_id(JarvisWsAudio),
    cv.Required("direction"): cv.templatable(cv.string),
})

@automation.register_action("jarvis_ws_audio.volume_change", SendVolumeChangeAction, VOLUME_CHANGE_SCHEMA)
async def volume_change_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    tmpl = await cg.templatable(config["direction"], args, cg.std_string)
    cg.add(var.set_direction(tmpl))
    return var
