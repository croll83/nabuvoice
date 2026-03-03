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
#include "esphome/components/network/util.h"

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

// --- Microphone format constants ---
// Voice PE I2S mic: 32-bit stereo (bits_per_sample: 32bit, channel: stereo)
// Each stereo sample pair = 8 bytes: [L0 L1 L2 L3 R0 R1 R2 R3] (little-endian)
static constexpr int MIC_BYTES_PER_STEREO_PAIR = 8;   // 4 bytes/ch × 2 channels
static constexpr int MIC_CHANNEL_OFFSET = 0;           // 0 = left channel (XMOS processed/beamformed audio)
static constexpr int MIC_GAIN_FACTOR = 1;              // No gain — XMOS DSP output is already amplified (gain×4 causes clipping)

// --- Timing constants ---
static constexpr uint32_t SESSION_TIMEOUT_MS = 30000;  // 30s max audio session
static constexpr uint32_t RECONNECT_MIN_MS = 1000;
static constexpr uint32_t RECONNECT_MAX_MS = 30000;
static constexpr uint32_t PING_INTERVAL_MS = 30000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
static constexpr uint32_t WS_WATCHDOG_TIMEOUT_MS = 90000;  // Force reconnect if no data for 90s

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

  // Initialize Opus encoder + decoder
  if (!this->init_opus_encoder_()) {
    ESP_LOGE(TAG, "Opus encoder init failed");
    this->mark_failed();
    return;
  }
  if (this->speaker_ && !this->init_opus_decoder_()) {
    ESP_LOGE(TAG, "Opus decoder init failed");
    this->mark_failed();
    return;
  }

  // Allocate mic buffer from PSRAM (ring buffer for raw bytes from ESPHome microphone)
  // Mic delivers 32-bit stereo @ 16kHz = 128000 bytes/sec; 1 second buffer
  // MUST use PSRAM — internal SRAM is too scarce for 128KB (causes OOM for I2S DMA)
  this->mic_buffer_size_ = 128000;
  this->mic_buffer_ = (uint8_t *)heap_caps_calloc(this->mic_buffer_size_, 1,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!this->mic_buffer_) {
    ESP_LOGE(TAG, "Failed to allocate mic buffer from PSRAM");
    this->mark_failed();
    return;
  }
  this->mic_buffer_write_pos_ = 0;
  this->mic_buffer_read_pos_ = 0;

  // Register microphone data callback
  // ESPHome microphone delivers audio as std::vector<uint8_t> chunks (~16ms each)
  this->microphone_->add_data_callback([this](const std::vector<uint8_t> &data) {
    size_t buf_size = this->mic_buffer_size_;
    for (size_t i = 0; i < data.size(); i++) {
      this->mic_buffer_[this->mic_buffer_write_pos_ % buf_size] = data[i];
      this->mic_buffer_write_pos_++;
    }
  });

  // Create dedicated FreeRTOS task for Opus encoding.
  // opus_encode → silk_Encode uses ~10KB+ stack, too much for ESPHome's loopTask.
  // Task stack is allocated from PSRAM to preserve internal RAM.
  this->audio_task_stack_ = (StackType_t *)heap_caps_malloc(
      32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!this->audio_task_stack_) {
    ESP_LOGE(TAG, "Failed to allocate audio task stack from PSRAM");
    this->mark_failed();
    return;
  }
  this->audio_task_handle_ = xTaskCreateStaticPinnedToCore(
      &JarvisWsAudio::audio_encode_task_,
      "jarvis_audio_enc",
      32768,    // 32KB stack
      this,
      5,        // Priority
      this->audio_task_stack_,
      &this->audio_task_tcb_,
      1         // Core 1
  );
  if (!this->audio_task_handle_) {
    ESP_LOGE(TAG, "Failed to create audio encode task");
    this->mark_failed();
    return;
  }

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
    // Clean up stale client (with auto-reconnect disabled, task has exited)
    if (this->ws_client_) {
      esp_websocket_client_destroy(this->ws_client_);
      this->ws_client_ = nullptr;
    }
    // Don't attempt to connect until WiFi is ready
    if (!network::is_connected()) {
      return;
    }
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
    this->voice_phase_ = PHASE_LISTENING;
    this->send_json_("audio_start");
    ESP_LOGI(TAG, "Audio session requested, sent audio_start");
  }

  // --- Active audio session: signal encoding task ---
  if (this->audio_session_active_ && this->conn_state_ == ConnState::STREAMING) {
    // Signal the dedicated audio task to encode & send (opus runs on its own stack)
    if (this->audio_task_handle_) {
      xTaskNotifyGive(this->audio_task_handle_);
    }

    // Session timeout
    if (now - this->audio_session_start_ms_ > SESSION_TIMEOUT_MS) {
      ESP_LOGW(TAG, "Audio session timeout");
      this->audio_session_active_ = false;
      this->conn_state_ = ConnState::CONNECTED;
      this->voice_phase_ = PHASE_ERROR;
      // Note: do NOT call microphone_->stop() here — MWW manages the mic lifecycle.
      // Stopping it would kill wake word detection.
    }
  }

  // --- TTS playback: signal audio task when frames are queued ---
  if (this->tts_playing_ && this->audio_task_handle_) {
    xTaskNotifyGive(this->audio_task_handle_);
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

  // --- Connection watchdog ---
  // If stuck in CONNECTING for too long, force disconnect and retry
  if (this->conn_state_ == ConnState::CONNECTING &&
      now - this->last_reconnect_attempt_ms_ > CONNECT_TIMEOUT_MS) {
    ESP_LOGW(TAG, "Connection attempt timed out, forcing disconnect");
    this->disconnect_ws_();
    return;
  }

  // If connected but no data received for a long time, force reconnect
  if (this->conn_state_ >= ConnState::CONNECTED &&
      now - this->last_data_ms_ > WS_WATCHDOG_TIMEOUT_MS) {
    ESP_LOGW(TAG, "No data from server for %us, forcing reconnect",
             WS_WATCHDOG_TIMEOUT_MS / 1000);
    this->disconnect_ws_();
    return;
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
  // Deferred mic stop flag (cleared but no longer stops mic — MWW manages lifecycle)
  if (this->mic_stop_pending_) {
    this->mic_stop_pending_ = false;
  }

  // trigger_listen from server (multi-turn or enrollment)
  if (this->trigger_listen_pending_) {
    this->trigger_listen_pending_ = false;
    bool silent = this->trigger_listen_silent_;
    ESP_LOGI(TAG, "Processing trigger_listen (silent=%d)", silent);
    // Multi-turn: server wants us to listen again
    // LED will transition to LISTENING via start_session → voice_phase_
    this->start_session();
  }

  // tts_done from server (response delivered, go back to idle)
  // If TTS is playing on internal speaker, wait for queue to drain first
  if (this->tts_done_pending_ && !this->tts_playing_) {
    this->tts_done_pending_ = false;
    ESP_LOGI(TAG, "TTS done — returning to idle");
    this->voice_phase_ = PHASE_IDLE;
    this->send_state("idle");
  }

  // wake_detected from server (server-side wake word)
  if (this->wake_detected_pending_) {
    this->wake_detected_pending_ = false;
    ESP_LOGI(TAG, "Server wake_detected — starting session");
    // LED will transition to LISTENING via start_session → voice_phase_
    this->start_session();
  }

  // config_update from server (sensitivity, etc.)
  if (this->config_update_pending_) {
    this->config_update_pending_ = false;
    float sensitivity = this->config_new_sensitivity_;
    ESP_LOGI(TAG, "Config update: sensitivity=%.2f (logged only, MWW sensitivity set via YAML)", sensitivity);
  }

  // Audio session done (set by speech_end or error)
  if (this->session_done_pending_) {
    this->session_done_pending_ = false;
    bool success = this->session_done_success_;
    ESP_LOGI(TAG, "Audio session done (success=%d)", success);
    if (success) {
      this->voice_phase_ = PHASE_THINKING;
      this->send_state("busy");
    } else {
      this->voice_phase_ = PHASE_ERROR;
      this->send_state("error");
    }
  }
}

// =============================================================================
// AUDIO SESSION PROCESSING
// =============================================================================

void JarvisWsAudio::process_audio_session_() {
  if (!this->opus_encoder_ || !this->enc_input_buffer_ || !this->enc_output_buffer_)
    return;

  // Mic delivers 32-bit stereo: each Opus frame needs 320 stereo pairs = 2560 bytes
  const size_t frame_bytes = OPUS_FRAME_SAMPLES * MIC_BYTES_PER_STEREO_PAIR;
  const size_t buf_size = this->mic_buffer_size_;

  // Process all available complete frames
  while (true) {
    size_t available = this->mic_buffer_write_pos_ - this->mic_buffer_read_pos_;
    if (available < frame_bytes)
      break;

    // Debug: log first frame's channel data for diagnosis
    if (this->debug_first_frame_) {
      this->debug_first_frame_ = false;
      size_t base = this->mic_buffer_read_pos_ % buf_size;
      // Show first stereo pair raw bytes
      ESP_LOGI(TAG, "First stereo pair [%02X %02X %02X %02X | %02X %02X %02X %02X]",
               this->mic_buffer_[base], this->mic_buffer_[(base+1)%buf_size],
               this->mic_buffer_[(base+2)%buf_size], this->mic_buffer_[(base+3)%buf_size],
               this->mic_buffer_[(base+4)%buf_size], this->mic_buffer_[(base+5)%buf_size],
               this->mic_buffer_[(base+6)%buf_size], this->mic_buffer_[(base+7)%buf_size]);
      // Show upper 16 bits of both channels for comparison
      int16_t left = (int16_t)((this->mic_buffer_[(base+3)%buf_size] << 8) | this->mic_buffer_[(base+2)%buf_size]);
      int16_t right = (int16_t)((this->mic_buffer_[(base+7)%buf_size] << 8) | this->mic_buffer_[(base+6)%buf_size]);
      ESP_LOGI(TAG, "Ch0(left)=%d Ch1(right)=%d (using ch%d, offset=%d)",
               left, right, MIC_CHANNEL_OFFSET / 4, MIC_CHANNEL_OFFSET);
    }

    // Convert 32-bit stereo → 16-bit mono (one channel, upper 16 bits, with gain)
    // Stereo pair layout (little-endian): [L0 L1 L2 L3 R0 R1 R2 R3]
    // Upper 16 bits of 32-bit LE sample = bytes [2] and [3] (relative to channel start)
    for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
      size_t pair_base = (this->mic_buffer_read_pos_ + i * MIC_BYTES_PER_STEREO_PAIR) % buf_size;
      // Select channel and take upper 16 bits of the 32-bit sample
      size_t sample_base = (pair_base + MIC_CHANNEL_OFFSET) % buf_size;
      uint8_t lo = this->mic_buffer_[(sample_base + 2) % buf_size];
      uint8_t hi = this->mic_buffer_[(sample_base + 3) % buf_size];
      int16_t sample = (int16_t)((hi << 8) | lo);

      // Apply gain (same as MWW gain_factor) with saturation
      int32_t amplified = (int32_t)sample * MIC_GAIN_FACTOR;
      if (amplified > 32767) amplified = 32767;
      if (amplified < -32768) amplified = -32768;
      this->enc_input_buffer_[i] = (int16_t)amplified;
    }
    this->mic_buffer_read_pos_ += frame_bytes;

    // Encode to Opus
    int encoded_size = opus_encode(this->opus_encoder_, this->enc_input_buffer_,
                                   OPUS_FRAME_SAMPLES, this->enc_output_buffer_,
                                   OPUS_MAX_PACKET_SIZE);
    if (encoded_size > 0) {
      esp_websocket_client_send_bin(this->ws_client_, (const char *)this->enc_output_buffer_,
                                    encoded_size, pdMS_TO_TICKS(100));
    }
  }
}

// =============================================================================
// AUDIO ENCODE TASK (runs on dedicated FreeRTOS task with 32KB stack)
// =============================================================================

void JarvisWsAudio::audio_encode_task_(void *param) {
  JarvisWsAudio *self = static_cast<JarvisWsAudio *>(param);
  ESP_LOGI(TAG, "Audio encode task started");
  while (true) {
    // Wait for notification from loop(), or poll every 20ms
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    if (self->audio_session_active_ && self->conn_state_ == ConnState::STREAMING) {
      self->process_audio_session_();
    }
    // TTS playback: decode queued Opus frames → speaker
    if (self->tts_playing_) {
      self->process_tts_playback_();
    }
  }
}

// =============================================================================
// TTS PLAYBACK (decode Opus frames from server → internal speaker)
// =============================================================================

void JarvisWsAudio::process_tts_playback_() {
  if (!this->opus_decoder_ || !this->dec_output_buffer_ || !this->speaker_)
    return;

  // Start the speaker on first frame (deferred from WS callback to audio task context)
  if (!this->tts_speaker_started_ && this->tts_queue_read_ != this->tts_queue_write_) {
    this->speaker_->set_audio_stream_info(audio::AudioStreamInfo(16, 1, SAMPLE_RATE));
    this->speaker_->start();
    this->tts_speaker_started_ = true;
    ESP_LOGI(TAG, "Speaker started for TTS (%dHz 16-bit mono)", SAMPLE_RATE);
  }

  // Decode all available frames from the queue
  while (this->tts_queue_read_ != this->tts_queue_write_) {
    TtsFrame &frame = this->tts_queue_[this->tts_queue_read_];

    int decoded = opus_decode(this->opus_decoder_,
                              frame.data, frame.length,
                              this->dec_output_buffer_, OPUS_FRAME_SAMPLES, 0);
    this->tts_queue_read_ = (this->tts_queue_read_ + 1) % TTS_QUEUE_SLOTS;

    if (decoded > 0) {
      size_t pcm_bytes = decoded * sizeof(int16_t);
      // Write decoded PCM to speaker (speaker handles buffering internally)
      size_t written = this->speaker_->play((const uint8_t *)this->dec_output_buffer_, pcm_bytes);
      if (written == 0) {
        // Speaker buffer full — back off and retry next iteration
        break;
      }
    }
  }

  // Check if TTS is complete: server sent tts_done AND queue is drained
  if (this->tts_done_received_ && this->tts_queue_read_ == this->tts_queue_write_) {
    ESP_LOGI(TAG, "Internal TTS playback finished");
    if (this->tts_speaker_started_) {
      this->speaker_->stop();
      this->tts_speaker_started_ = false;
    }
    this->tts_playing_ = false;
    this->tts_done_received_ = false;
    // Signal tts_done_pending to transition to idle (processed in loop)
    this->tts_done_pending_ = true;
  }
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

bool JarvisWsAudio::init_opus_decoder_() {
  int err;
  this->opus_decoder_ = opus_decoder_create(SAMPLE_RATE, 1, &err);
  if (err != OPUS_OK || !this->opus_decoder_) {
    ESP_LOGE(TAG, "Opus decoder create failed: %d", err);
    return false;
  }

  this->dec_output_buffer_ = (int16_t *)heap_caps_malloc(
      OPUS_FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!this->dec_output_buffer_) {
    ESP_LOGE(TAG, "Opus decoder buffer alloc failed");
    return false;
  }

  // TTS frame queue (PSRAM)
  this->tts_queue_ = (TtsFrame *)heap_caps_calloc(
      TTS_QUEUE_SLOTS, sizeof(TtsFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!this->tts_queue_) {
    ESP_LOGE(TAG, "TTS queue alloc failed");
    return false;
  }

  ESP_LOGI(TAG, "Opus decoder: %dHz mono, TTS queue: %d slots", SAMPLE_RATE, TTS_QUEUE_SLOTS);
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
  ws_cfg.task_stack = 6144;
  ws_cfg.task_prio = 5;
  ws_cfg.disable_auto_reconnect = true;  // We handle reconnection ourselves

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
  this->voice_phase_ = PHASE_NOT_READY;
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
      this->voice_phase_ = PHASE_IDLE;
      this->reconnect_delay_ms_ = RECONNECT_MIN_MS;
      this->last_ping_ms_ = millis();
      this->last_data_ms_ = millis();
      // Send hello
      this->send_hello_();
      break;

    case WEBSOCKET_EVENT_DATA:
      this->last_data_ms_ = millis();
      if (data->op_code == 0x01 && data->data_ptr && data->data_len > 0) {
        // Text frame: JSON control message
        this->on_ws_text_message_(data->data_ptr, data->data_len);
      } else if (data->op_code == 0x02 && data->data_ptr && data->data_len > 0) {
        // Binary frame: TTS Opus from server → queue for playback
        if (this->tts_playing_ && this->tts_queue_ &&
            data->data_len <= TTS_MAX_FRAME_SIZE) {
          int wr = this->tts_queue_write_;
          int next_wr = (wr + 1) % TTS_QUEUE_SLOTS;
          if (next_wr != this->tts_queue_read_) {  // not full
            this->tts_queue_[wr].length = data->data_len;
            memcpy(this->tts_queue_[wr].data, data->data_ptr, data->data_len);
            this->tts_queue_write_ = next_wr;
          }
        }
      }
      break;

    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "WebSocket disconnected");
      this->conn_state_ = ConnState::DISCONNECTED;
      this->voice_phase_ = PHASE_NOT_READY;
      this->audio_session_active_ = false;
      this->mic_stop_pending_ = true;  // defer mic stop to main loop
      break;

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WebSocket error");
      this->conn_state_ = ConnState::DISCONNECTED;
      this->voice_phase_ = PHASE_NOT_READY;
      this->audio_session_active_ = false;
      this->mic_stop_pending_ = true;  // defer mic stop to main loop
      break;

    case WEBSOCKET_EVENT_CLOSED:
      ESP_LOGW(TAG, "WebSocket closed cleanly");
      this->conn_state_ = ConnState::DISCONNECTED;
      this->voice_phase_ = PHASE_NOT_READY;
      this->audio_session_active_ = false;
      this->mic_stop_pending_ = true;
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
      this->voice_phase_ = PHASE_THINKING;
      this->session_done_pending_ = true;
      this->session_done_success_ = true;
    }

  } else if (strcmp(type, "trigger_listen") == 0) {
    cJSON *silent_obj = cJSON_GetObjectItem(msg, "silent");
    this->trigger_listen_silent_ = silent_obj ? cJSON_IsTrue(silent_obj) : true;
    this->trigger_listen_pending_ = true;

  } else if (strcmp(type, "tts_start") == 0) {
    ESP_LOGI(TAG, "TTS started (replying)");
    this->voice_phase_ = PHASE_REPLYING;
    // Start internal speaker for TTS playback
    if (this->speaker_ && this->opus_decoder_ && this->speaker_type_ == "internal") {
      if (!this->tts_playing_) {
        // First tts_start: init queue and start fresh
        this->tts_queue_read_ = 0;
        this->tts_queue_write_ = 0;
        this->tts_done_received_ = false;
        this->tts_speaker_started_ = false;  // will be started on first frame in process_tts_playback_
        this->tts_playing_ = true;
        ESP_LOGI(TAG, "Internal TTS playback starting (speaker_type=%s)", this->speaker_type_.c_str());
      } else {
        // Subsequent tts_start (multi-chunk TTS): just clear tts_done flag, keep playing
        this->tts_done_received_ = false;
        ESP_LOGI(TAG, "TTS continuation (multi-chunk), already playing");
      }
    } else {
      ESP_LOGI(TAG, "TTS via external speaker (speaker_type=%s, speaker=%p, decoder=%p)",
               this->speaker_type_.c_str(), this->speaker_, this->opus_decoder_);
    }

  } else if (strcmp(type, "tts_done") == 0) {
    if (this->tts_playing_) {
      this->tts_done_received_ = true;  // drain queue before stopping
    } else {
      this->tts_done_pending_ = true;
    }

  } else if (strcmp(type, "config_update") == 0) {
    cJSON *sens = cJSON_GetObjectItem(msg, "wake_word_sensitivity");
    if (sens && cJSON_IsNumber(sens)) {
      this->config_new_sensitivity_ = (float)sens->valuedouble;
    }
    // Speaker type: "alexa" or "internal"
    const char *speaker = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "speaker_type"));
    if (speaker) {
      this->speaker_type_ = std::string(speaker);
      ESP_LOGI(TAG, "Speaker type updated: %s", speaker);
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
      this->voice_phase_ = PHASE_ERROR;
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
  // Reset ring buffer for fresh audio (mic is already running via MWW)
  this->mic_buffer_read_pos_ = this->mic_buffer_write_pos_;
  this->audio_session_requested_ = true;
  // Reset debug flag so we log the first frame of each new session
  // (static variable in process_audio_session_ — reset via external flag)
  this->debug_first_frame_ = true;
}

void JarvisWsAudio::stop_session() {
  if (this->audio_session_active_) {
    ESP_LOGI(TAG, "Stopping audio session");
    this->send_json_("audio_end");
    this->audio_session_active_ = false;
    this->conn_state_ = ConnState::CONNECTED;
    this->voice_phase_ = PHASE_IDLE;
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
  uint32_t now = millis();
  if (now - this->last_volume_change_ms_ < 200) return;  // Throttle: max 5 per second
  this->last_volume_change_ms_ = now;
  ESP_LOGI(TAG, "Sending volume_change: %s", direction.c_str());
  this->send_json_("volume_change", "direction", direction.c_str());
}

void JarvisWsAudio::send_state(const std::string &state) {
  this->send_json_("state", "state", state.c_str());
}

}  // namespace jarvis_ws_audio
}  // namespace esphome
