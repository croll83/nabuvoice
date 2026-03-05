/**
 * =============================================================================
 * JARVIS Voice PE - WebSocket Audio Component for ESPHome
 * =============================================================================
 *
 * Port of AtomS3R jarvis_ws_audio to ESPHome component framework.
 *
 * Persistent WebSocket connection to JARVIS wakeword server / orchestrator.
 * Handles audio streaming (raw PCM) and control channel (JSON) on a single WS.
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
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/audio/audio.h"
#include <string>

// ESP-IDF WebSocket client (same library as AtomS3R)
#include "esp_websocket_client.h"

// ESP-IDF HTTP client (for fire-and-forget POST to orchestrator)
#include "esp_http_client.h"

// Opus encoder (same as AtomS3R)
#include <opus.h>

// FreeRTOS (for dedicated audio encoding task)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
  void set_orchestrator_url(const std::string &url) { this->orchestrator_url_ = url; }
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

  /** Send volume change via HTTP POST to orchestrator (rotary encoder) */
  void send_volume_change(const std::string &direction);

  /** Send speaker suppress via HTTP POST to orchestrator (wake word / button) */
  void send_speaker_suppress();

  /** Send generic state update */
  void send_state(const std::string &state);

  // --- State queries ---
  bool is_connected() const { return this->conn_state_ >= ConnState::CONNECTED; }
  bool is_streaming() const { return this->audio_session_active_; }
  ConnState get_conn_state() const { return this->conn_state_; }

  /** Voice phase for LED control (matches YAML substitution IDs) */
  int get_voice_phase() const { return this->voice_phase_; }

  /** Speaker type from orchestrator config: "alexa" or "internal" */
  const std::string &get_speaker_type() const { return this->speaker_type_; }

 protected:
  // --- Configuration ---
  std::string server_url_;
  std::string device_token_;
  std::string orchestrator_url_;
  std::string firmware_version_;
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  bool server_wakeword_mode_{false};

  // --- Connection state ---
  volatile ConnState conn_state_{ConnState::DISCONNECTED};
  esp_websocket_client_handle_t ws_client_{nullptr};
  std::string device_id_;   // MAC address, populated in setup()

  // --- Audio session state ---
  volatile bool audio_session_active_{false};
  volatile bool audio_session_requested_{false};
  uint32_t audio_session_start_ms_{0};

  // --- Voice phase (for LED control, matches YAML substitution IDs) ---
  static constexpr int PHASE_IDLE = 1;
  static constexpr int PHASE_LISTENING = 3;
  static constexpr int PHASE_THINKING = 4;
  static constexpr int PHASE_REPLYING = 5;
  static constexpr int PHASE_NOT_READY = 10;
  static constexpr int PHASE_ERROR = 11;
  volatile int voice_phase_{PHASE_NOT_READY};

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

  // --- PCM send buffer (raw 16-bit mono frames) ---
  uint8_t *pcm_send_buffer_{nullptr};   // 640 bytes = 320 samples × 2 bytes (one 20ms frame)

  // --- Opus decoder (TTS playback) ---
  OpusDecoder *opus_decoder_{nullptr};
  int16_t *dec_output_buffer_{nullptr};  // decoded PCM (320 samples)

  // --- TTS frame queue (incoming Opus frames from server) ---
  static constexpr int TTS_QUEUE_SLOTS = 100;        // ~2 seconds of audio
  static constexpr int TTS_MAX_FRAME_SIZE = 256;      // Max Opus frame bytes
  struct TtsFrame {
    uint16_t length;
    uint8_t data[TTS_MAX_FRAME_SIZE];
  };
  TtsFrame *tts_queue_{nullptr};
  volatile int tts_queue_write_{0};
  int tts_queue_read_{0};
  volatile bool tts_playing_{false};
  volatile bool tts_done_received_{false};  // server sent tts_done, drain queue then stop
  bool tts_speaker_started_{false};          // speaker_->start() called for current TTS session
  bool tts_pcm_pending_{false};              // decoded PCM waiting to be written to speaker
  size_t tts_pcm_pending_size_{0};           // total bytes of pending decoded PCM
  size_t tts_pcm_written_{0};               // bytes already written from pending PCM

  // --- Audio encoding task (runs opus_encode on separate stack) ---
  TaskHandle_t audio_task_handle_{nullptr};
  StackType_t *audio_task_stack_{nullptr};
  StaticTask_t audio_task_tcb_;
  static void audio_encode_task_(void *param);

  // --- Reconnection & watchdog ---
  uint32_t reconnect_delay_ms_{1000};
  uint32_t last_reconnect_attempt_ms_{0};
  uint32_t last_ping_ms_{0};
  uint32_t last_data_ms_{0};  // last time we received any WS data (for watchdog)
  uint32_t last_volume_change_ms_{0};  // throttle volume_change messages

  // --- Audio buffer for microphone data ---
  // Mic callback extracts left channel (16-bit mono) from 32-bit stereo and stores here.
  // Ring buffer holds clean 16-bit mono samples ready for PCM send.
  // Allocated from PSRAM to avoid exhausting internal SRAM.
  uint8_t *mic_buffer_{nullptr};
  size_t mic_buffer_size_{0};          // 32000 bytes = 1 second of 16-bit mono @ 16kHz
  volatile size_t mic_buffer_write_pos_{0};
  size_t mic_buffer_read_pos_{0};

  // --- Speaker type (set by orchestrator via config_update) ---
  std::string speaker_type_{"alexa"};  // "alexa" or "internal"

  // --- Debug ---
  volatile bool debug_first_frame_{true};  // Log first audio frame of each session

  // --- Internal methods ---
  bool init_pcm_send_buffer_();
  bool init_opus_decoder_();
  void build_ws_url_(char *buf, size_t buf_size);
  bool connect_ws_();
  void disconnect_ws_();
  bool send_json_(const char *type, const char *extra_key = nullptr,
                  const char *extra_val = nullptr);
  bool send_hello_();
  void process_audio_session_();
  void process_tts_playback_();
  void handle_deferred_flags_();

  // --- HTTP fire-and-forget helper (static, spawns FreeRTOS task) ---
  static void http_post_fire_and_forget_(const char *url, const char *json_body,
                                          const char *auth_token);

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
  void play(const Ts &...x) override { this->parent_->start_session(); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class StopSessionAction : public Action<Ts...> {
 public:
  explicit StopSessionAction(JarvisWsAudio *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->stop_session(); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class SendSpeakerStopAction : public Action<Ts...> {
 public:
  explicit SendSpeakerStopAction(JarvisWsAudio *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->send_speaker_stop(); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class SendDndAction : public Action<Ts...> {
 public:
  explicit SendDndAction(JarvisWsAudio *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(bool, enabled)
  void play(const Ts &...x) override { this->parent_->send_dnd(this->enabled_.value(x...)); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class SendVolumeChangeAction : public Action<Ts...> {
 public:
  explicit SendVolumeChangeAction(JarvisWsAudio *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, direction)
  void play(const Ts &...x) override { this->parent_->send_volume_change(this->direction_.value(x...)); }
 protected:
  JarvisWsAudio *parent_;
};

template<typename... Ts>
class SpeakerSuppressAction : public Action<Ts...> {
 public:
  explicit SpeakerSuppressAction(JarvisWsAudio *parent) : parent_(parent) {}
  void play(const Ts &...x) override { this->parent_->send_speaker_suppress(); }
 protected:
  JarvisWsAudio *parent_;
};

}  // namespace jarvis_ws_audio
}  // namespace esphome
