/**
 * =============================================================================
 * JARVIS Voice PE - WebSocket Audio Component for ESPHome
 * =============================================================================
 *
 * Port of AtomS3R jarvis_ws_audio to ESPHome component framework.
 *
 * Persistent WebSocket connection to JARVIS wakeword server / orchestrator.
 * Handles audio streaming (Opus) and control channel (JSON) on a single WS.
 *
 * Protocol: identical to AtomS3R (see jarvis_ws_audio.c), plus:
 *   - {"type": "volume_change", "direction": "up|down"} (new, for rotary encoder)
 *
 * Supports two wake word modes:
 *   - Local:  micro_wake_word on-device, stream only after detection
 *   - Server: continuous Opus streaming, server-side openWakeWord
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/microphone/microphone.h"
#include <string>

// ESP-IDF WebSocket client (same library as AtomS3R)
#include "esp_websocket_client.h"

// Opus encoder (same as AtomS3R)
#include <opus.h>

namespace esphome {
namespace jarvis_ws_audio {

// Connection state machine (mirrors AtomS3R)
enum class ConnState : uint8_t {
  DISCONNECTED = 0,
  CONNECTING,
  CONNECTED,         // Idle, no audio session
  AUDIO_STARTING,    // Sent audio_start, waiting for ready
  STREAMING,         // Audio session active
};

class JarvisWsAudio : public Component {
 public:
  // --- ESPHome Component interface ---
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // --- Configuration setters (called by codegen) ---
  void set_server_url(const std::string &url) { this->server_url_ = url; }
  void set_device_token(const std::string &token) { this->device_token_ = token; }
  void set_firmware_version(const std::string &version) { this->firmware_version_ = version; }
  void set_microphone(microphone::Microphone *mic) { this->microphone_ = mic; }
  void set_server_wakeword_mode(bool enabled) { this->server_wakeword_mode_ = enabled; }

  // --- Public actions (called from YAML automations) ---

  /** Start an audio session (called on wake word detection or button press) */
  void start_session();

  /** Stop the current audio session (called on button abort) */
  void stop_session();

  /** Send speaker_stop to server (double-press → stop Echo) */
  void send_speaker_stop();

  /** Send DND state (mute switch toggle) */
  void send_dnd(bool enabled);

  /** Send volume change (rotary encoder) */
  void send_volume_change(const std::string &direction);

  /** Send generic state update */
  void send_state(const std::string &state);

  // --- State queries ---
  bool is_connected() const { return this->conn_state_ >= ConnState::CONNECTED; }
  bool is_streaming() const { return this->audio_session_active_; }
  ConnState get_conn_state() const { return this->conn_state_; }

 protected:
  // --- Configuration ---
  std::string server_url_;
  std::string device_token_;
  std::string firmware_version_;
  microphone::Microphone *microphone_{nullptr};
  bool server_wakeword_mode_{false};

  // --- Connection state ---
  volatile ConnState conn_state_{ConnState::DISCONNECTED};
  esp_websocket_client_handle_t ws_client_{nullptr};
  std::string device_id_;   // MAC address, populated in setup()

  // --- Audio session state ---
  volatile bool audio_session_active_{false};
  volatile bool audio_session_requested_{false};
  uint32_t audio_session_start_ms_{0};

  // --- Deferred flags (set by WS callbacks, processed in loop()) ---
  volatile bool trigger_listen_pending_{false};
  volatile bool trigger_listen_silent_{true};
  volatile bool tts_done_pending_{false};
  volatile bool wake_detected_pending_{false};
  volatile bool config_update_pending_{false};
  volatile float config_new_sensitivity_{0.82f};
  volatile bool session_done_pending_{false};
  volatile bool session_done_success_{false};
  volatile bool mic_stop_pending_{false};  // deferred mic stop from WS task context

  // --- Opus encoder ---
  OpusEncoder *opus_encoder_{nullptr};
  int16_t *enc_input_buffer_{nullptr};
  uint8_t *enc_output_buffer_{nullptr};

  // --- Reconnection ---
  uint32_t reconnect_delay_ms_{1000};
  uint32_t last_reconnect_attempt_ms_{0};
  uint32_t last_ping_ms_{0};

  // --- Audio buffer for microphone data ---
  // ESPHome microphone provides data via callback (uint8_t); we accumulate here
  std::vector<uint8_t> mic_buffer_;
  volatile size_t mic_buffer_write_pos_{0};
  size_t mic_buffer_read_pos_{0};

  // --- Internal methods ---
  bool init_opus_encoder_();
  void build_ws_url_(char *buf, size_t buf_size);
  bool connect_ws_();
  void disconnect_ws_();
  bool send_json_(const char *type, const char *extra_key = nullptr,
                  const char *extra_val = nullptr);
  bool send_hello_();
  void process_audio_session_();
  void handle_deferred_flags_();

  // --- WebSocket event handler (static, ESP-IDF callback) ---
  static void ws_event_handler_(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data);
  void on_ws_event_(int32_t event_id, esp_websocket_event_data_t *data);
  void on_ws_text_message_(const char *data, int len);
};

// =============================================================================
// Automation Actions (for YAML integration)
// =============================================================================

template<typename... Ts>
class StartSessionAction : public Action<Ts...> {
 public:
  explicit StartSessionAction(JarvisWsAudio *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_session(); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class StopSessionAction : public Action<Ts...> {
 public:
  explicit StopSessionAction(JarvisWsAudio *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->stop_session(); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class SendSpeakerStopAction : public Action<Ts...> {
 public:
  explicit SendSpeakerStopAction(JarvisWsAudio *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->send_speaker_stop(); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class SendDndAction : public Action<Ts...> {
 public:
  explicit SendDndAction(JarvisWsAudio *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, enabled)
  void play(Ts... x) override { this->parent_->send_dnd(this->enabled_.value(x...)); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class SendVolumeChangeAction : public Action<Ts...> {
 public:
  explicit SendVolumeChangeAction(JarvisWsAudio *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, direction)
  void play(Ts... x) override { this->parent_->send_volume_change(this->direction_.value(x...)); }
 protected:
  JarvisWsAudio *parent_;
};

}  // namespace jarvis_ws_audio
}  // namespace esphome
