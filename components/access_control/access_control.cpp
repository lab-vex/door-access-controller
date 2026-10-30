#include "access_control.h"
#include "web_page.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace esphome {
namespace access_control {

static const char *const TAG = "access_control";

// ---- FP008 / DFRobot FP-series protocol (EF AA framing, XOR checksum) ----
static const uint8_t SYNC_1 = 0xEF;
static const uint8_t SYNC_2 = 0xAA;
static const uint8_t MSG_REPLY = 0x00;
static const uint8_t MSG_NOTE = 0x01;

static const uint8_t CMD_CHECK_STATE = 0x11;
static const uint8_t CMD_IDENTIFY = 0x12;
static const uint8_t CMD_DELETE_FACE = 0x20;
static const uint8_t CMD_DELETE_ALL = 0x21;
static const uint8_t CMD_GET_USERS = 0x24;
static const uint8_t CMD_ENROL = 0x26;
static const uint8_t CMD_DELETE_PALM = 0x80;
static const uint8_t CMD_LED = 0x90;

static const uint8_t RES_OK = 0x00;
static const uint8_t RES_REJECTED = 0x01;
static const uint8_t RES_UNKNOWN_USER = 0x08;
static const uint8_t RES_MAX_USERS = 0x09;
static const uint8_t RES_ALREADY_ENROLLED = 0x0A;
static const uint8_t RES_TIMEOUT = 0x0D;

static const uint8_t LED_GREEN = 0x00;
static const uint8_t LED_RED = 0x01;
static const uint8_t LED_ON = 0x00;
static const uint8_t LED_OFF = 0x01;

static const uint16_t PALM_UID_START = 501;
static const uint32_t CARD_ENROL_TIMEOUT_MS = 30000;
static const uint32_t PIN_ENROL_TIMEOUT_MS = 60000;

// Pending on_access callbacks are fired outside the mutex so YAML automations
// can safely call back into this component.
struct PendingReport {
  std::string method;
  std::string name;
  bool granted;
};
static std::vector<PendingReport> pending_reports;  // only touched from the main loop task

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static std::string json_escape(const char *s) {
  std::string out;
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t) c < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

static std::string clean_name(const std::string &in) {
  std::string out;
  for (char c : in) {
    if (c >= 0x20 && c <= 0x7E && c != '<' && c != '>' && c != '"' && c != '\\')
      out += c;
  }
  // trim
  size_t a = out.find_first_not_of(' ');
  if (a == std::string::npos)
    return "";
  size_t b = out.find_last_not_of(' ');
  out = out.substr(a, b - a + 1);
  if (out.size() > NAME_LEN - 1)
    out.resize(NAME_LEN - 1);
  return out;
}

static bool parse_u32(const std::string &s, uint32_t *out) {
  if (s.empty() || s.size() > 10)
    return false;
  for (char c : s)
    if (c < '0' || c > '9')
      return false;
  unsigned long long v = strtoull(s.c_str(), nullptr, 10);
  if (v == 0 || v > 0xFFFFFFFFULL)
    return false;
  *out = (uint32_t) v;
  return true;
}

static const char *kind_str(EnrolKind k) {
  switch (k) {
    case ENROL_CARD:
      return "card";
    case ENROL_FACE:
      return "face";
    case ENROL_PALM:
      return "palm";
    case ENROL_PIN:
      return "pin";
    default:
      return "";
  }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void AccessControl::setup() {
  if (this->presence_pin_ != nullptr)
    this->presence_pin_->setup();

  this->load_profiles_();

  this->base_->init();
  this->base_->add_handler(this);

  this->ops_.push_back(Op{OP_CHECK_STATE, -1, 0, 0, 0});
}

void AccessControl::dump_config() {
  ESP_LOGCONFIG(TAG, "Access control:");
  LOG_PIN("  Presence pin: ", this->presence_pin_);
  ESP_LOGCONFIG(TAG, "  Identify timeout: %" PRIu32 "s", this->identify_timeout_s_);
  ESP_LOGCONFIG(TAG, "  Enrol timeout: %" PRIu32 "s", this->enrol_timeout_s_);
  ESP_LOGCONFIG(TAG, "  LED feedback: %s", YESNO(this->led_feedback_));
  int used = 0;
  for (auto &p : this->profiles_)
    used += p.used ? 1 : 0;
  ESP_LOGCONFIG(TAG, "  Profiles: %d of %u", used, MAX_PROFILES);
  ESP_LOGCONFIG(TAG, "  Profile page: http://<device-ip>/");
}

void AccessControl::loop() {
  {
    std::lock_guard<std::mutex> lock(this->mtx_);
    const uint32_t now = millis();

    this->read_uart_();

    // command timeout
    if (this->inflight_cmd_ != 0 && (int32_t) (now - this->inflight_deadline_) > 0) {
      if (this->inflight_cmd_ == CMD_IDENTIFY) {
        ESP_LOGV(TAG, "Identify timed out without reply");
      } else {
        ESP_LOGW(TAG, "Face/palm module did not reply to command 0x%02X", this->inflight_cmd_);
      }
      if (this->inflight_is_op_ &&
          (this->inflight_op_.type == OP_ENROL_FACE || this->inflight_op_.type == OP_ENROL_PALM)) {
        this->set_message_("The face/palm module didn't respond. Check its wiring and power.", false);
        this->enrol_kind_ = ENROL_NONE;
        this->enrol_profile_ = -1;
      }
      if ((int32_t) (now - this->last_fp_reply_) > 15000)
        this->fp_online_ = false;
      this->inflight_cmd_ = 0;
      this->inflight_is_op_ = false;
      this->identify_cooldown_until_ = now + 1000;
    }

    // card enrol timeout
    if (this->enrol_kind_ == ENROL_CARD && (int32_t) (now - this->enrol_deadline_) > 0) {
      this->enrol_kind_ = ENROL_NONE;
      this->enrol_profile_ = -1;
      this->set_message_("No card scanned - enrolment timed out.", false);
    }

    // PIN enrol timeout (panel idle)
    if (this->enrol_kind_ == ENROL_PIN && (int32_t) (now - this->enrol_deadline_) > 0) {
      this->enrol_kind_ = ENROL_NONE;
      this->enrol_profile_ = -1;
      this->pin_first_.clear();
      this->set_message_("No PIN entered on the panel - timed out.", false);
    }

    if (this->inflight_cmd_ == 0)
      this->run_next_op_();

    if (this->dirty_ != 0)
      this->save_dirty_();
  }

  // fire on_access automations outside the lock
  if (!pending_reports.empty()) {
    std::vector<PendingReport> reports;
    reports.swap(pending_reports);
    for (auto &r : reports)
      this->access_callback_.call(r.method, r.name, r.granted);
  }
}

// ---------------------------------------------------------------------------
// profiles
// ---------------------------------------------------------------------------
void AccessControl::load_profiles_() {
  const uint32_t base_hash = fnv1_hash("access_control_profile_v1");
  int loaded = 0;
  for (uint8_t i = 0; i < MAX_PROFILES; i++) {
    this->prefs_[i] = global_preferences->make_preference<Profile>(base_hash + i, true);
    Profile p{};
    if (this->prefs_[i].load(&p) && p.used == 1) {
      p.name[NAME_LEN - 1] = 0;
      p.pin[PIN_LEN - 1] = 0;
      this->profiles_[i] = p;
      loaded++;
    } else {
      memset(&this->profiles_[i], 0, sizeof(Profile));
    }
  }
  ESP_LOGI(TAG, "Loaded %d profile(s)", loaded);
}

void AccessControl::save_dirty_() {
  for (uint8_t i = 0; i < MAX_PROFILES; i++) {
    if (this->dirty_ & (1UL << i))
      this->prefs_[i].save(&this->profiles_[i]);
  }
  this->dirty_ = 0;
  global_preferences->sync();
}

int AccessControl::find_card_(uint32_t card) const {
  for (int i = 0; i < MAX_PROFILES; i++) {
    const Profile &p = this->profiles_[i];
    if (!p.used)
      continue;
    for (uint32_t c : p.cards)
      if (c == card)
        return i;
  }
  return -1;
}

int AccessControl::find_uid_(uint16_t uid) const {
  for (int i = 0; i < MAX_PROFILES; i++) {
    const Profile &p = this->profiles_[i];
    if (!p.used)
      continue;
    for (uint16_t f : p.faces)
      if (f == uid)
        return i;
    for (uint16_t f : p.palms)
      if (f == uid)
        return i;
  }
  return -1;
}

int AccessControl::find_pin_(const char *pin) const {
  for (int i = 0; i < MAX_PROFILES; i++) {
    const Profile &p = this->profiles_[i];
    if (p.used && p.pin[0] != 0 && strcmp(p.pin, pin) == 0)
      return i;
  }
  return -1;
}

void AccessControl::report_(const std::string &method, int profile, bool granted, const std::string &detail) {
  std::string name = profile >= 0 ? std::string(this->profiles_[profile].name) : std::string("Unknown");
  ESP_LOGI(TAG, "Access %s: %s via %s (%s)", granted ? "GRANTED" : "DENIED", name.c_str(), method.c_str(),
           detail.c_str());
  this->events_.push_front(Event{millis(), method, name, detail, granted});
  while (this->events_.size() > MAX_EVENTS)
    this->events_.pop_back();
  pending_reports.push_back(PendingReport{method, name, granted});
  if (method == "face" || method == "palm")
    this->led_(granted);
}

void AccessControl::set_message_(const std::string &msg, bool ok) {
  this->message_ = msg;
  this->message_ok_ = ok;
  this->message_seq_++;
  ESP_LOGI(TAG, "%s", msg.c_str());
}

// ---------------------------------------------------------------------------
// card / PIN entry points (main loop)
// ---------------------------------------------------------------------------
void AccessControl::handle_card(const std::string &code) {
  {
    std::lock_guard<std::mutex> lock(this->mtx_);
    uint32_t card;
    if (!parse_u32(code, &card)) {
      ESP_LOGW(TAG, "Ignoring card with unusable number '%s'", code.c_str());
      return;
    }
    int owner = this->find_card_(card);

    if (this->enrol_kind_ == ENROL_CARD) {
      int target = this->enrol_profile_;
      this->enrol_kind_ = ENROL_NONE;
      this->enrol_profile_ = -1;
      if (target < 0 || !this->profiles_[target].used) {
        this->set_message_("That profile no longer exists.", false);
      } else if (owner >= 0) {
        this->set_message_("Card " + code + " is already assigned to " + this->profiles_[owner].name + ".", false);
      } else {
        Profile &p = this->profiles_[target];
        bool added = false;
        for (auto &c : p.cards) {
          if (c == 0) {
            c = card;
            added = true;
            break;
          }
        }
        if (added) {
          this->dirty_ |= 1UL << target;
          this->set_message_("Card " + code + " added to " + p.name + ".", true);
        } else {
          this->set_message_(std::string(p.name) + " already has the maximum number of cards.", false);
        }
      }
    } else if (owner >= 0) {
      bool ok = this->profiles_[owner].enabled;
      this->report_("card", owner, ok, ok ? "card " + code : "profile disabled");
    } else {
      this->report_("card", -1, false, "unregistered card " + code);
    }
  }
  if (!pending_reports.empty()) {
    std::vector<PendingReport> reports;
    reports.swap(pending_reports);
    for (auto &r : reports)
      this->access_callback_.call(r.method, r.name, r.granted);
  }
}

bool AccessControl::handle_pin(const std::string &pin) {
  bool ok = false;
  {
    std::lock_guard<std::mutex> lock(this->mtx_);
    int owner = pin.size() >= 4 && pin.size() < PIN_LEN ? this->find_pin_(pin.c_str()) : -1;
    if (owner >= 0) {
      ok = this->profiles_[owner].enabled;
      this->report_("pin", owner, ok, ok ? "PIN" : "profile disabled");
    } else {
      this->report_("pin", -1, false, "wrong PIN");
    }
  }
  std::vector<PendingReport> reports;
  reports.swap(pending_reports);
  for (auto &r : reports)
    this->access_callback_.call(r.method, r.name, r.granted);
  return ok;
}

bool AccessControl::pin_enrol_active() {
  std::lock_guard<std::mutex> lock(this->mtx_);
  return this->enrol_kind_ == ENROL_PIN;
}

std::string AccessControl::pin_enrol_prompt() {
  std::lock_guard<std::mutex> lock(this->mtx_);
  return this->pin_prompt_;
}

int AccessControl::submit_enrol_pin(const std::string &pin) {
  std::lock_guard<std::mutex> lock(this->mtx_);
  if (this->enrol_kind_ != ENROL_PIN)
    return PIN_ENROL_INACTIVE;
  const int idx = this->enrol_profile_;
  if (idx < 0 || !this->profiles_[idx].used) {
    this->enrol_kind_ = ENROL_NONE;
    this->enrol_profile_ = -1;
    this->set_message_("That profile no longer exists.", false);
    return PIN_ENROL_INACTIVE;
  }
  Profile &p = this->profiles_[idx];
  this->enrol_deadline_ = millis() + PIN_ENROL_TIMEOUT_MS;

  bool valid = pin.size() >= 4 && pin.size() <= PIN_LEN - 1;
  for (char ch : pin)
    valid &= ch >= '0' && ch <= '9';
  if (!valid) {
    this->pin_first_.clear();
    this->pin_prompt_ = "PIN must be 4-8 digits.\nNew PIN for " + std::string(p.name);
    return PIN_ENROL_RETRY;
  }

  if (this->pin_first_.empty()) {
    int owner = this->find_pin_(pin.c_str());
    if (owner >= 0 && owner != idx) {
      this->pin_prompt_ = "PIN already in use.\nNew PIN for " + std::string(p.name);
      this->set_message_("That PIN is already used by another profile - try a different one.", false);
      return PIN_ENROL_RETRY;
    }
    this->pin_first_ = pin;
    this->pin_prompt_ = "Enter it again to confirm";
    this->set_message_("Now enter the same PIN again on the panel to confirm.", true);
    return PIN_ENROL_CONFIRM;
  }

  if (pin != this->pin_first_) {
    this->pin_first_.clear();
    this->pin_prompt_ = "Didn't match.\nNew PIN for " + std::string(p.name);
    this->set_message_("The two PINs didn't match - enter a new PIN on the panel.", false);
    return PIN_ENROL_RETRY;
  }

  memset(p.pin, 0, PIN_LEN);
  strncpy(p.pin, pin.c_str(), PIN_LEN - 1);
  this->dirty_ |= 1UL << idx;
  this->pin_first_.clear();
  this->enrol_kind_ = ENROL_NONE;
  this->enrol_profile_ = -1;
  this->set_message_("PIN saved for " + std::string(p.name) + ".", true);
  return PIN_ENROL_SAVED;
}

void AccessControl::cancel_pin_enrol() {
  std::lock_guard<std::mutex> lock(this->mtx_);
  if (this->enrol_kind_ != ENROL_PIN)
    return;
  this->enrol_kind_ = ENROL_NONE;
  this->enrol_profile_ = -1;
  this->pin_first_.clear();
  this->set_message_("PIN setup cancelled on the panel.", false);
}

// ---------------------------------------------------------------------------
// FP008 UART
// ---------------------------------------------------------------------------
void AccessControl::send_cmd_(uint8_t cmd, const uint8_t *payload, uint16_t len, uint32_t timeout_ms) {
  uint8_t hdr[5] = {SYNC_1, SYNC_2, cmd, (uint8_t) (len >> 8), (uint8_t) (len & 0xFF)};
  uint8_t chk = cmd ^ hdr[3] ^ hdr[4];
  for (uint16_t i = 0; i < len; i++)
    chk ^= payload[i];
  this->write_array(hdr, 5);
  if (len > 0)
    this->write_array(payload, len);
  this->write_byte(chk);
  this->inflight_cmd_ = cmd;
  this->inflight_deadline_ = millis() + timeout_ms;
  ESP_LOGV(TAG, "TX cmd 0x%02X len %u", cmd, len);
}

void AccessControl::read_uart_() {
  uint8_t b;
  while (this->available() && this->read_byte(&b)) {
    switch (this->rx_state_) {
      case 0:  // sync 1
        if (b == SYNC_1)
          this->rx_state_ = 1;
        break;
      case 1:  // sync 2
        if (b == SYNC_2) {
          this->rx_state_ = 2;
          this->rx_hdr_pos_ = 0;
        } else if (b != SYNC_1) {
          this->rx_state_ = 0;
        }
        break;
      case 2:  // type, len hi, len lo
        this->rx_hdr_[this->rx_hdr_pos_++] = b;
        if (this->rx_hdr_pos_ == 3) {
          this->rx_len_ = (this->rx_hdr_[1] << 8) | this->rx_hdr_[2];
          this->rx_pos_ = 0;
          if (this->rx_len_ > sizeof(this->rx_buf_)) {
            ESP_LOGW(TAG, "Frame too long (%u), resyncing", this->rx_len_);
            this->rx_state_ = 0;
          } else {
            this->rx_state_ = this->rx_len_ == 0 ? 4 : 3;
          }
        }
        break;
      case 3:  // payload
        this->rx_buf_[this->rx_pos_++] = b;
        if (this->rx_pos_ >= this->rx_len_)
          this->rx_state_ = 4;
        break;
      case 4: {  // checksum
        uint8_t chk = this->rx_hdr_[0] ^ this->rx_hdr_[1] ^ this->rx_hdr_[2];
        for (uint16_t i = 0; i < this->rx_len_; i++)
          chk ^= this->rx_buf_[i];
        if (chk == b) {
          this->process_frame_(this->rx_hdr_[0], this->rx_buf_, this->rx_len_);
        } else {
          ESP_LOGW(TAG, "Bad checksum from face/palm module (got 0x%02X, want 0x%02X)", b, chk);
        }
        this->rx_state_ = 0;
        break;
      }
      default:
        this->rx_state_ = 0;
    }
  }
}

void AccessControl::process_frame_(uint8_t msg_type, const uint8_t *p, uint16_t len) {
  this->last_fp_reply_ = millis();
  if (!this->fp_online_) {
    this->fp_online_ = true;
    ESP_LOGI(TAG, "Face/palm module is responding");
  }
  if (msg_type == MSG_REPLY) {
    if (len >= 2)
      this->handle_reply_(p, len);
  } else if (msg_type == MSG_NOTE) {
    // p[0] = note id (0x01 = detection status), p[1] = status (0 = target found, 1 = nothing in view)
    if (len >= 2 && p[0] == 0x01) {
      this->note_state_ = p[1];
      this->note_ms_ = millis();
    }
    ESP_LOGV(TAG, "Module note: %s", format_hex_pretty(p, len).c_str());
  } else {
    ESP_LOGD(TAG, "Module frame type 0x%02X: %s", msg_type, format_hex_pretty(p, len).c_str());
  }
}

void AccessControl::handle_reply_(const uint8_t *p, uint16_t len) {
  const uint8_t cmd = p[0];
  const uint8_t result = p[1];
  if (cmd != this->inflight_cmd_) {
    ESP_LOGD(TAG, "Unexpected reply to 0x%02X (result 0x%02X)", cmd, result);
    return;
  }
  const bool was_op = this->inflight_is_op_;
  const Op op = this->inflight_op_;
  this->inflight_cmd_ = 0;
  this->inflight_is_op_ = false;
  const uint32_t now = millis();

  switch (cmd) {
    case CMD_IDENTIFY: {
      if (result == RES_OK && len >= 4) {
        uint16_t uid = (p[2] << 8) | p[3];
        const char *method = uid >= PALM_UID_START ? "palm" : "face";
        int owner = this->find_uid_(uid);
        if (owner >= 0) {
          bool ok = this->profiles_[owner].enabled;
          this->report_(method, owner, ok, ok ? std::string(method) + " #" + to_string(uid) : "profile disabled");
        } else {
          this->report_(method, -1, false, "enrolled but not linked to a profile (#" + to_string(uid) + ")");
        }
        this->identify_cooldown_until_ = now + 4000;
      } else if (result == RES_UNKNOWN_USER || result == RES_REJECTED) {
        if ((int32_t) (now - this->last_unknown_report_) > 10000) {
          this->report_("face/palm", -1, false, "not recognised");
          this->last_unknown_report_ = now;
        }
        this->identify_cooldown_until_ = now + 1500;
      } else {
        // timeout / nothing in view / liveness failure: try again shortly
        ESP_LOGV(TAG, "Identify result 0x%02X", result);
        this->identify_cooldown_until_ = now + 200;
      }
      break;
    }

    case CMD_ENROL: {
      const bool palm = was_op && op.type == OP_ENROL_PALM;
      const char *what = palm ? "Palm" : "Face";
      const bool still_wanted = was_op && this->enrol_profile_ == op.profile &&
                                this->enrol_kind_ == (palm ? ENROL_PALM : ENROL_FACE);
      if (result == RES_OK && len >= 4) {
        uint16_t uid = (p[2] << 8) | p[3];
        bool stored = false;
        if (still_wanted && op.profile >= 0 && this->profiles_[op.profile].used) {
          Profile &pr = this->profiles_[op.profile];
          uint16_t *slots = palm ? pr.palms : pr.faces;
          uint8_t n = palm ? MAX_PALMS : MAX_FACES;
          for (uint8_t i = 0; i < n; i++) {
            if (slots[i] == 0) {
              slots[i] = uid;
              stored = true;
              break;
            }
          }
          if (stored) {
            this->dirty_ |= 1UL << op.profile;
            this->set_message_(std::string(what) + " added to " + pr.name + ".", true);
          }
        }
        if (!stored) {
          // cancelled or no room: don't leave an orphan on the module
          this->ops_.push_back(Op{OP_DELETE_UID, -1, uid, 0, 0});
          if (still_wanted)
            this->set_message_(std::string("No room for another ") + (palm ? "palm" : "face") + ".", false);
        }
      } else if (still_wanted) {
        if (result == RES_ALREADY_ENROLLED)
          this->set_message_(std::string("That ") + (palm ? "palm" : "face") +
                                 " is already registered (possibly to another profile).",
                             false);
        else if (result == RES_TIMEOUT)
          this->set_message_(std::string("Didn't see a ") + (palm ? "palm" : "face") + " in time - try again.",
                             false);
        else if (result == RES_MAX_USERS)
          this->set_message_("The face/palm module is full.", false);
        else
          this->set_message_(std::string(what) + " enrolment failed (code " + to_string(result) + ").", false);
      }
      if (still_wanted) {
        this->enrol_kind_ = ENROL_NONE;
        this->enrol_profile_ = -1;
      }
      this->identify_cooldown_until_ = now + 2000;
      break;
    }

    case CMD_DELETE_FACE:
    case CMD_DELETE_PALM:
      ESP_LOGI(TAG, "Module delete #%u: %s", op.uid, result == RES_OK ? "ok" : "not found/failed");
      break;

    case CMD_DELETE_ALL:
      if (result == RES_OK)
        this->set_message_("All faces and palms erased from the module.", true);
      else
        this->set_message_("Module erase failed (code " + to_string(result) + ").", false);
      break;

    case CMD_CHECK_STATE:
      ESP_LOGI(TAG, "Face/palm module state: %s", len >= 3 && p[2] == 0 ? "ready" : "busy/error");
      this->ops_.push_back(Op{OP_CHECK_STATE, -1, 0, 1, 0});  // follow up: user counts
      break;

    case CMD_GET_USERS:
      if (result == RES_OK && len >= 4) {
        uint16_t n = (p[2] << 8) | p[3];
        if (was_op && op.a == 1) {
          this->fp_face_count_ = n;
          this->ops_.push_back(Op{OP_CHECK_STATE, -1, 0, 2, 0});
        } else {
          this->fp_palm_count_ = n;
          ESP_LOGI(TAG, "Module holds %u face(s) and %u palm(s)", this->fp_face_count_, this->fp_palm_count_);
        }
      }
      break;

    default:
      break;
  }
}

void AccessControl::run_next_op_() {
  const uint32_t now = millis();
  if (!this->ops_.empty()) {
    Op op = this->ops_.front();
    this->ops_.pop_front();
    this->inflight_op_ = op;
    this->inflight_is_op_ = true;
    switch (op.type) {
      case OP_ENROL_FACE:
      case OP_ENROL_PALM: {
        const bool palm = op.type == OP_ENROL_PALM;
        // skip if cancelled while queued
        if (this->enrol_profile_ != op.profile || this->enrol_kind_ != (palm ? ENROL_PALM : ENROL_FACE)) {
          this->inflight_is_op_ = false;
          return;
        }
        uint8_t d[40]{};
        d[0] = 0;  // normal user
        std::string tag = "P" + to_string(op.profile + 1);
        memcpy(&d[1], tag.c_str(), std::min<size_t>(tag.size(), 32));
        d[33] = palm ? 0xFD : 0x00;
        d[34] = 0x01;  // single frame
        d[35] = 0x00;  // no duplicate faces
        d[36] = (uint8_t) this->enrol_timeout_s_;
        this->note_state_ = 0xFF;
        this->set_message_(std::string(palm ? "Hold your palm flat, fingers together, 15-20 cm in front of the scanner"
                                            : "Look straight at the scanner from 40-60 cm") +
                               " (" + to_string(this->enrol_timeout_s_) + " s)...",
                           true);
        this->send_cmd_(CMD_ENROL, d, sizeof(d), (this->enrol_timeout_s_ + 3) * 1000);
        break;
      }
      case OP_DELETE_UID: {
        const bool palm = op.uid >= PALM_UID_START;
        uint8_t d[3] = {(uint8_t) (op.uid >> 8), (uint8_t) (op.uid & 0xFF), (uint8_t) (palm ? 0x01 : 0x00)};
        this->send_cmd_(palm ? CMD_DELETE_PALM : CMD_DELETE_FACE, d, 3, 7000);
        break;
      }
      case OP_DELETE_ALL: {
        uint8_t d[5]{};
        this->send_cmd_(CMD_DELETE_ALL, d, 5, 6000);
        break;
      }
      case OP_LED: {
        uint8_t d[2] = {op.a, op.b};
        this->send_cmd_(CMD_LED, d, 2, 1000);
        break;
      }
      case OP_CHECK_STATE: {
        if (op.a == 0) {
          this->send_cmd_(CMD_CHECK_STATE, nullptr, 0, 2000);
        } else {
          uint8_t d[1] = {op.a};  // 1 = face count, 2 = palm count
          this->send_cmd_(CMD_GET_USERS, d, 1, 1500);
        }
        break;
      }
    }
    return;
  }

  // Nothing queued: look for faces/palms, unless a face/palm enrolment is pending
  if (this->enrol_kind_ == ENROL_FACE || this->enrol_kind_ == ENROL_PALM)
    return;
  if ((int32_t) (now - this->identify_cooldown_until_) < 0)
    return;
  if (this->presence_pin_ != nullptr && !this->presence_pin_->digital_read())
    return;
  this->start_identify_();
}

void AccessControl::start_identify_() {
  uint8_t d[2] = {0x00, (uint8_t) this->identify_timeout_s_};
  this->inflight_is_op_ = false;
  this->send_cmd_(CMD_IDENTIFY, d, 2, (this->identify_timeout_s_ + 2) * 1000);
}

void AccessControl::led_(bool granted) {
  if (!this->led_feedback_)
    return;
  const uint8_t colour = granted ? LED_GREEN : LED_RED;
  this->ops_.push_back(Op{OP_LED, -1, 0, colour, LED_ON});
  this->set_timeout("led_off", 2000, [this, colour]() {
    std::lock_guard<std::mutex> lock(this->mtx_);
    this->ops_.push_back(Op{OP_LED, -1, 0, colour, LED_OFF});
  });
}

// ---------------------------------------------------------------------------
// web
// ---------------------------------------------------------------------------
bool AccessControl::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  std::string path(url.c_str(), url.size());
  if (request->method() == HTTP_GET && (path == "/" || path == "/api/state"))
    return true;
  return request->method() == HTTP_POST && path.rfind("/api/", 0) == 0;
}

void AccessControl::handleRequest(AsyncWebServerRequest *req) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = req->url_to(buf);
  std::string path(url.c_str(), url.size());

  if (path == "/") {
    req->send(200, "text/html", INDEX_HTML);
    return;
  }
  if (path == "/api/state") {
    std::string body;
    {
      std::lock_guard<std::mutex> lock(this->mtx_);
      body = this->state_json_();
    }
    auto *res = req->beginResponse(200, "application/json", body);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
    return;
  }
  this->api_(req, path);
}

std::string AccessControl::state_json_() {
  const uint32_t now = millis();
  std::string j;
  j.reserve(4096);
  j += "{\"fp\":{\"online\":";
  j += this->fp_online_ ? "true" : "false";
  j += ",\"busy\":";
  j += (this->inflight_cmd_ != 0 && this->inflight_cmd_ != CMD_IDENTIFY) || !this->ops_.empty() ? "true" : "false";
  j += "},\"enrol\":{\"kind\":\"";
  j += kind_str(this->enrol_kind_);
  j += "\",\"profile\":" + to_string(this->enrol_profile_ + 1);
  j += ",\"hint\":\"";
  if (this->enrol_kind_ == ENROL_PIN) {
    j += this->pin_first_.empty() ? "Waiting for the PIN on the panel" : "Waiting for confirmation on the panel";
  } else if (this->inflight_cmd_ == CMD_ENROL && (now - this->note_ms_) < 1500) {
    if (this->note_state_ == 0)
      j += "Detected - hold still";
    else if (this->note_state_ == 1)
      j += "Nothing detected yet";
    else
      j += "Detected - adjust position (" + to_string(this->note_state_) + ")";
  }
  j += "\"}";
  j += ",\"message\":{\"seq\":" + to_string(this->message_seq_) + ",\"ok\":" + (this->message_ok_ ? "true" : "false") +
       ",\"text\":\"" + json_escape(this->message_.c_str()) + "\"}";
  j += ",\"limits\":{\"profiles\":" + to_string(MAX_PROFILES) + ",\"cards\":" + to_string(MAX_CARDS) +
       ",\"faces\":" + to_string(MAX_FACES) + ",\"palms\":" + to_string(MAX_PALMS) + "}";
  j += ",\"profiles\":[";
  bool first = true;
  for (int i = 0; i < MAX_PROFILES; i++) {
    const Profile &p = this->profiles_[i];
    if (!p.used)
      continue;
    if (!first)
      j += ",";
    first = false;
    j += "{\"id\":" + to_string(i + 1) + ",\"name\":\"" + json_escape(p.name) + "\",\"enabled\":" +
         (p.enabled ? "true" : "false") + ",\"pin\":" + (p.pin[0] ? "true" : "false") + ",\"cards\":[";
    bool f2 = true;
    for (uint32_t c : p.cards) {
      if (!c)
        continue;
      if (!f2)
        j += ",";
      f2 = false;
      j += to_string(c);
    }
    j += "],\"faces\":[";
    f2 = true;
    for (uint16_t u : p.faces) {
      if (!u)
        continue;
      if (!f2)
        j += ",";
      f2 = false;
      j += to_string(u);
    }
    j += "],\"palms\":[";
    f2 = true;
    for (uint16_t u : p.palms) {
      if (!u)
        continue;
      if (!f2)
        j += ",";
      f2 = false;
      j += to_string(u);
    }
    j += "]}";
  }
  j += "],\"events\":[";
  first = true;
  for (auto &e : this->events_) {
    if (!first)
      j += ",";
    first = false;
    j += "{\"age\":" + to_string((now - e.ms) / 1000) + ",\"method\":\"" + json_escape(e.method.c_str()) +
         "\",\"name\":\"" + json_escape(e.name.c_str()) + "\",\"granted\":" + (e.granted ? "true" : "false") +
         ",\"detail\":\"" + json_escape(e.detail.c_str()) + "\"}";
  }
  j += "]}";
  return j;
}

static void send_json(AsyncWebServerRequest *req, bool ok, const std::string &error = "") {
  std::string body = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"" + json_escape(error.c_str()) + "\"}";
  req->send(ok ? 200 : 400, "application/json", body.c_str());
}

void AccessControl::api_(AsyncWebServerRequest *req, const std::string &path) {
  std::lock_guard<std::mutex> lock(this->mtx_);

  // Resolve ?id= to a used profile slot where needed
  int idx = -1;
  if (req->hasArg("id")) {
    int id = atoi(req->arg("id").c_str());
    if (id >= 1 && id <= MAX_PROFILES && this->profiles_[id - 1].used)
      idx = id - 1;
  }
  auto need_profile = [&]() -> bool {
    if (idx < 0) {
      send_json(req, false, "Profile not found.");
      return false;
    }
    return true;
  };

  if (path == "/api/profile/add") {
    std::string name = clean_name(req->arg("name"));
    if (name.empty())
      return send_json(req, false, "Enter a name.");
    for (auto &p : this->profiles_)
      if (p.used && strcasecmp(p.name, name.c_str()) == 0)
        return send_json(req, false, "A profile with that name already exists.");
    for (int i = 0; i < MAX_PROFILES; i++) {
      Profile &p = this->profiles_[i];
      if (p.used)
        continue;
      memset(&p, 0, sizeof(Profile));
      p.used = 1;
      p.enabled = 1;
      strncpy(p.name, name.c_str(), NAME_LEN - 1);
      this->dirty_ |= 1UL << i;
      this->set_message_("Profile " + name + " created.", true);
      return send_json(req, true);
    }
    return send_json(req, false, "All profile slots are in use.");
  }

  if (path == "/api/profile/delete") {
    if (!need_profile())
      return;
    Profile &p = this->profiles_[idx];
    for (uint16_t u : p.faces)
      if (u)
        this->ops_.push_back(Op{OP_DELETE_UID, -1, u, 0, 0});
    for (uint16_t u : p.palms)
      if (u)
        this->ops_.push_back(Op{OP_DELETE_UID, -1, u, 0, 0});
    if (this->enrol_profile_ == idx) {
      this->enrol_kind_ = ENROL_NONE;
      this->enrol_profile_ = -1;
    }
    std::string name = p.name;
    memset(&p, 0, sizeof(Profile));
    this->dirty_ |= 1UL << idx;
    this->set_message_("Profile " + name + " deleted.", true);
    return send_json(req, true);
  }

  if (path == "/api/profile/update") {
    if (!need_profile())
      return;
    Profile &p = this->profiles_[idx];
    if (req->hasArg("name")) {
      std::string name = clean_name(req->arg("name"));
      if (name.empty())
        return send_json(req, false, "Enter a name.");
      for (int i = 0; i < MAX_PROFILES; i++)
        if (i != idx && this->profiles_[i].used && strcasecmp(this->profiles_[i].name, name.c_str()) == 0)
          return send_json(req, false, "A profile with that name already exists.");
      memset(p.name, 0, NAME_LEN);
      strncpy(p.name, name.c_str(), NAME_LEN - 1);
    }
    if (req->hasArg("enabled"))
      p.enabled = req->arg("enabled") == "1" ? 1 : 0;
    this->dirty_ |= 1UL << idx;
    return send_json(req, true);
  }

  if (path == "/api/enrol") {
    if (!need_profile())
      return;
    if (this->enrol_kind_ != ENROL_NONE)
      return send_json(req, false, "Another enrolment is already in progress.");
    std::string kind = req->arg("kind");
    Profile &p = this->profiles_[idx];
    if (kind == "card") {
      bool room = false;
      for (uint32_t c : p.cards)
        room |= c == 0;
      if (!room)
        return send_json(req, false, "This profile already has the maximum number of cards.");
      this->enrol_kind_ = ENROL_CARD;
      this->enrol_profile_ = idx;
      this->enrol_deadline_ = millis() + CARD_ENROL_TIMEOUT_MS;
      this->set_message_("Scan the card on the reader now (30 s)...", true);
      return send_json(req, true);
    }
    if (kind == "face" || kind == "palm") {
      const bool palm = kind == "palm";
      if (!this->fp_online_)
        return send_json(req, false, "The face/palm module isn't responding.");
      bool room = false;
      if (palm) {
        for (uint16_t u : p.palms)
          room |= u == 0;
      } else {
        for (uint16_t u : p.faces)
          room |= u == 0;
      }
      if (!room)
        return send_json(req, false, std::string("This profile already has the maximum number of ") +
                                         (palm ? "palms." : "faces."));
      this->enrol_kind_ = palm ? ENROL_PALM : ENROL_FACE;
      this->enrol_profile_ = idx;
      this->ops_.push_back(Op{palm ? OP_ENROL_PALM : OP_ENROL_FACE, (int8_t) idx, 0, 0, 0});
      this->set_message_("Getting the scanner ready...", true);
      return send_json(req, true);
    }
    if (kind == "pin") {
      this->enrol_kind_ = ENROL_PIN;
      this->enrol_profile_ = idx;
      this->enrol_deadline_ = millis() + PIN_ENROL_TIMEOUT_MS;
      this->pin_first_.clear();
      this->pin_prompt_ = "New PIN for " + std::string(p.name);
      this->set_message_("Type the new PIN on the door panel, then press the tick.", true);
      return send_json(req, true);
    }
    return send_json(req, false, "Unknown enrolment type.");
  }

  if (path == "/api/enrol/cancel") {
    this->pin_first_.clear();
    if (this->enrol_kind_ != ENROL_NONE) {
      this->enrol_kind_ = ENROL_NONE;
      this->enrol_profile_ = -1;
      this->set_message_("Enrolment cancelled.", false);
    }
    return send_json(req, true);
  }

  if (path == "/api/card/add") {
    if (!need_profile())
      return;
    uint32_t card;
    if (!parse_u32(req->arg("card"), &card))
      return send_json(req, false, "Enter a valid card number.");
    int owner = this->find_card_(card);
    if (owner >= 0)
      return send_json(req, false, std::string("That card is already assigned to ") + this->profiles_[owner].name + ".");
    for (auto &c : this->profiles_[idx].cards) {
      if (c == 0) {
        c = card;
        this->dirty_ |= 1UL << idx;
        return send_json(req, true);
      }
    }
    return send_json(req, false, "This profile already has the maximum number of cards.");
  }

  if (path == "/api/card/remove") {
    if (!need_profile())
      return;
    uint32_t card;
    if (!parse_u32(req->arg("card"), &card))
      return send_json(req, false, "Bad card number.");
    for (auto &c : this->profiles_[idx].cards) {
      if (c == card) {
        c = 0;
        this->dirty_ |= 1UL << idx;
        return send_json(req, true);
      }
    }
    return send_json(req, false, "Card not found on this profile.");
  }

  if (path == "/api/bio/remove") {
    if (!need_profile())
      return;
    uint32_t uid;
    if (!parse_u32(req->arg("uid"), &uid) || uid > 0xFFFF)
      return send_json(req, false, "Bad ID.");
    Profile &p = this->profiles_[idx];
    for (auto &u : p.faces)
      if (u == uid)
        u = 0;
    for (auto &u : p.palms)
      if (u == uid)
        u = 0;
    this->ops_.push_back(Op{OP_DELETE_UID, -1, (uint16_t) uid, 0, 0});
    this->dirty_ |= 1UL << idx;
    return send_json(req, true);
  }

  if (path == "/api/pin") {
    if (!need_profile())
      return;
    std::string pin = req->arg("pin");
    Profile &p = this->profiles_[idx];
    if (pin.empty()) {
      memset(p.pin, 0, PIN_LEN);
      this->dirty_ |= 1UL << idx;
      return send_json(req, true);
    }
    if (pin.size() < 4 || pin.size() > PIN_LEN - 1)
      return send_json(req, false, "PIN must be 4-8 digits.");
    for (char c : pin)
      if (c < '0' || c > '9')
        return send_json(req, false, "PIN must be digits only.");
    int owner = this->find_pin_(pin.c_str());
    if (owner >= 0 && owner != idx)
      return send_json(req, false, "That PIN is already used by another profile.");
    memset(p.pin, 0, PIN_LEN);
    strncpy(p.pin, pin.c_str(), PIN_LEN - 1);
    this->dirty_ |= 1UL << idx;
    return send_json(req, true);
  }

  if (path == "/api/fp/wipe") {
    for (int i = 0; i < MAX_PROFILES; i++) {
      Profile &p = this->profiles_[i];
      if (!p.used)
        continue;
      memset(p.faces, 0, sizeof(p.faces));
      memset(p.palms, 0, sizeof(p.palms));
      this->dirty_ |= 1UL << i;
    }
    if (this->enrol_kind_ == ENROL_FACE || this->enrol_kind_ == ENROL_PALM) {
      this->enrol_kind_ = ENROL_NONE;
      this->enrol_profile_ = -1;
    }
    this->ops_.push_back(Op{OP_DELETE_ALL, -1, 0, 0, 0});
    return send_json(req, true);
  }

  send_json(req, false, "Unknown request.");
}

}  // namespace access_control
}  // namespace esphome
