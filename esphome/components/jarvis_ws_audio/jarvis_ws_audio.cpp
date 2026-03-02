/**
 * =============================================================================
 * JARVIS Voice PE - WebSocket Audio Component Implementation
 * =============================================================================
 *
 * Port of AtomS3R jarvis_ws_audio.c to ESPHome component framework.
 * See jarvis_ws_audio.h for documentation.
 *
 * Key differences from AtomS3R:
 *   - Runs in ESPHome's main loop instead of a dedicated FreeRTOS task
 *   - Audio comes from ESPHome microphone component instead of jarvis_audio ring buffer
 *   - LED state managed by parent YAML, not by this component
 *   - New message: volume_change for rotary encoder
 *
 * Identical to AtomS3R:
 *   - Opus encoding (same codec, same params)
 *   - esp_websocket_client (same ESP-IDF library)
 *   - WebSocket protocol (all message types)
 *   - Connection state machine
 *   - Reconnect with exponential backoff
 */

#include "jarvis_ws_audio.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#include <cstring>
#include <cJSON.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_mac.h"

namespace esphome {
namespace jarvis_ws_audio {

static const char *const TAG = "jarvis_ws_audio";

// --- Audio constants (identical to AtomS3R) ---
static constexpr int SAMPLE_RATE = 16000;
static constexpr int OPUS_FRAME_SAMPLES = 320;       // 20ms @ 16kHz
static constexpr int OPUS_MAX_PACKET_SIZE = 1276;
static constexpr int OPUS_BITRATE_VAL = 30000;
static constexpr int OPUS_COMPLEXITY_VAL = 0;

// --- Timing constants ---
static constexpr uint32_t SESSION_TIMEOUT_MS = 30000;  // 30s max audio session
static constexpr uint32_t RECONNECT_MIN_MS = 1000;
static constexpr uint32_t RECONNECT_MAX_MS = 30000;
static constexpr uint32_t PING_INTERVAL_MS = 30000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;

// =============================================================================
// SETUP
// =============================================================================

void JarvisWsAudio::setup() {
  ESP_LOGI(TAG, "Setting up JARVIS WebSocket Audio...");

  // Get device MAC address (same format as AtomS3R: AABBCCDDEEFF)
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_str[13];
  snprintf(mac_str, sizeof(mac_str), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  this->device_id_ = std::string(mac_str);
  ESP_LOGI(TAG, "Device ID (MAC): %s", this->device_id_.c_str());

  // Initialize Opus encoder
  if (!this->init_opus_encoder_()) {
    ESP_LOGE(TAG, "Opus encoder init failed");
    this->mark_failed();
    return;
  }

  // Allocate mic buffer (enough for continuous streaming)
  // Ring buffer: 2 seconds @ 16kHz = 32000 samples
  this->mic_buffer_.resize(32000, 0);
  this->mic_buffer_write_pos_ = 0;

  // Register microphone data callback
  // TODO: verify ESPHome microphone API for data callback registration
  // The microphone component may need to be started explicitly
  // this->microphone_->add_data_callback([this](const std::vector<int16_t> &data) {
  //   // Copy audio data to our ring buffer
  //   for (auto sample : data) {
  //     this->mic_buffer_[this->mic_buffer_write_pos_ % this->mic_buffer_.size()] = sample;
  //     this->mic_buffer_write_pos_++;
  //   }
  // });

  ESP_LOGI(TAG, "JARVIS WS Audio initialized (mode=%s)",
           this->server_wakeword_mode_ ? "server" : "local");
}

// =============================================================================
// MAIN LOOP
// =============================================================================

void JarvisWsAudio::loop() {
  uint32_t now = millis();

  // --- Connection management ---
  if (this->conn_state_ == ConnState::DISCONNECTED) {
    // Reconnect with exponential backoff
    if (now - this->last_reconnect_attempt_ms_ >= this->reconnect_delay_ms_) {
      this->last_reconnect_attempt_ms_ = now;
      if (this->connect_ws_()) {
        this->reconnect_delay_ms_ = RECONNECT_MIN_MS;
      } else {
        this->reconnect_delay_ms_ = std::min(this->reconnect_delay_ms_ * 2, RECONNECT_MAX_MS);
        ESP_LOGW(TAG, "Reconnect failed, next attempt in %ums", this->reconnect_delay_ms_);
      }
    }
    return;
  }

  // --- Process deferred flags from WS callbacks ---
  this->handle_deferred_flags_();

  // --- Audio session management ---
  if (this->audio_session_requested_ && this->conn_state_ == ConnState::CONNECTED) {
    this->audio_session_requested_ = false;
    // Send audio_start
    this->conn_state_ = ConnState::AUDIO_STARTING;
    this->audio_session_active_ = true;
    this->audio_session_start_ms_ = now;
    this->send_json_("audio_start");
    ESP_LOGI(TAG, "Audio session requested, sent audio_start");
  }

  // --- Active audio session: read mic, encode Opus, send ---
  if (this->audio_session_active_ && this->conn_state_ == ConnState::STREAMING) {
    this->process_audio_session_();

    // Session timeout
    if (now - this->audio_session_start_ms_ > SESSION_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Audio session timeout");
      this->audio_session_active_ = false;
      this->conn_state_ = ConnState::CONNECTED;
    }
  }

  // --- Server wakeword mode: continuous Opus streaming when idle ---
  if (this->server_wakeword_mode_ &&
      this->conn_state_ == ConnState::CONNECTED &&
      !this->audio_session_active_ &&
      !this->audio_session_requested_) {
    // TODO: read from microphone, encode Opus, send continuously
    // This enables server-side openWakeWord detection
    // Same logic as AtomS3R's #ifndef USE_LOCAL_WAKEWORD block
  }

  // --- Keepalive ping ---
  if (this->conn_state_ >= ConnState::CONNECTED &&
      !this->audio_session_active_ &&
      now - this->last_ping_ms_ > PING_INTERVAL_MS) {
    this->send_json_("pong");
    this->last_ping_ms_ = now;
  }
}

// =============================================================================
// DEFERRED FLAGS (same pattern as AtomS3R main_task)
// =============================================================================

void JarvisWsAudio::handle_deferred_flags_() {
  // trigger_listen from server (multi-turn or enrollment)
  if (this->trigger_listen_pending_) {
    this->trigger_listen_pending_ = false;
    bool silent = this->trigger_listen_silent_;
    ESP_LOGI(TAG, "Processing trigger_listen (silent=%d)", silent);
    // TODO: fire trigger callback / automation
    // If not silent: play wake sound, suppress speakers
    // Then start audio session
    this->start_session();
  }

  // tts_done from server (response delivered, go back to idle)
  if (this->tts_done_pending_) {
    this->tts_done_pending_ = false;
    ESP_LOGI(TAG, "TTS done — returning to idle");
    this->send_state("idle");
    // TODO: fire tts_done automation trigger
  }

  // wake_detected from server (server-side wake word)
  if (this->wake_detected_pending_) {
    this->wake_detected_pending_ = false;
    ESP_LOGI(TAG, "Server wake_detected — starting session");
    // TODO: play wake sound, suppress speakers
    this->start_session();
  }

  // config_update from server
  if (this->config_update_pending_) {
    this->config_update_pending_ = false;
    float sensitivity = this->config_new_sensitivity_;
    ESP_LOGI(TAG, "Config update: sensitivity=%.2f", sensitivity);
    // TODO: update micro_wake_word sensitivity if available
  }

  // Audio session done (set by streaming loop completion)
  if (this->session_done_pending_) {
    this->session_done_pending_ = false;
    bool success = this->session_done_success_;
    ESP_LOGI(TAG, "Audio session done (success=%d)", success);
    if (success) {
      this->send_state("busy");
    } else {
      this->send_state("error");
    }
    // TODO: fire session_done automation trigger
  }
}

// =============================================================================
// AUDIO SESSION PROCESSING
// =============================================================================

void JarvisWsAudio::process_audio_session_() {
  if (!this->opus_encoder_ || !this->enc_input_buffer_ || !this->enc_output_buffer_)
    return;

  // TODO: Read OPUS_FRAME_SAMPLES (320) samples from microphone ring buffer
  // For now this is a placeholder — exact microphone API integration
  // depends on ESPHome microphone component's data delivery mechanism.
  //
  // The AtomS3R reads from a ring buffer filled by a separate I2S task:
  //   size_t samples = jarvis_audio_read_raw(enc_input_buffer, OPUS_FRAME_SAMPLES, 15);
  //
  // In ESPHome, the microphone component delivers data via callback.
  // We need to accumulate samples in mic_buffer_ and read from there.
  //
  // Example:
  //   size_t available = mic_buffer_write_pos_ - mic_buffer_read_pos_;
  //   if (available < OPUS_FRAME_SAMPLES) return;  // Not enough data yet
  //   memcpy(enc_input_buffer_, &mic_buffer_[read_pos % size], OPUS_FRAME_SAMPLES * 2);
  //   mic_buffer_read_pos_ += OPUS_FRAME_SAMPLES;

  // Encode to Opus
  // int encoded_size = opus_encode(opus_encoder_, enc_input_buffer_,
  //                                OPUS_FRAME_SAMPLES, enc_output_buffer_,
  //                                OPUS_MAX_PACKET_SIZE);
  // if (encoded_size > 0) {
  //   esp_websocket_client_send_bin(ws_client_, (const char *)enc_output_buffer_,
  //                                 encoded_size, pdMS_TO_TICKS(1000));
  // }
}

// =============================================================================
// OPUS INITIALIZATION (identical to AtomS3R)
// =============================================================================

bool JarvisWsAudio::init_opus_encoder_() {
  int err;
  this->opus_encoder_ = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK || !this->opus_encoder_) {
    ESP_LOGE(TAG, "Opus encoder create failed: %d", err);
    return false;
  }

  opus_encoder_ctl(this->opus_encoder_, OPUS_SET_BITRATE(OPUS_BITRATE_VAL));
  opus_encoder_ctl(this->opus_encoder_, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY_VAL));
  opus_encoder_ctl(this->opus_encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

  this->enc_input_buffer_ = (int16_t *)heap_caps_malloc(
      OPUS_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  this->enc_output_buffer_ = (uint8_t *)heap_caps_malloc(
      OPUS_MAX_PACKET_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!this->enc_input_buffer_ || !this->enc_output_buffer_) {
    ESP_LOGE(TAG, "Opus buffer alloc failed");
    return false;
  }

  ESP_LOGI(TAG, "Opus encoder: %dHz mono, bitrate=%d, complexity=%d",
           SAMPLE_RATE, OPUS_BITRATE_VAL, OPUS_COMPLEXITY_VAL);
  return true;
}

// =============================================================================
// WEBSOCKET CONNECTION (same as AtomS3R)
// =============================================================================

void JarvisWsAudio::build_ws_url_(char *buf, size_t buf_size) {
  if (this->device_token_.empty()) {
    snprintf(buf, buf_size, "%s?device_id=%s",
             this->server_url_.c_str(), this->device_id_.c_str());
  } else {
    snprintf(buf, buf_size, "%s?device_id=%s&token=%s",
             this->server_url_.c_str(), this->device_id_.c_str(),
             this->device_token_.c_str());
  }
}

bool JarvisWsAudio::connect_ws_() {
  char url[384];
  this->build_ws_url_(url, sizeof(url));
  ESP_LOGI(TAG, "Connecting to %s", url);

  this->conn_state_ = ConnState::CONNECTING;

  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = url;
  ws_cfg.buffer_size = 2048;
  ws_cfg.task_stack = 4096;
  ws_cfg.task_prio = 5;

  this->ws_client_ = esp_websocket_client_init(&ws_cfg);
  if (!this->ws_client_) {
    ESP_LOGE(TAG, "Failed to init WS client");
    this->conn_state_ = ConnState::DISCONNECTED;
    return false;
  }

  esp_websocket_register_events(this->ws_client_, WEBSOCKET_EVENT_ANY,
                                &JarvisWsAudio::ws_event_handler_, this);

  esp_err_t err = esp_websocket_client_start(this->ws_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WS client: %s", esp_err_to_name(err));
    esp_websocket_client_destroy(this->ws_client_);
    this->ws_client_ = nullptr;
    this->conn_state_ = ConnState::DISCONNECTED;
    return false;
  }

  // Wait for connection (event handler sets CONNECTED)
  // Note: in ESPHome we can't block, so we return and check state in loop()
  // The event handler will update conn_state_ asynchronously
  return true;
}

void JarvisWsAudio::disconnect_ws_() {
  this->audio_session_active_ = false;
  if (this->ws_client_) {
    esp_websocket_client_stop(this->ws_client_);
    esp_websocket_client_destroy(this->ws_client_);
    this->ws_client_ = nullptr;
  }
  this->conn_state_ = ConnState::DISCONNECTED;
}

// =============================================================================
// WEBSOCKET EVENT HANDLER (identical logic to AtomS3R)
// =============================================================================

void JarvisWsAudio::ws_event_handler_(void *handler_args, esp_event_base_t base,
                                       int32_t event_id, void *event_data) {
  auto *self = static_cast<JarvisWsAudio *>(handler_args);
  auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
  self->on_ws_event_(event_id, data);
}

void JarvisWsAudio::on_ws_event_(int32_t event_id, esp_websocket_event_data_t *data) {
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WebSocket connected");
      this->conn_state_ = ConnState::CONNECTED;
      this->reconnect_delay_ms_ = RECONNECT_MIN_MS;
      this->last_ping_ms_ = millis();
      // Send hello
      this->send_hello_();
      break;

    case WEBSOCKET_EVENT_DATA:
      if (data->op_code == 0x01 && data->data_ptr && data->data_len > 0) {
        // Text frame: JSON control message
        this->on_ws_text_message_(data->data_ptr, data->data_len);
      }
      // Binary frames (TTS Opus from server) — not used for Voice PE
      // (TTS goes through Alexa, not device speaker)
      // But kept for future use / fallback local TTS
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WebSocket disconnected");
      this->conn_state_ = ConnState::DISCONNECTED;
      this->audio_session_active_ = false;
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WebSocket error");
      this->conn_state_ = ConnState::DISCONNECTED;
      this->audio_session_active_ = false;
      break;

    default:
      break;
  }
}

void JarvisWsAudio::on_ws_text_message_(const char *data, int len) {
  // Parse JSON (identical to AtomS3R ws_event_handler)
  char *json_buf = (char *)malloc(len + 1);
  if (!json_buf) return;
  memcpy(json_buf, data, len);
  json_buf[len] = '\0';

  cJSON *msg = cJSON_Parse(json_buf);
  free(json_buf);
  if (!msg) return;

  const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "type"));
  if (!type) { cJSON_Delete(msg); return; }

  if (strcmp(type, "welcome") == 0) {
    ESP_LOGI(TAG, "Server welcome received");

  } else if (strcmp(type, "ready") == 0) {
    ESP_LOGI(TAG, "Audio session ready");
    if (this->conn_state_ == ConnState::AUDIO_STARTING) {
      this->conn_state_ = ConnState::STREAMING;
    }

  } else if (strcmp(type, "speech_end") == 0) {
    ESP_LOGI(TAG, "Server detected speech end");
    if (this->conn_state_ == ConnState::STREAMING) {
      this->audio_session_active_ = false;
      this->conn_state_ = ConnState::CONNECTED;
      this->session_done_pending_ = true;
      this->session_done_success_ = true;
    }

  } else if (strcmp(type, "trigger_listen") == 0) {
    cJSON *silent_obj = cJSON_GetObjectItem(msg, "silent");
    this->trigger_listen_silent_ = silent_obj ? cJSON_IsTrue(silent_obj) : true;
    this->trigger_listen_pending_ = true;

  } else if (strcmp(type, "tts_done") == 0) {
    this->tts_done_pending_ = true;

  } else if (strcmp(type, "config_update") == 0) {
    cJSON *sens = cJSON_GetObjectItem(msg, "wake_word_sensitivity");
    if (sens && cJSON_IsNumber(sens)) {
      this->config_new_sensitivity_ = (float)sens->valuedouble;
    }
    this->config_update_pending_ = true;

  } else if (strcmp(type, "wake_detected") == 0) {
    this->wake_detected_pending_ = true;

  } else if (strcmp(type, "ping") == 0) {
    this->send_json_("pong");

  } else if (strcmp(type, "error") == 0) {
    ESP_LOGE(TAG, "Server error");
    if (this->audio_session_active_) {
      this->audio_session_active_ = false;
      this->conn_state_ = ConnState::CONNECTED;
      this->session_done_pending_ = true;
      this->session_done_success_ = false;
    }
  }

  cJSON_Delete(msg);
}

// =============================================================================
// JSON SEND HELPERS (identical to AtomS3R)
// =============================================================================

bool JarvisWsAudio::send_json_(const char *type, const char *extra_key,
                                const char *extra_val) {
  if (!this->ws_client_ || this->conn_state_ < ConnState::CONNECTED) return false;

  cJSON *json = cJSON_CreateObject();
  if (!json) return false;

  cJSON_AddStringToObject(json, "type", type);
  if (extra_key && extra_val) {
    cJSON_AddStringToObject(json, extra_key, extra_val);
  }

  char *str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!str) return false;

  int ret = esp_websocket_client_send_text(this->ws_client_, str, strlen(str),
                                            pdMS_TO_TICKS(1000));
  free(str);
  return (ret >= 0);
}

bool JarvisWsAudio::send_hello_() {
  if (!this->ws_client_ || this->conn_state_ < ConnState::CONNECTED) return false;

  cJSON *json = cJSON_CreateObject();
  if (!json) return false;

  cJSON_AddStringToObject(json, "type", "hello");
  cJSON_AddStringToObject(json, "device_id", this->device_id_.c_str());
  cJSON_AddStringToObject(json, "fw", this->firmware_version_.c_str());

  char *str = cJSON_PrintUnformatted(json);
  cJSON_Delete(json);
  if (!str) return false;

  int ret = esp_websocket_client_send_text(this->ws_client_, str, strlen(str),
                                            pdMS_TO_TICKS(1000));
  free(str);
  return (ret >= 0);
}

// =============================================================================
// PUBLIC ACTIONS
// =============================================================================

void JarvisWsAudio::start_session() {
  if (this->conn_state_ != ConnState::CONNECTED) {
    ESP_LOGW(TAG, "Cannot start session: not connected (state=%d)", (int)this->conn_state_);
    return;
  }
  if (this->audio_session_active_) {
    ESP_LOGW(TAG, "Audio session already active");
    return;
  }
  this->audio_session_requested_ = true;
}

void JarvisWsAudio::stop_session() {
  if (this->audio_session_active_) {
    ESP_LOGI(TAG, "Stopping audio session");
    this->send_json_("audio_end");
    this->audio_session_active_ = false;
    this->conn_state_ = ConnState::CONNECTED;
  }
}

void JarvisWsAudio::send_speaker_stop() {
  ESP_LOGI(TAG, "Sending speaker_stop");
  this->send_json_("speaker_stop");
}

void JarvisWsAudio::send_dnd(bool enabled) {
  ESP_LOGI(TAG, "Sending DND state: %s", enabled ? "true" : "false");
  this->send_json_("state", "state", enabled ? "dnd" : "idle");
}

void JarvisWsAudio::send_volume_change(const std::string &direction) {
  ESP_LOGI(TAG, "Sending volume_change: %s", direction.c_str());
  this->send_json_("volume_change", "direction", direction.c_str());
}

void JarvisWsAudio::send_state(const std::string &state) {
  this->send_json_("state", "state", state.c_str());
}

}  // namespace jarvis_ws_audio
}  // namespace esphome
