#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <deque>
#include <mutex>
#include <string>

namespace esphome {
namespace access_control {

static const uint8_t MAX_PROFILES = 20;
static const uint8_t MAX_CARDS = 4;
static const uint8_t MAX_FACES = 2;
static const uint8_t MAX_PALMS = 2;
static const uint8_t NAME_LEN = 24;
static const uint8_t PIN_LEN = 9;  // 4-8 digits + terminator
static const uint8_t MAX_EVENTS = 25;

// Stored in flash, one record per profile slot. Plain data only.
struct Profile {
  uint8_t used;
  uint8_t enabled;
  char name[NAME_LEN];
  uint32_t cards[MAX_CARDS];
  uint16_t faces[MAX_FACES];  // FP008 user IDs (1-500)
  uint16_t palms[MAX_PALMS];  // FP008 user IDs (501-800)
  char pin[PIN_LEN];
};

enum EnrolKind : uint8_t { ENROL_NONE = 0, ENROL_CARD, ENROL_FACE, ENROL_PALM, ENROL_PIN };

// Result of submit_enrol_pin()
enum PinEnrolResult : int { PIN_ENROL_RETRY = 0, PIN_ENROL_CONFIRM = 1, PIN_ENROL_SAVED = 2, PIN_ENROL_INACTIVE = 3 };

enum OpType : uint8_t { OP_ENROL_FACE, OP_ENROL_PALM, OP_DELETE_UID, OP_DELETE_ALL, OP_LED, OP_CHECK_STATE };

struct Op {
  OpType type;
  int8_t profile;
  uint16_t uid;
  uint8_t a;
  uint8_t b;
};

struct Event {
  uint32_t ms;
  std::string method;
  std::string name;
  std::string detail;
  bool granted;
};

class AccessControl : public Component, public uart::UARTDevice, public AsyncWebHandler {
 public:
  explicit AccessControl(web_server_base::WebServerBase *base) : base_(base) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  void set_presence_pin(GPIOPin *pin) { this->presence_pin_ = pin; }
  void set_identify_timeout(uint32_t s) { this->identify_timeout_s_ = s; }
  void set_led_feedback(bool v) { this->led_feedback_ = v; }
  void set_enrol_timeout(uint32_t s) { this->enrol_timeout_s_ = s; }

  /// Call from the wiegand on_tag automation: id(access).handle_card(x);
  void handle_card(const std::string &code);
  /// For a future keypad: returns true if the PIN belongs to an enabled profile.
  bool handle_pin(const std::string &pin);

  // ---- PIN enrolment from the door panel (started by "Set PIN" on the web page) ----
  /// True while the web page is waiting for a PIN to be typed on the panel.
  bool pin_enrol_active();
  /// What the panel should show above the PIN pad (e.g. "New PIN for Cody").
  std::string pin_enrol_prompt();
  /// Feed a PIN typed on the panel. First entry asks for confirmation, second saves it.
  int submit_enrol_pin(const std::string &pin);
  /// Panel Exit button / idle timeout.
  void cancel_pin_enrol();

  void add_on_access_callback(std::function<void(std::string, std::string, bool)> &&cb) {
    this->access_callback_.add(std::move(cb));
  }

  // AsyncWebHandler
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  // ---- profiles ----
  void load_profiles_();
  void save_dirty_();
  int find_card_(uint32_t card) const;
  int find_uid_(uint16_t uid) const;
  int find_pin_(const char *pin) const;
  void report_(const std::string &method, int profile, bool granted, const std::string &detail);
  void set_message_(const std::string &msg, bool ok);

  // ---- FP008 protocol ----
  void read_uart_();
  void process_frame_(uint8_t msg_type, const uint8_t *p, uint16_t len);
  void handle_reply_(const uint8_t *p, uint16_t len);
  void send_cmd_(uint8_t cmd, const uint8_t *payload, uint16_t len, uint32_t timeout_ms);
  void run_next_op_();
  void start_identify_();
  void led_(bool granted);

  // ---- web ----
  std::string state_json_();
  void api_(AsyncWebServerRequest *req, const std::string &path);

  web_server_base::WebServerBase *base_;
  GPIOPin *presence_pin_{nullptr};
  uint32_t identify_timeout_s_{3};
  bool led_feedback_{true};
  uint32_t enrol_timeout_s_{20};

  std::mutex mtx_;  // guards everything below that the web task touches
  Profile profiles_[MAX_PROFILES]{};
  ESPPreferenceObject prefs_[MAX_PROFILES];
  uint32_t dirty_{0};

  // enrolment (one at a time)
  EnrolKind enrol_kind_{ENROL_NONE};
  int8_t enrol_profile_{-1};
  uint32_t enrol_deadline_{0};
  std::string pin_first_;    // first entry while confirming a new PIN
  std::string pin_prompt_;   // text shown above the panel PIN pad
  std::string message_;
  bool message_ok_{true};
  uint32_t message_seq_{0};

  std::deque<Op> ops_;
  std::deque<Event> events_;

  // UART parser
  uint8_t rx_state_{0};
  uint8_t rx_hdr_[3];
  uint8_t rx_hdr_pos_{0};
  uint8_t rx_buf_[260];
  uint16_t rx_len_{0};
  uint16_t rx_pos_{0};

  // command in flight
  uint8_t inflight_cmd_{0};
  uint32_t inflight_deadline_{0};
  Op inflight_op_{};
  bool inflight_is_op_{false};
  bool fp_online_{false};
  uint32_t last_fp_reply_{0};
  uint16_t fp_face_count_{0};
  uint16_t fp_palm_count_{0};

  uint8_t note_state_{0xFF};
  uint32_t note_ms_{0};

  uint32_t identify_cooldown_until_{0};
  uint32_t last_unknown_report_{0};

  CallbackManager<void(std::string, std::string, bool)> access_callback_;
};

class AccessTrigger : public Trigger<std::string, std::string, bool> {
 public:
  explicit AccessTrigger(AccessControl *parent) {
    parent->add_on_access_callback(
        [this](const std::string &method, const std::string &name, bool granted) {
          this->trigger(method, name, granted);
        });
  }
};

}  // namespace access_control
}  // namespace esphome
