/**
 * Copyright 2020 The Magma Authors.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/util/time_util.h>

#include "CreditKey.h"
#include "EnumToString.h"
#include "RuleStore.h"
#include "SessionState.h"
#include "StoredState.h"
#include "MetricsHelpers.h"
#include "magma_logging.h"

namespace {
const char* LABEL_IMSI      = "IMSI";
const char* LABEL_APN       = "apn";
const char* LABEL_MSISDN    = "msisdn";
const char* LABEL_DIRECTION = "direction";
const char* DIRECTION_UP    = "up";
const char* DIRECTION_DOWN  = "down";
}  // namespace

using magma::service303::increment_counter;

namespace magma {

std::unique_ptr<SessionState> SessionState::unmarshal(
    const StoredSessionState& marshaled, StaticRuleStore& rule_store) {
  return std::make_unique<SessionState>(marshaled, rule_store);
}

StoredSessionState SessionState::marshal() {
  StoredSessionState marshaled{};

  marshaled.fsm_state              = curr_state_;
  marshaled.config                 = config_;
  marshaled.imsi                   = imsi_;
  marshaled.session_id             = session_id_;
  marshaled.subscriber_quota_state = subscriber_quota_state_;
  marshaled.tgpp_context           = tgpp_context_;
  marshaled.request_number         = request_number_;
  marshaled.pdp_start_time         = pdp_start_time_;
  marshaled.pdp_end_time           = pdp_end_time_;
  marshaled.pending_event_triggers = pending_event_triggers_;
  marshaled.revalidation_time      = revalidation_time_;
  marshaled.bearer_id_by_policy = bearer_id_by_policy_;

  marshaled.monitor_map = StoredMonitorMap();
  for (auto& monitor_pair : monitor_map_) {
    StoredMonitor monitor{};
    monitor.credit = monitor_pair.second->credit.marshal();
    monitor.level  = monitor_pair.second->level;
    marshaled.monitor_map[monitor_pair.first] = monitor;
  }
  marshaled.session_level_key = session_level_key_;

  marshaled.credit_map = StoredChargingCreditMap(4, &ccHash, &ccEqual);
  for (auto& credit_pair : credit_map_) {
    auto key                  = CreditKey();
    key.rating_group          = credit_pair.first.rating_group;
    key.service_identifier    = credit_pair.first.service_identifier;
    marshaled.credit_map[key] = credit_pair.second->marshal();
  }

  for (auto& rule_id : active_static_rules_) {
    marshaled.static_rule_ids.push_back(rule_id);
  }
  std::vector<PolicyRule> dynamic_rules;
  dynamic_rules_.get_rules(dynamic_rules);
  marshaled.dynamic_rules = std::move(dynamic_rules);

  std::vector<PolicyRule> gy_dynamic_rules;
  gy_dynamic_rules_.get_rules(gy_dynamic_rules);
  marshaled.gy_dynamic_rules = std::move(gy_dynamic_rules);

  for (auto& rule_id : scheduled_static_rules_) {
    marshaled.scheduled_static_rules.insert(rule_id);
  }
  std::vector<PolicyRule> scheduled_dynamic_rules;
  scheduled_dynamic_rules_.get_rules(scheduled_dynamic_rules);
  marshaled.scheduled_dynamic_rules = std::move(scheduled_dynamic_rules);
  for (auto& it : rule_lifetimes_) {
    marshaled.rule_lifetimes[it.first] = it.second;
  }

  return marshaled;
}

SessionState::SessionState(
    const StoredSessionState& marshaled, StaticRuleStore& rule_store)
    : imsi_(marshaled.imsi),
      session_id_(marshaled.session_id),
      request_number_(marshaled.request_number),
      curr_state_(marshaled.fsm_state),
      config_(marshaled.config),
      pdp_start_time_(marshaled.pdp_start_time),
      pdp_end_time_(marshaled.pdp_end_time),
      subscriber_quota_state_(marshaled.subscriber_quota_state),
      tgpp_context_(marshaled.tgpp_context),
      static_rules_(rule_store),
      pending_event_triggers_(marshaled.pending_event_triggers),
      revalidation_time_(marshaled.revalidation_time),
      credit_map_(4, &ccHash, &ccEqual),
      bearer_id_by_policy_(marshaled.bearer_id_by_policy) {
  session_level_key_ = marshaled.session_level_key;
  for (auto it : marshaled.monitor_map) {
    Monitor monitor;
    monitor.credit = SessionCredit(it.second.credit);
    monitor.level  = it.second.level;

    monitor_map_[it.first] = std::make_unique<Monitor>(monitor);
  }

  for (const auto& it : marshaled.credit_map) {
    credit_map_[it.first] =
        std::make_unique<ChargingGrant>(ChargingGrant(it.second));
  }

  for (const std::string& rule_id : marshaled.static_rule_ids) {
    active_static_rules_.push_back(rule_id);
  }
  for (auto& rule : marshaled.dynamic_rules) {
    dynamic_rules_.insert_rule(rule);
  }

  for (const std::string& rule_id : marshaled.scheduled_static_rules) {
    scheduled_static_rules_.insert(rule_id);
  }
  for (auto& rule : marshaled.scheduled_dynamic_rules) {
    scheduled_dynamic_rules_.insert_rule(rule);
  }
  for (auto& it : marshaled.rule_lifetimes) {
    rule_lifetimes_[it.first] = it.second;
  }
  for (auto& rule : marshaled.gy_dynamic_rules) {
    gy_dynamic_rules_.insert_rule(rule);
  }
}

SessionState::SessionState(
    const std::string& imsi, const std::string& session_id,
    const SessionConfig& cfg, StaticRuleStore& rule_store,
    const magma::lte::TgppContext& tgpp_context, uint64_t pdp_start_time)
    : imsi_(imsi),
      session_id_(session_id),
      // Request number set to 1, because request 0 is INIT call
      request_number_(1),
      curr_state_(SESSION_ACTIVE),
      config_(cfg),
      pdp_start_time_(pdp_start_time),
      pdp_end_time_(0),
      tgpp_context_(tgpp_context),
      static_rules_(rule_store),
      credit_map_(4, &ccHash, &ccEqual) {}

static UsageMonitorUpdate make_usage_monitor_update(
    const SessionCredit::Usage& usage_in, const std::string& monitoring_key,
    MonitoringLevel level) {
  UsageMonitorUpdate update;
  update.set_bytes_tx(usage_in.bytes_tx);
  update.set_bytes_rx(usage_in.bytes_rx);
  update.set_level(level);
  update.set_monitoring_key(monitoring_key);
  return update;
}

SessionCreditUpdateCriteria* SessionState::get_credit_uc(
    const CreditKey& key, SessionStateUpdateCriteria& uc) {
  if (uc.charging_credit_map.find(key) == uc.charging_credit_map.end()) {
    uc.charging_credit_map[key] = credit_map_[key]->get_update_criteria();
  }
  return &(uc.charging_credit_map[key]);
}

bool SessionState::apply_update_criteria(SessionStateUpdateCriteria& uc) {
  SessionStateUpdateCriteria _;
  if (uc.is_fsm_updated) {
    curr_state_ = uc.updated_fsm_state;
  }

  if (uc.is_pending_event_triggers_updated) {
    for (auto it : uc.pending_event_triggers) {
      pending_event_triggers_[it.first] = it.second;
      if (it.first == REVALIDATION_TIMEOUT) {
        revalidation_time_ = uc.revalidation_time;
      }
    }
  }
  // QoS Management
  if (uc.is_bearer_mapping_updated) {
    bearer_id_by_policy_ = uc.bearer_id_by_policy;
  }

  // Config
  if (uc.is_config_updated) {
    config_ = uc.updated_config;
  }

  // Static rules
  for (const auto& rule_id : uc.static_rules_to_install) {
    if (is_static_rule_installed(rule_id)) {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because static rule already installed: " << rule_id
                   << std::endl;
      return false;
    }
    if (uc.new_rule_lifetimes.find(rule_id) != uc.new_rule_lifetimes.end()) {
      auto lifetime = uc.new_rule_lifetimes[rule_id];
      activate_static_rule(rule_id, lifetime, _);
    } else if (is_static_rule_scheduled(rule_id)) {
      install_scheduled_static_rule(rule_id, _);
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because rule lifetime is unspecified: " << rule_id
                   << std::endl;
      return false;
    }
  }
  for (const auto& rule_id : uc.static_rules_to_uninstall) {
    if (is_static_rule_installed(rule_id)) {
      deactivate_static_rule(rule_id, _);
    } else if (is_static_rule_scheduled(rule_id)) {
      install_scheduled_static_rule(rule_id, _);
      deactivate_static_rule(rule_id, _);
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because static rule already uninstalled: " << rule_id
                   << std::endl;
      return false;
    }
  }
  for (const auto& rule_id : uc.new_scheduled_static_rules) {
    if (is_static_rule_scheduled(rule_id)) {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because static rule already scheduled: " << rule_id
                   << std::endl;
      return false;
    }
    auto lifetime = uc.new_rule_lifetimes[rule_id];
    schedule_static_rule(rule_id, lifetime, _);
  }

  // Dynamic rules
  for (const auto& rule : uc.dynamic_rules_to_install) {
    if (is_dynamic_rule_installed(rule.id())) {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because dynamic rule already installed: " << rule.id()
                   << std::endl;
      return false;
    }
    if (uc.new_rule_lifetimes.find(rule.id()) != uc.new_rule_lifetimes.end()) {
      auto lifetime = uc.new_rule_lifetimes[rule.id()];
      insert_dynamic_rule(rule, lifetime, _);
    } else if (is_dynamic_rule_scheduled(rule.id())) {
      install_scheduled_dynamic_rule(rule.id(), _);
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because rule lifetime is unspecified: " << rule.id()
                   << std::endl;
      return false;
    }
  }
  for (const auto& rule_id : uc.dynamic_rules_to_uninstall) {
    if (is_dynamic_rule_installed(rule_id)) {
      dynamic_rules_.remove_rule(rule_id, NULL);
    } else if (is_dynamic_rule_scheduled(rule_id)) {
      install_scheduled_static_rule(rule_id, _);
      dynamic_rules_.remove_rule(rule_id, NULL);
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because dynamic rule already uninstalled: " << rule_id
                   << std::endl;
      return false;
    }
  }
  for (const auto& rule : uc.new_scheduled_dynamic_rules) {
    if (is_dynamic_rule_scheduled(rule.id())) {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because dynamic rule already scheduled: " << rule.id()
                   << std::endl;
      return false;
    }
    auto lifetime = uc.new_rule_lifetimes[rule.id()];
    schedule_dynamic_rule(rule, lifetime, _);
  }

  // Gy Dynamic rules
  for (const auto& rule : uc.gy_dynamic_rules_to_install) {
    if (is_gy_dynamic_rule_installed(rule.id())) {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because gy dynamic rule already installed: "
                   << rule.id() << std::endl;
      return false;
    }
    if (uc.new_rule_lifetimes.find(rule.id()) != uc.new_rule_lifetimes.end()) {
      auto lifetime = uc.new_rule_lifetimes[rule.id()];
      insert_gy_dynamic_rule(rule, lifetime, _);
      MLOG(MERROR) << "Merge: " << session_id_
                   << " gy dynamic rule " << rule.id() << std::endl;
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because gy dynamic rule lifetime is not found"
                   << std::endl;
      return false;
    }
  }
  for (const auto& rule_id : uc.gy_dynamic_rules_to_uninstall) {
    if (is_gy_dynamic_rule_installed(rule_id)) {
      gy_dynamic_rules_.remove_rule(rule_id, NULL);
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because gy dynamic rule already uninstalled: "
                   << rule_id << std::endl;
      return false;
    }
  }

  // Restrict rules
  for (const auto& rule_id : uc.restrict_rules_to_install) {
    if (is_restrict_rule_installed(rule_id)) {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because restrict rule already installed: " << rule_id
                   << std::endl;
      return false;
    }
    if (uc.new_rule_lifetimes.find(rule_id) != uc.new_rule_lifetimes.end()) {
      auto lifetime = uc.new_rule_lifetimes[rule_id];
      activate_restrict_rule(rule_id, lifetime, _);
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because rule lifetime is unspecified: " << rule_id
                   << std::endl;
      return false;
    }
  }
  for (const auto& rule_id : uc.restrict_rules_to_uninstall) {
    if (is_restrict_rule_installed(rule_id)) {
      deactivate_restrict_rule(rule_id, _);
    } else {
      MLOG(MERROR) << "Failed to merge: " << session_id_
                   << " because restrict rule already uninstalled: " << rule_id
                   << std::endl;
      return false;
    }
  }

  // Charging credit
  for (const auto& it : uc.charging_credit_map) {
    auto key           = it.first;
    auto credit_update = it.second;
    apply_charging_credit_update(key, credit_update);
  }
  for (const auto& it : uc.charging_credit_to_install) {
    auto key           = it.first;
    auto stored_credit = it.second;
    credit_map_[key] = std::make_unique<ChargingGrant>(stored_credit);
  }

  // Monitoring credit
  if (uc.is_session_level_key_updated) {
    set_session_level_key(uc.updated_session_level_key);
  }
  for (const auto& it : uc.monitor_credit_map) {
    auto key           = it.first;
    auto credit_update = it.second;
    apply_monitor_updates(key, credit_update);
  }
  for (const auto& it : uc.monitor_credit_to_install) {
    auto key            = it.first;
    auto stored_monitor = it.second;
    set_monitor(key, Monitor(stored_monitor), _);
    monitor_map_[key] = std::make_unique<Monitor>(stored_monitor);
  }

  if (uc.updated_pdp_end_time > 0) {
    pdp_end_time_ = uc.updated_pdp_end_time;
  }

  return true;
}

void SessionState::add_rule_usage(
    const std::string& rule_id, uint64_t used_tx, uint64_t used_rx,
    SessionStateUpdateCriteria& update_criteria) {
  CreditKey charging_key;
  if (dynamic_rules_.get_charging_key_for_rule_id(rule_id, &charging_key) ||
      static_rules_.get_charging_key_for_rule_id(rule_id, &charging_key)) {
    MLOG(MINFO) << "Updating used charging credit for Rule=" << rule_id
                << " Rating Group=" << charging_key.rating_group
                << " Service Identifier=" << charging_key.service_identifier;
    auto it = credit_map_.find(charging_key);
    if (it != credit_map_.end()) {
      auto credit_uc = get_credit_uc(charging_key, update_criteria);
      it->second->credit.add_used_credit(used_tx, used_rx, *credit_uc);
      if (it->second->should_deactivate_service()) {
        it->second->set_service_state(SERVICE_NEEDS_DEACTIVATION, *credit_uc);
      }
    } else {
      MLOG(MDEBUG) << "Rating Group " << charging_key.rating_group
                   << " not found, not adding the usage";
    }
  }
  std::string monitoring_key;
  if (dynamic_rules_.get_monitoring_key_for_rule_id(rule_id, &monitoring_key) ||
      static_rules_.get_monitoring_key_for_rule_id(rule_id, &monitoring_key)) {
    MLOG(MINFO) << "Updating used monitoring credit for Rule=" << rule_id
                << " Monitoring Key=" << monitoring_key;
    add_to_monitor(monitoring_key, used_tx, used_rx, update_criteria);
  }
  if (session_level_key_ != "" && monitoring_key != session_level_key_) {
    // Update session level key if its different
    add_to_monitor(session_level_key_, used_tx, used_rx, update_criteria);
  }
  if (is_dynamic_rule_installed(rule_id) || is_static_rule_installed(rule_id)) {
    update_data_usage_metrics(used_tx, used_rx);
  }
}

void SessionState::apply_session_rule_set(
    RuleSetToApply& rule_set, RulesToProcess& rules_to_activate,
    RulesToProcess& rules_to_deactivate, SessionStateUpdateCriteria& uc) {
  apply_session_static_rule_set(
      rule_set.static_rules, rules_to_activate, rules_to_deactivate, uc);
  apply_session_dynamic_rule_set(
      rule_set.dynamic_rules, rules_to_activate, rules_to_deactivate, uc);
}

void SessionState::apply_session_static_rule_set(
    std::unordered_set<std::string> static_rules,
    RulesToProcess& rules_to_activate, RulesToProcess& rules_to_deactivate,
    SessionStateUpdateCriteria& uc) {
  // No activation time / deactivation support yet for rule set interface
  RuleLifetime lifetime{
      .activation_time   = 0,
      .deactivation_time = 0,
  };
  // Go through the rule set and install any rules not yet installed
  for (const auto& static_rule_id : static_rules) {
    if (!is_static_rule_installed(static_rule_id)) {
      MLOG(MINFO) << "Installing static rule " << static_rule_id << " for "
                  << session_id_;
      activate_static_rule(static_rule_id, lifetime, uc);
      rules_to_activate.static_rules.push_back(static_rule_id);
    }
  }
  std::vector<std::string> static_rules_to_deactivate;

  // Go through the existing rules and uninstall any rule not in the rule set
  for (const auto static_rule_id : active_static_rules_) {
    if (static_rules.find(static_rule_id) == static_rules.end()) {
      rules_to_deactivate.static_rules.push_back(static_rule_id);
    }
  }
  // Do the actual removal separately so we're not modifying the vector while
  // looping
  for (const auto static_rule_id : rules_to_deactivate.static_rules) {
    MLOG(MINFO) << "Removing static rule " << static_rule_id << " for "
                << session_id_;
    deactivate_static_rule(static_rule_id, uc);
  }
}

void SessionState::apply_session_dynamic_rule_set(
    std::unordered_map<std::string, PolicyRule> dynamic_rules,
    RulesToProcess& rules_to_activate, RulesToProcess& rules_to_deactivate,
    SessionStateUpdateCriteria& uc) {
  // No activation time / deactivation support yet for rule set interface
  RuleLifetime lifetime{
      .activation_time   = 0,
      .deactivation_time = 0,
  };
  for (const auto& dynamic_rule_pair : dynamic_rules) {
    if (!is_dynamic_rule_installed(dynamic_rule_pair.first)) {
      MLOG(MINFO) << "installing dynamic rule " << dynamic_rule_pair.first
                  << " for " << session_id_;
      insert_dynamic_rule(dynamic_rule_pair.second, lifetime, uc);
      rules_to_activate.dynamic_rules.push_back(dynamic_rule_pair.second);
    }
  }
  std::vector<PolicyRule> active_dynamic_rules;
  dynamic_rules_.get_rules(active_dynamic_rules);
  for (const auto& dynamic_rule : active_dynamic_rules) {
    if (dynamic_rules.find(dynamic_rule.id()) == dynamic_rules.end()) {
      MLOG(MINFO) << "Removing dynamic rule " << dynamic_rule.id() << " for "
                  << session_id_;
      remove_dynamic_rule(dynamic_rule.id(), nullptr, uc);
      rules_to_deactivate.dynamic_rules.push_back(dynamic_rule);
    }
  }
}

void SessionState::set_subscriber_quota_state(
    const magma::lte::SubscriberQuotaUpdate_Type state,
    SessionStateUpdateCriteria& update_criteria) {
  update_criteria.updated_subscriber_quota_state = state;
  subscriber_quota_state_                        = state;
}

bool SessionState::active_monitored_rules_exist() {
  return total_monitored_rules_count() > 0;
}

SessionFsmState SessionState::get_state() {
  return curr_state_;
}

bool SessionState::is_terminating() {
  if (curr_state_ == SESSION_RELEASED || curr_state_ == SESSION_TERMINATED) {
    return true;
  }
  return false;
}

void SessionState::get_monitor_updates(
    UpdateSessionRequest& update_request_out,
    std::vector<std::unique_ptr<ServiceAction>>* actions_out,
    SessionStateUpdateCriteria& update_criteria) {
  for (auto& monitor_pair : monitor_map_) {
    auto mkey    = monitor_pair.first;
    auto& credit = monitor_pair.second->credit;

    bool is_partially_exhausted = credit.is_quota_exhausted(
        SessionCredit::USAGE_REPORTING_THRESHOLD);
    bool is_totally_exhausted = credit.is_quota_exhausted(1);

    if (!is_partially_exhausted ||
        (!is_totally_exhausted && credit.current_grant_contains_zero())){
      // The update will be skipped in case we havent used enough data yet
      // OR in the case the monitor got a 0 grant and it is not exhausted
      // (we will only send the last update when it is totally exhausted)
      continue;
    }
    MLOG(MDEBUG) << "Session " << session_id_ << " monitoring key " << mkey
                 << " updating due to quota exhaustion"
                 << " with request number " << request_number_;
    auto credit_uc = get_monitor_uc(mkey, update_criteria);
    auto usage     = credit.get_usage_for_reporting(*credit_uc);
    auto update =
        make_usage_monitor_update(usage, mkey, monitor_pair.second->level);
    auto new_req = update_request_out.mutable_usage_monitors()->Add();

    add_common_fields_to_usage_monitor_update(new_req);
    new_req->mutable_update()->CopyFrom(update);
    new_req->set_event_trigger(USAGE_REPORT);
    request_number_++;
    update_criteria.request_number_increment++;
  }
}

void SessionState::add_common_fields_to_usage_monitor_update(
    UsageMonitoringUpdateRequest* req) {
  req->set_session_id(session_id_);
  req->set_request_number(request_number_);
  req->set_sid(imsi_);
  req->set_ue_ipv4(config_.common_context.ue_ipv4());
  req->set_rat_type(config_.common_context.rat_type());
  fill_protos_tgpp_context(req->mutable_tgpp_ctx());
  if (config_.rat_specific_context.has_wlan_context()) {
    const auto& wlan_context = config_.rat_specific_context.wlan_context();
    req->set_hardware_addr(wlan_context.mac_addr_binary());
  }
}

void SessionState::get_updates(
    UpdateSessionRequest& update_request_out,
    std::vector<std::unique_ptr<ServiceAction>>* actions_out,
    SessionStateUpdateCriteria& update_criteria) {
  if (curr_state_ != SESSION_ACTIVE) return;
  get_charging_updates(update_request_out, actions_out, update_criteria);
  get_monitor_updates(update_request_out, actions_out, update_criteria);
  get_event_trigger_updates(update_request_out, actions_out, update_criteria);
}

void SessionState::mark_as_awaiting_termination(
    SessionStateUpdateCriteria& update_criteria) {
  set_fsm_state(SESSION_TERMINATION_SCHEDULED, update_criteria);
}

SubscriberQuotaUpdate_Type SessionState::get_subscriber_quota_state() const {
  return subscriber_quota_state_;
}

void SessionState::complete_termination(
    SessionReporter& reporter, SessionStateUpdateCriteria& update_criteria) {
  switch (curr_state_) {
    case SESSION_ACTIVE:
      MLOG(MERROR) << session_id_ << " Encountered unexpected state 'ACTIVE' when "
                   << "forcefully completing termination. Not terminating...";
      return;
    case SESSION_TERMINATED:
      // session is already terminated. Do nothing.
      return;
    case SESSION_RELEASED:
      MLOG(MINFO) << session_id_ << " Forcefully terminating session since it did "
                  << "not receive usage from pipelined in time.";
    default:  // Continue termination but no logs are necessary for other states
      break;
  }
  // mark entire session as terminated
  set_fsm_state(SESSION_TERMINATED, update_criteria);
  auto termination_req = make_termination_request(update_criteria);
  auto logging_cb = SessionReporter::get_terminate_logging_cb(termination_req);
  reporter.report_terminate_session(termination_req, logging_cb);
}

SessionTerminateRequest SessionState::make_termination_request(
    SessionStateUpdateCriteria& update_criteria) {
  SessionTerminateRequest req;
  req.set_sid(imsi_);
  req.set_session_id(session_id_);
  req.set_request_number(request_number_);
  req.set_ue_ipv4(config_.common_context.ue_ipv4());
  req.set_msisdn(config_.common_context.msisdn());
  req.set_apn(config_.common_context.apn());
  req.set_rat_type(config_.common_context.rat_type());
  fill_protos_tgpp_context(req.mutable_tgpp_ctx());
  if (config_.rat_specific_context.has_lte_context()) {
    const auto& lte_context = config_.rat_specific_context.lte_context();
    req.set_spgw_ipv4(lte_context.spgw_ipv4());
    req.set_imei(lte_context.imei());
    req.set_plmn_id(lte_context.plmn_id());
    req.set_imsi_plmn_id(lte_context.imsi_plmn_id());
    req.set_user_location(lte_context.user_location());
  } else if (config_.rat_specific_context.has_wlan_context()) {
    const auto& wlan_context = config_.rat_specific_context.wlan_context();
    req.set_hardware_addr(wlan_context.mac_addr_binary());
  }

  // gx monitors
  for (auto& credit_pair : monitor_map_) {
    auto credit_uc = get_monitor_uc(credit_pair.first, update_criteria);
    req.mutable_monitor_usages()->Add()->CopyFrom(make_usage_monitor_update(
        credit_pair.second->credit.get_all_unreported_usage_for_reporting(
            *credit_uc),
        credit_pair.first, credit_pair.second->level));
  }
  // gy credits
  for (auto& credit_pair : credit_map_) {
    auto credit_uc    = get_credit_uc(credit_pair.first, update_criteria);
    auto credit_usage = credit_pair.second->get_credit_usage(
        CreditUsage::TERMINATED, *credit_uc, true);
    credit_pair.first.set_credit_usage(&credit_usage);
    req.mutable_credit_usages()->Add()->CopyFrom(credit_usage);
  }
  return req;
}

SessionState::TotalCreditUsage SessionState::get_total_credit_usage() {
  // Collate unique charging/monitoring keys used by rules
  std::unordered_set<CreditKey, decltype(&ccHash), decltype(&ccEqual)>
      used_charging_keys(4, ccHash, ccEqual);
  std::unordered_set<std::string> used_monitoring_keys;

  std::vector<std::reference_wrapper<PolicyRuleBiMap>> bimaps{static_rules_,
                                                              dynamic_rules_};

  for (auto bimap : bimaps) {
    PolicyRuleBiMap& rules = bimap;
    std::vector<std::string> rule_ids{};
    std::vector<std::string>& rule_ids_ptr = rule_ids;
    rules.get_rule_ids(rule_ids_ptr);

    for (auto rule_id : rule_ids) {
      CreditKey charging_key;
      bool should_track_charging_key =
          rules.get_charging_key_for_rule_id(rule_id, &charging_key);
      std::string monitoring_key;
      bool should_track_monitoring_key =
          rules.get_monitoring_key_for_rule_id(rule_id, &monitoring_key);

      if (should_track_charging_key) used_charging_keys.insert(charging_key);
      if (should_track_monitoring_key)
        used_monitoring_keys.insert(monitoring_key);
    }
  }

  // Sum up usage
  TotalCreditUsage usage{
      .monitoring_tx = 0,
      .monitoring_rx = 0,
      .charging_tx   = 0,
      .charging_rx   = 0,
  };
  for (auto monitoring_key : used_monitoring_keys) {
    usage.monitoring_tx += get_monitor(monitoring_key, USED_TX);
    usage.monitoring_rx += get_monitor(monitoring_key, USED_RX);
  }
  for (auto charging_key : used_charging_keys) {
    auto it = credit_map_.find(charging_key);
    if (it != credit_map_.end()) {
      usage.charging_tx += it->second->credit.get_credit(USED_TX);
      usage.charging_rx += it->second->credit.get_credit(USED_RX);
    }
  }
  return usage;
}

std::string SessionState::get_session_id() const {
  return session_id_;
}

SessionConfig SessionState::get_config() const {
  return config_;
}

void SessionState::set_config(const SessionConfig& config) {
  config_ = config;
}

bool SessionState::is_radius_cwf_session() const {
  return (config_.common_context.rat_type() == RATType::TGPP_WLAN);
}

void SessionState::get_session_info(SessionState::SessionInfo& info) {
  info.imsi    = imsi_;
  info.ip_addr = config_.common_context.ue_ipv4();
  get_dynamic_rules().get_rules(info.dynamic_rules);
  get_gy_dynamic_rules().get_rules(info.gy_dynamic_rules);
  info.static_rules = active_static_rules_;
  info.restrict_rules = active_restrict_rules_;
  info.ambr = config_.get_apn_ambr();
}

void SessionState::set_tgpp_context(
    const magma::lte::TgppContext& tgpp_context,
    SessionStateUpdateCriteria& update_criteria) {
  update_criteria.updated_tgpp_context = tgpp_context;
  tgpp_context_                        = tgpp_context;
}

void SessionState::fill_protos_tgpp_context(
    magma::lte::TgppContext* tgpp_context) const {
  *tgpp_context = tgpp_context_;
}

uint32_t SessionState::get_request_number() {
  return request_number_;
}

uint64_t SessionState::get_pdp_start_time() {
  return pdp_start_time_;
}

uint64_t SessionState::get_pdp_end_time() {
  return pdp_end_time_;
}

void SessionState::set_pdp_end_time(uint64_t epoch) {
  pdp_end_time_ = epoch;
}

void SessionState::increment_request_number(uint32_t incr) {
  request_number_ += incr;
}

bool SessionState::is_dynamic_rule_scheduled(const std::string& rule_id) {
  return scheduled_dynamic_rules_.get_rule(rule_id, NULL);
}

bool SessionState::is_static_rule_scheduled(const std::string& rule_id) {
  return scheduled_static_rules_.count(rule_id) == 1;
}

bool SessionState::is_dynamic_rule_installed(const std::string& rule_id) {
  return dynamic_rules_.get_rule(rule_id, NULL);
}

bool SessionState::is_gy_dynamic_rule_installed(const std::string& rule_id) {
  return gy_dynamic_rules_.get_rule(rule_id, NULL);
}

bool SessionState::is_static_rule_installed(const std::string& rule_id) {
  return std::find(
             active_static_rules_.begin(), active_static_rules_.end(),
             rule_id) != active_static_rules_.end();
}

bool SessionState::is_restrict_rule_installed(const std::string& rule_id) {
  return std::find(
             active_restrict_rules_.begin(), active_restrict_rules_.end(),
             rule_id) != active_restrict_rules_.end();
}

void SessionState::insert_dynamic_rule(
    const PolicyRule& rule, RuleLifetime& lifetime,
    SessionStateUpdateCriteria& update_criteria) {
  if (is_dynamic_rule_installed(rule.id())) {
    return;
  }
  rule_lifetimes_[rule.id()] = lifetime;
  dynamic_rules_.insert_rule(rule);
  update_criteria.dynamic_rules_to_install.push_back(rule);
  update_criteria.new_rule_lifetimes[rule.id()] = lifetime;
}

void SessionState::insert_gy_dynamic_rule(
    const PolicyRule& rule, RuleLifetime& lifetime,
    SessionStateUpdateCriteria& update_criteria) {
  if (is_gy_dynamic_rule_installed(rule.id())) {
    MLOG(MDEBUG) << "Tried to insert " << rule.id()
                 << " (gy dynamic rule), but it already existed";
    return;
  }
  rule_lifetimes_[rule.id()] = lifetime;
  gy_dynamic_rules_.insert_rule(rule);
  update_criteria.gy_dynamic_rules_to_install.push_back(rule);
  update_criteria.new_rule_lifetimes[rule.id()] = lifetime;
}

void SessionState::activate_static_rule(
    const std::string& rule_id, RuleLifetime& lifetime,
    SessionStateUpdateCriteria& update_criteria) {
  rule_lifetimes_[rule_id] = lifetime;
  active_static_rules_.push_back(rule_id);
  update_criteria.static_rules_to_install.insert(rule_id);
  update_criteria.new_rule_lifetimes[rule_id] = lifetime;
}

void SessionState::activate_restrict_rule(
    const std::string& rule_id, RuleLifetime& lifetime,
    SessionStateUpdateCriteria& update_criteria) {
  rule_lifetimes_[rule_id] = lifetime;
  active_restrict_rules_.push_back(rule_id);
  update_criteria.restrict_rules_to_install.insert(rule_id);
  update_criteria.new_rule_lifetimes[rule_id] = lifetime;
}

bool SessionState::remove_dynamic_rule(
    const std::string& rule_id, PolicyRule* rule_out,
    SessionStateUpdateCriteria& update_criteria) {
  bool removed = dynamic_rules_.remove_rule(rule_id, rule_out);
  if (removed) {
    update_criteria.dynamic_rules_to_uninstall.insert(rule_id);
  }
  return removed;
}

bool SessionState::remove_scheduled_dynamic_rule(
    const std::string& rule_id, PolicyRule* rule_out,
    SessionStateUpdateCriteria& update_criteria) {
  bool removed = scheduled_dynamic_rules_.remove_rule(rule_id, rule_out);
  if (removed) {
    update_criteria.dynamic_rules_to_uninstall.insert(rule_id);
  }
  return removed;
}

bool SessionState::remove_gy_dynamic_rule(
    const std::string& rule_id, PolicyRule* rule_out,
    SessionStateUpdateCriteria& update_criteria) {
  bool removed = gy_dynamic_rules_.remove_rule(rule_id, rule_out);
  if (removed) {
    update_criteria.gy_dynamic_rules_to_uninstall.insert(rule_id);
  }
  return removed;
}

bool SessionState::deactivate_static_rule(
    const std::string& rule_id, SessionStateUpdateCriteria& update_criteria) {
  auto it = std::find(
      active_static_rules_.begin(), active_static_rules_.end(), rule_id);
  if (it == active_static_rules_.end()) {
    return false;
  }
  update_criteria.static_rules_to_uninstall.insert(rule_id);
  active_static_rules_.erase(it);
  return true;
}

bool SessionState::deactivate_scheduled_static_rule(
    const std::string& rule_id, SessionStateUpdateCriteria& update_criteria) {
  if (scheduled_static_rules_.count(rule_id) == 0) {
    return false;
  }
  scheduled_static_rules_.erase(rule_id);
  return true;
}

bool SessionState::deactivate_restrict_rule(
    const std::string& rule_id, SessionStateUpdateCriteria& update_criteria) {
  auto it = std::find(
      active_restrict_rules_.begin(), active_restrict_rules_.end(), rule_id);
  if (it == active_restrict_rules_.end()) {
    return false;
  }
  update_criteria.restrict_rules_to_uninstall.insert(rule_id);
  active_restrict_rules_.erase(it);
  return true;
}

void SessionState::sync_rules_to_time(
    std::time_t current_time, SessionStateUpdateCriteria& update_criteria) {
  // Update active static rules
  for (const std::string& rule_id : active_static_rules_) {
    if (should_rule_be_deactivated(rule_id, current_time)) {
      deactivate_static_rule(rule_id, update_criteria);
    }
  }
  // Update scheduled static rules
  std::set<std::string> scheduled_rule_ids = scheduled_static_rules_;
  for (const std::string& rule_id : scheduled_rule_ids) {
    if (should_rule_be_active(rule_id, current_time)) {
      install_scheduled_static_rule(rule_id, update_criteria);
    } else if (should_rule_be_deactivated(rule_id, current_time)) {
      scheduled_static_rules_.erase(rule_id);
      update_criteria.static_rules_to_uninstall.insert(rule_id);
    }
  }
  // Update active dynamic rules
  std::vector<std::string> dynamic_rule_ids;
  dynamic_rules_.get_rule_ids(dynamic_rule_ids);
  for (const std::string& rule_id : dynamic_rule_ids) {
    if (should_rule_be_deactivated(rule_id, current_time)) {
      remove_dynamic_rule(rule_id, NULL, update_criteria);
    }
  }
  // Update scheduled dynamic rules
  dynamic_rule_ids.clear();
  scheduled_dynamic_rules_.get_rule_ids(dynamic_rule_ids);
  for (const std::string& rule_id : dynamic_rule_ids) {
    if (should_rule_be_active(rule_id, current_time)) {
      install_scheduled_dynamic_rule(rule_id, update_criteria);
    } else if (should_rule_be_deactivated(rule_id, current_time)) {
      remove_scheduled_dynamic_rule(rule_id, NULL, update_criteria);
    }
  }
}

std::vector<std::string>& SessionState::get_static_rules() {
  return active_static_rules_;
}

std::set<std::string>& SessionState::get_scheduled_static_rules() {
  return scheduled_static_rules_;
}

std::vector<std::string>& SessionState::get_restrict_rules() {
  return active_restrict_rules_;
}

DynamicRuleStore& SessionState::get_dynamic_rules() {
  return dynamic_rules_;
}

DynamicRuleStore& SessionState::get_scheduled_dynamic_rules() {
  return scheduled_dynamic_rules_;
}

RuleLifetime& SessionState::get_rule_lifetime(const std::string& rule_id) {
  return rule_lifetimes_[rule_id];
}

DynamicRuleStore& SessionState::get_gy_dynamic_rules() {
  return gy_dynamic_rules_;
}

uint32_t SessionState::total_monitored_rules_count() {
  uint32_t monitored_dynamic_rules = dynamic_rules_.monitored_rules_count();
  uint32_t monitored_static_rules  = 0;
  for (auto& rule_id : active_static_rules_) {
    std::string _;
    auto is_monitored =
        static_rules_.get_monitoring_key_for_rule_id(rule_id, &_);
    if (is_monitored) {
      monitored_static_rules++;
    }
  }
  return monitored_dynamic_rules + monitored_static_rules;
}

void SessionState::schedule_dynamic_rule(
    const PolicyRule& rule, RuleLifetime& lifetime,
    SessionStateUpdateCriteria& update_criteria) {
  update_criteria.new_rule_lifetimes[rule.id()] = lifetime;
  update_criteria.new_scheduled_dynamic_rules.push_back(rule);
  rule_lifetimes_[rule.id()] = lifetime;
  scheduled_dynamic_rules_.insert_rule(rule);
}

void SessionState::schedule_static_rule(
    const std::string& rule_id, RuleLifetime& lifetime,
    SessionStateUpdateCriteria& update_criteria) {
  update_criteria.new_rule_lifetimes[rule_id] = lifetime;
  update_criteria.new_scheduled_static_rules.insert(rule_id);
  rule_lifetimes_[rule_id] = lifetime;
  scheduled_static_rules_.insert(rule_id);
}

void SessionState::install_scheduled_dynamic_rule(
    const std::string& rule_id, SessionStateUpdateCriteria& update_criteria) {
  PolicyRule dynamic_rule;
  bool removed = scheduled_dynamic_rules_.remove_rule(rule_id, &dynamic_rule);
  if (!removed) {
    MLOG(MERROR) << "Failed to mark a scheduled dynamic rule as installed "
                 << "with rule_id: " << rule_id;
    return;
  }
  update_criteria.dynamic_rules_to_install.push_back(dynamic_rule);
  dynamic_rules_.insert_rule(dynamic_rule);
}

void SessionState::install_scheduled_static_rule(
    const std::string& rule_id, SessionStateUpdateCriteria& update_criteria) {
  auto it = scheduled_static_rules_.find(rule_id);
  if (it == scheduled_static_rules_.end()) {
    MLOG(MERROR) << "Failed to mark a scheduled static rule as installed "
                    "with rule_id: "
                 << rule_id;
  }
  update_criteria.static_rules_to_install.insert(rule_id);
  scheduled_static_rules_.erase(rule_id);
  active_static_rules_.push_back(rule_id);
}

uint32_t SessionState::get_credit_key_count() {
  return credit_map_.size() + monitor_map_.size();
}

bool SessionState::is_active() {
  return curr_state_ == SESSION_ACTIVE;
}

void SessionState::set_fsm_state(
    SessionFsmState new_state, SessionStateUpdateCriteria& uc) {
  // Only log and reflect change into update criteria if the state is new
  if (curr_state_ != new_state) {
    MLOG(MDEBUG) << "Session " << session_id_ << " FSM state change from "
                 << session_fsm_state_to_str(curr_state_) << " to "
                 << session_fsm_state_to_str(new_state);
    curr_state_          = new_state;
    uc.is_fsm_updated    = true;
    uc.updated_fsm_state = new_state;
  }
}

bool SessionState::should_rule_be_active(
    const std::string& rule_id, std::time_t time) {
  auto lifetime = rule_lifetimes_[rule_id];
  bool deactivated =
      (lifetime.deactivation_time > 0) && (lifetime.deactivation_time < time);
  return lifetime.activation_time < time && !deactivated;
}

bool SessionState::should_rule_be_deactivated(
    const std::string& rule_id, std::time_t time) {
  auto lifetime = rule_lifetimes_[rule_id];
  return lifetime.deactivation_time > 0 && lifetime.deactivation_time < time;
}

StaticRuleInstall SessionState::get_static_rule_install(
    const std::string& rule_id, const RuleLifetime& lifetime) {
  StaticRuleInstall rule_install{};
  rule_install.set_rule_id(rule_id);
  rule_install.mutable_activation_time()->set_seconds(lifetime.activation_time);
  rule_install.mutable_deactivation_time()->set_seconds(
      lifetime.deactivation_time);
  return rule_install;
}

DynamicRuleInstall SessionState::get_dynamic_rule_install(
    const std::string& rule_id, const RuleLifetime& lifetime) {
  DynamicRuleInstall rule_install{};
  PolicyRule* policy_rule = rule_install.mutable_policy_rule();
  if (!dynamic_rules_.get_rule(rule_id, policy_rule)) {
    scheduled_dynamic_rules_.get_rule(rule_id, policy_rule);
  }
  rule_install.mutable_activation_time()->set_seconds(lifetime.activation_time);
  rule_install.mutable_deactivation_time()->set_seconds(
      lifetime.deactivation_time);
  return rule_install;
}

// Charging Credits
static FinalActionInfo get_final_action_info(
    const magma::lte::ChargingCredit& credit) {
  FinalActionInfo final_action_info;
  if (credit.is_final()) {
    final_action_info.final_action = credit.final_action();
    if (credit.final_action() == ChargingCredit_FinalAction_REDIRECT) {
      final_action_info.redirect_server = credit.redirect_server();
    }
    else if (credit.final_action() == ChargingCredit_FinalAction_RESTRICT_ACCESS) {
      for (auto rule : credit.restrict_rules()) {
        final_action_info.restrict_rules.push_back(rule);
      }
    }
  }
  return final_action_info;
}

bool SessionState::reset_reporting_charging_credit(
    const CreditKey& key, SessionStateUpdateCriteria& update_criteria) {
  auto it = credit_map_.find(key);
  if (it == credit_map_.end()) {
    MLOG(MERROR) << "Could not reset credit for IMSI" << imsi_
                 << " and charging key " << key << " because it wasn't found";
    return false;
  }
  auto credit_uc = get_credit_uc(key, update_criteria);
  it->second->credit.reset_reporting_credit(credit_uc);
  return true;
}

bool SessionState::receive_charging_credit(
    const CreditUpdateResponse& update,
    SessionStateUpdateCriteria& update_criteria) {
  auto it = credit_map_.find(CreditKey(update));
  if (it == credit_map_.end()) {
    // new credit
    return init_charging_credit(update, update_criteria);
  }
  auto& grant    = it->second;
  auto credit_uc = get_credit_uc(CreditKey(update), update_criteria);
  if (!update.success()) {
    // update unsuccessful, reset credit and return
    MLOG(MDEBUG) << session_id_ << " Received an unsuccessful update for RG "
                 << update.charging_key();
    grant->credit.mark_failure(update.result_code(), credit_uc);
    if (grant->should_deactivate_service()) {
      grant->set_service_state(SERVICE_NEEDS_DEACTIVATION, *credit_uc);
    }
    return false;
  }
  MLOG(MINFO) << session_id_ << " Received a charging credit for RG: "
              << update.charging_key();
  grant->receive_charging_grant(update.credit(), credit_uc);

  if (grant->reauth_state == REAUTH_PROCESSING) {
    grant->set_reauth_state(REAUTH_NOT_NEEDED, *credit_uc);
  }
  if (!grant->credit.is_quota_exhausted(1) &&
      grant->service_state != SERVICE_ENABLED) {
    // if quota no longer exhausted, reenable services as needed
    MLOG(MINFO) << "Quota available. Activating service";
    grant->set_service_state(SERVICE_NEEDS_ACTIVATION, *credit_uc);
  }
  return contains_credit(update.credit().granted_units()) ||
         is_infinite_credit(update);
}

bool SessionState::init_charging_credit(
    const CreditUpdateResponse& update,
    SessionStateUpdateCriteria& update_criteria) {
  if (!update.success()) {
    // init failed, don't track key
    MLOG(MERROR) << "Credit init failed for imsi " << imsi_
                 << " and charging key " << update.charging_key();
    return false;
  }
  MLOG(MINFO) << session_id_ << " Initialized a charging credit for RG: "
              << update.charging_key();

  auto charging_grant    = std::make_unique<ChargingGrant>();
  charging_grant->credit = SessionCredit(SERVICE_ENABLED, update.limit_type());

  charging_grant->receive_charging_grant(update.credit());
  update_criteria.charging_credit_to_install[CreditKey(update)] =
      charging_grant->marshal();
  credit_map_[CreditKey(update)] = std::move(charging_grant);
  return contains_credit(update.credit().granted_units()) ||
      is_infinite_credit(update);
}

bool SessionState::contains_credit(const GrantedUnits& gsu) {
  return (gsu.total().is_valid() && gsu.total().volume() > 0) ||
         (gsu.tx().is_valid() && gsu.tx().volume() > 0) ||
         (gsu.rx().is_valid() && gsu.rx().volume() > 0);
}

bool SessionState::is_infinite_credit(const CreditUpdateResponse& response) {
  return (response.limit_type() == INFINITE_UNMETERED) ||
         (response.limit_type() == INFINITE_METERED);
}

uint64_t SessionState::get_charging_credit(
    const CreditKey& key, Bucket bucket) const {
  auto it = credit_map_.find(key);
  if (it == credit_map_.end()) {
    return 0;
  }
  return it->second->credit.get_credit(bucket);
}

ReAuthResult SessionState::reauth_key(
    const CreditKey& charging_key,
    SessionStateUpdateCriteria& update_criteria) {
  auto it = credit_map_.find(charging_key);
  if (it != credit_map_.end()) {
    // if credit is already reporting, don't initiate update
    auto& grant = it->second;
    if (grant->credit.is_reporting()) {
      return ReAuthResult::UPDATE_NOT_NEEDED;
    }
    auto uc = grant->get_update_criteria();
    grant->set_reauth_state(REAUTH_REQUIRED, uc);
    update_criteria.charging_credit_map[charging_key] = uc;
    return ReAuthResult::UPDATE_INITIATED;
  }
  // charging_key cannot be found, initialize credit and engage reauth
  auto grant           = std::make_unique<ChargingGrant>();
  grant->credit        = SessionCredit(SERVICE_DISABLED);
  grant->reauth_state  = REAUTH_REQUIRED;
  grant->service_state = SERVICE_DISABLED;
  update_criteria.charging_credit_to_install[charging_key] = grant->marshal();
  credit_map_[charging_key]                                = std::move(grant);
  return ReAuthResult::UPDATE_INITIATED;
}

ReAuthResult SessionState::reauth_all(
    SessionStateUpdateCriteria& update_criteria) {
  auto res = ReAuthResult::UPDATE_NOT_NEEDED;
  for (auto& credit_pair : credit_map_) {
    auto key    = credit_pair.first;
    auto& grant = credit_pair.second;
    // Only update credits that aren't reporting
    if (!grant->credit.is_reporting()) {
      update_criteria.charging_credit_map[key] = grant->get_update_criteria();
      grant->set_reauth_state(
          REAUTH_REQUIRED, update_criteria.charging_credit_map[key]);
      res = ReAuthResult::UPDATE_INITIATED;
    }
  }
  return res;
}

void SessionState::apply_charging_credit_update(
    const CreditKey& key, SessionCreditUpdateCriteria& credit_update) {
  auto it = credit_map_.find(key);
  if (it == credit_map_.end()) {
    return;
  }
  auto& charging_grant = it->second;
  auto& credit         = charging_grant->credit;
  if (credit_update.deleted) {
    credit_map_.erase(key);
    return;
  }

  // Credit merging
  credit.set_grant_tracking_type(
      credit_update.grant_tracking_type, credit_update);
  credit.set_received_granted_units(credit_update.received_granted_units,
                                    credit_update);
  for (int i = USED_TX; i != MAX_VALUES; i++) {
    Bucket bucket = static_cast<Bucket>(i);
    credit.add_credit(
        credit_update.bucket_deltas.find(bucket)->second, bucket,
        credit_update);
  }

  // set charging grant
  charging_grant->is_final_grant    = credit_update.is_final;
  charging_grant->final_action_info = credit_update.final_action_info;
  charging_grant->expiry_time       = credit_update.expiry_time;
  charging_grant->reauth_state      = credit_update.reauth_state;
  charging_grant->service_state     = credit_update.service_state;

}

void SessionState::set_charging_credit(
    const CreditKey& key, ChargingGrant charging_grant,
    SessionStateUpdateCriteria& uc) {
  credit_map_[key] = std::make_unique<ChargingGrant>(charging_grant);
  uc.charging_credit_to_install[key] = credit_map_[key]->marshal();
}

CreditUsageUpdate SessionState::make_credit_usage_update_req(
    CreditUsage& usage) const {
  CreditUsageUpdate req;
  req.set_session_id(session_id_);
  req.set_request_number(request_number_);
  req.set_sid(imsi_);
  req.set_msisdn(config_.common_context.msisdn());
  req.set_ue_ipv4(config_.common_context.ue_ipv4());
  req.set_apn(config_.common_context.apn());
  req.set_rat_type(config_.common_context.rat_type());
  fill_protos_tgpp_context(req.mutable_tgpp_ctx());
  if (config_.rat_specific_context.has_lte_context()) {
    const auto& lte_context = config_.rat_specific_context.lte_context();
    req.set_spgw_ipv4(lte_context.spgw_ipv4());
    req.set_imei(lte_context.imei());
    req.set_plmn_id(lte_context.plmn_id());
    req.set_imsi_plmn_id(lte_context.imsi_plmn_id());
    req.set_user_location(lte_context.user_location());
  } else if (config_.rat_specific_context.has_wlan_context()) {
    const auto& wlan_context = config_.rat_specific_context.wlan_context();
    req.set_hardware_addr(wlan_context.mac_addr_binary());
  }
  req.mutable_usage()->CopyFrom(usage);
  return req;
}

void SessionState::get_charging_updates(
    UpdateSessionRequest& update_request_out,
    std::vector<std::unique_ptr<ServiceAction>>* actions_out,
    SessionStateUpdateCriteria& uc) {
  for (auto& credit_pair : credit_map_) {
    auto& key      = credit_pair.first;
    auto& grant    = credit_pair.second;
    auto credit_uc = get_credit_uc(key, uc);

    auto action_type = grant->get_action(*credit_uc);
    auto action      = std::make_unique<ServiceAction>(action_type);
    switch (action_type) {
      case CONTINUE_SERVICE: {
        CreditUsage::UpdateType update_type;
        if (!grant->get_update_type(&update_type)) {
          break;  // no update
        }
        // Create Update struct
        MLOG(MDEBUG) << "Subscriber " << imsi_ << " rating group " << key
                     << " updating due to type "
                     << credit_update_type_to_str(update_type)
                     << " with request number " << request_number_;

        if (update_type == CreditUsage::REAUTH_REQUIRED) {
          grant->set_reauth_state(REAUTH_PROCESSING, *credit_uc);
        }
        auto update = grant->get_credit_usage(update_type, *credit_uc, false);
        key.set_credit_usage(&update);
        auto credit_req = make_credit_usage_update_req(update);
        update_request_out.mutable_updates()->Add()->CopyFrom(credit_req);
        request_number_++;
        uc.request_number_increment++;
      } break;
      case REDIRECT:
        if (grant->service_state == SERVICE_REDIRECTED) {
          MLOG(MDEBUG) << "Redirection already activated.";
          continue;
        }
        grant->set_service_state(SERVICE_REDIRECTED, *credit_uc);
        action->set_redirect_server(grant->final_action_info.redirect_server);
      case RESTRICT_ACCESS: {
        if (grant->service_state == SERVICE_RESTRICTED) {
          MLOG(MDEBUG) << "Service Restriction is already activated.";
          continue;
        }
        auto restrict_rule_ids = action->get_mutable_restrict_rule_ids();
        grant->set_service_state(SERVICE_RESTRICTED, *credit_uc);
        for (auto &rule : grant->final_action_info.restrict_rules) {
          restrict_rule_ids->push_back(rule);
        }
      }
      case ACTIVATE_SERVICE:
        action->set_ambr(config_.get_apn_ambr());
      case TERMINATE_SERVICE:
        MLOG(MDEBUG) << "Subscriber " << imsi_ << " rating group " << key
                     << " action type " << action_type;
        action->set_credit_key(key);
        action->set_imsi(imsi_);
        action->set_ip_addr(config_.common_context.ue_ipv4());
        action->set_session_id(session_id_);
        static_rules_.get_rule_ids_for_charging_key(
            key, *action->get_mutable_rule_ids());
        dynamic_rules_.get_rule_definitions_for_charging_key(
            key, *action->get_mutable_rule_definitions());
        actions_out->push_back(std::move(action));
        break;
      default:
        MLOG(MWARNING) << "Unexpected action type " << action_type;
        break;
    }
  }
}

// Monitors
bool SessionState::receive_monitor(
    const UsageMonitoringUpdateResponse& update,
    SessionStateUpdateCriteria& update_criteria) {
  if (!update.has_credit()) {
    // We are overloading UsageMonitoringUpdateResponse/Request with other
    // EventTriggered requests, so we could receive updates that don't affect
    // UsageMonitors.
    MLOG(MINFO) << "Received a UsageMonitoringUpdateResponse without a monitor"
                << ", not creating a monitor.";
    return true;
  }
  if (update.success() &&
      update.credit().level() == MonitoringLevel::SESSION_LEVEL) {
    update_session_level_key(update, update_criteria);
  }
  auto mkey = update.credit().monitoring_key();
  auto it = monitor_map_.find(mkey);

  if (update_criteria.monitor_credit_map.find(mkey) !=
      update_criteria.monitor_credit_map.end() &&
      update_criteria.monitor_credit_map[mkey].deleted){
    // This will only happen if the PCRF responds back with more credit when
    // the monitor has already been set to be terminated
    MLOG(MDEBUG) <<"Ignoring Monitor update" << mkey << " update because it "
                                                  "has been set for deletion";
    return false;
  }

  if (it == monitor_map_.end()) {
    // new credit
    return init_new_monitor(update, update_criteria);
  }
  auto credit_uc = get_monitor_uc(mkey, update_criteria);
  if (!update.success()) {
    it->second->credit.mark_failure(update.result_code(), credit_uc);
    return false;
  }

  MLOG(MINFO) << session_id_ << " Received monitor credit for " << mkey;
  const auto& gsu = update.credit().granted_units();
  it->second->credit.receive_credit(gsu, credit_uc);
  return true;
}

void SessionState::apply_monitor_updates(
    const std::string& key, SessionCreditUpdateCriteria& update) {
  auto it = monitor_map_.find(key);
  if (it == monitor_map_.end()) {
    return;
  }

  auto& credit = it->second->credit;
  // Credit merging
  credit.set_grant_tracking_type(update.grant_tracking_type, update);
  credit.set_received_granted_units(update.received_granted_units,update);
  for (int i = USED_TX; i != MAX_VALUES; i++) {
    Bucket bucket = static_cast<Bucket>(i);
    it->second->credit.add_credit(
        update.bucket_deltas.find(bucket)->second, bucket, update);
  }
}

uint64_t SessionState::get_monitor(
    const std::string& key, Bucket bucket) const {
  auto it = monitor_map_.find(key);
  if (it == monitor_map_.end()) {
    return 0;
  }
  return it->second->credit.get_credit(bucket);
}

bool SessionState::add_to_monitor(
    const std::string& key, uint64_t used_tx, uint64_t used_rx,
    SessionStateUpdateCriteria& uc) {
  auto it = monitor_map_.find(key);
  if (it == monitor_map_.end()) {
    MLOG(MDEBUG) << "Monitoring Key " << key
                 << " not found, not adding the usage";
    return false;
  }

  auto credit_uc = get_monitor_uc(key, uc);
  // add credit or delete monitor
  if (it->second->should_delete_monitor() ){
    MLOG(MINFO) << "Erasing monitor " << key << " due to quota exhausted";
    if (it->second->level == MonitoringLevel::SESSION_LEVEL){
      uc.is_session_level_key_updated = true;
      uc.updated_session_level_key = "";
    }
    credit_uc->deleted = true;
    monitor_map_.erase(key);
  } else {
    it->second->credit.add_used_credit(used_tx, used_rx, *credit_uc);
  }
  return true;
}

void SessionState::set_monitor(
    const std::string& key, Monitor monitor,
    SessionStateUpdateCriteria& update_criteria) {
  update_criteria.monitor_credit_to_install[key] = monitor.marshal();
  monitor_map_[key] = std::make_unique<Monitor>(monitor);
}

bool SessionState::reset_reporting_monitor(
    const std::string& key, SessionStateUpdateCriteria& update_criteria) {
  auto it = monitor_map_.find(key);
  if (it == monitor_map_.end()) {
    MLOG(MERROR) << "Could not reset credit for IMSI" << imsi_
                 << " and monitoring key " << key << " because it wasn't found";
    return false;
  }
  auto credit_uc = get_monitor_uc(key, update_criteria);
  it->second->credit.reset_reporting_credit(credit_uc);
  return true;
}

bool SessionState::init_new_monitor(
    const UsageMonitoringUpdateResponse& update,
    SessionStateUpdateCriteria& update_criteria) {
  if (!update.success()) {
    MLOG(MERROR) << "Monitoring init failed for imsi " << imsi_
                 << " and monitoring key " << update.credit().monitoring_key();
    return false;
  }
  if (update.credit().action() == UsageMonitoringCredit::DISABLE) {
    MLOG(MWARNING) << "Monitoring init has action disabled for subscriber "
                   << imsi_ << " and monitoring key "
                   << update.credit().monitoring_key();
    return false;
  }
  MLOG(MDEBUG) << session_id_ << " Initialized a monitoring credit for mkey "
               << update.credit().monitoring_key();
  auto monitor   = std::make_unique<Monitor>();
  monitor->level = update.credit().level();
  // validity time and final units not used for monitors
  auto _ = SessionCreditUpdateCriteria{};
  FinalActionInfo final_action_info;
  auto gsu = update.credit().granted_units();
  monitor->credit.receive_credit(gsu, NULL);

  update_criteria.monitor_credit_to_install[update.credit().monitoring_key()] =
      monitor->marshal();
  monitor_map_[update.credit().monitoring_key()] = std::move(monitor);
  return true;
}

void SessionState::update_session_level_key(
    const UsageMonitoringUpdateResponse& update,
    SessionStateUpdateCriteria& uc) {
  const auto& new_key = update.credit().monitoring_key();
  if (session_level_key_ != "" && session_level_key_ != new_key) {
    MLOG(MINFO) << "Session level monitoring key is updated from "
                << session_level_key_ << " to " << new_key;
  }
  if (update.credit().action() == UsageMonitoringCredit::DISABLE) {
    session_level_key_ = "";
  } else {
    session_level_key_ = new_key;
  }
  uc.is_session_level_key_updated = true;
  uc.updated_session_level_key    = session_level_key_;
}

void SessionState::set_session_level_key(const std::string new_key) {
  session_level_key_ = new_key;
}

BearerUpdate SessionState::get_dedicated_bearer_updates(
    RulesToProcess& rules_to_activate, RulesToProcess& rules_to_deactivate,
    SessionStateUpdateCriteria& uc) {
  BearerUpdate update;
  // Rule Installs
  for (const auto& rule_id : rules_to_activate.static_rules) {
    update_bearer_creation_req(STATIC, rule_id, config_, update);
  }
  for (const auto& rule : rules_to_activate.dynamic_rules) {
    const auto& rule_id = rule.id();
    update_bearer_creation_req(DYNAMIC, rule_id, config_, update);
  }

  // Rule Removals
  for (const auto& rule_id : rules_to_deactivate.static_rules) {
    update_bearer_deletion_req(STATIC, rule_id, config_, update, uc);
  }
  for (const auto& rule : rules_to_deactivate.dynamic_rules) {
    const auto& rule_id = rule.id();
    update_bearer_deletion_req(DYNAMIC, rule_id, config_, update, uc);
  }
  return update;
}

void SessionState::bind_policy_to_bearer(
    const PolicyBearerBindingRequest& request, SessionStateUpdateCriteria& uc) {
  const std::string& rule_id = request.policy_rule_id();
  auto policy_type           = get_policy_type(rule_id);
  if (!policy_type) {
    MLOG(MDEBUG) << "Policy " << rule_id
                 << " not found, when trying to bind to bearerID "
                 << request.bearer_id();
    return;
  }
  MLOG(MINFO) << session_id_ << " now has policy " << rule_id
              << " tied to bearerID " << request.bearer_id();
  bearer_id_by_policy_[PolicyID(*policy_type, rule_id)] = request.bearer_id();
  uc.is_bearer_mapping_updated = true;
  uc.bearer_id_by_policy       = bearer_id_by_policy_;
}

std::experimental::optional<PolicyType> SessionState::get_policy_type(
    const std::string& rule_id) {
  if (is_static_rule_installed(rule_id)) {
    return STATIC;
  } else if (is_dynamic_rule_installed(rule_id)) {
    return DYNAMIC;
  } else {
    return {};
  }
}

SessionCreditUpdateCriteria* SessionState::get_monitor_uc(
    const std::string& key, SessionStateUpdateCriteria& uc) {
  if (uc.monitor_credit_map.find(key) == uc.monitor_credit_map.end()) {
    uc.monitor_credit_map[key] =
        monitor_map_[key]->credit.get_update_criteria();
  }
  return &(uc.monitor_credit_map[key]);
}

// Event Triggers
void SessionState::get_event_trigger_updates(
    UpdateSessionRequest& update_request_out,
    std::vector<std::unique_ptr<ServiceAction>>* actions_out,
    SessionStateUpdateCriteria& update_criteria) {
  // todo We should also handle other event triggers here too
  auto it = pending_event_triggers_.find(REVALIDATION_TIMEOUT);
  if (it != pending_event_triggers_.end() && it->second == READY) {
    MLOG(MDEBUG) << "Session " << session_id_
                 << " updating due to EventTrigger: REVALIDATION_TIMEOUT"
                 << " with request number " << request_number_;
    auto new_req = update_request_out.mutable_usage_monitors()->Add();
    add_common_fields_to_usage_monitor_update(new_req);
    new_req->set_event_trigger(REVALIDATION_TIMEOUT);
    request_number_++;
    update_criteria.request_number_increment++;
    // todo we might want to make sure that the update went successfully before
    // clearing here
    remove_event_trigger(REVALIDATION_TIMEOUT, update_criteria);
  }
}

void SessionState::add_new_event_trigger(
    magma::lte::EventTrigger trigger,
    SessionStateUpdateCriteria& update_criteria) {
  MLOG(MINFO) << "Event Trigger " << trigger << " is pending for "
              << session_id_;
  set_event_trigger(trigger, PENDING, update_criteria);
}

void SessionState::mark_event_trigger_as_triggered(
    magma::lte::EventTrigger trigger,
    SessionStateUpdateCriteria& update_criteria) {
  auto it = pending_event_triggers_.find(trigger);
  if (it == pending_event_triggers_.end() ||
      pending_event_triggers_[trigger] != PENDING) {
    MLOG(MWARNING) << "Event Trigger " << trigger << " requested to be "
                   << "triggered is not pending for " << session_id_;
  }
  MLOG(MINFO) << "Event Trigger " << trigger << " is ready to update for "
              << session_id_;
  set_event_trigger(trigger, READY, update_criteria);
}

void SessionState::remove_event_trigger(
    magma::lte::EventTrigger trigger,
    SessionStateUpdateCriteria& update_criteria) {
  MLOG(MINFO) << "Event Trigger " << trigger << " is removed for "
              << session_id_;
  pending_event_triggers_.erase(trigger);
  set_event_trigger(trigger, CLEARED, update_criteria);
}

void SessionState::set_event_trigger(
    magma::lte::EventTrigger trigger, const EventTriggerState value,
    SessionStateUpdateCriteria& update_criteria) {
  pending_event_triggers_[trigger]                  = value;
  update_criteria.is_pending_event_triggers_updated = true;
  update_criteria.pending_event_triggers[trigger]   = value;
}

void SessionState::set_revalidation_time(
    const google::protobuf::Timestamp& time,
    SessionStateUpdateCriteria& update_criteria) {
  revalidation_time_                = time;
  update_criteria.revalidation_time = time;
}

bool SessionState::is_credit_in_final_unit_state(
    const CreditKey& charging_key) const {
  auto it = credit_map_.find(charging_key);
  if (it == credit_map_.end()) {
    return false;
  }
  return (it->second->service_state == SERVICE_REDIRECTED ||
      it->second->service_state == SERVICE_RESTRICTED);
}

// QoS/Bearer Management
bool SessionState::policy_has_qos(
    const PolicyType policy_type, const std::string& rule_id,
    PolicyRule* rule_out) {
  if (policy_type == STATIC) {
    bool exists = static_rules_.get_rule(rule_id, rule_out);
    return exists && rule_out->has_qos();
  }
  if (policy_type == DYNAMIC) {
    bool exists = dynamic_rules_.get_rule(rule_id, rule_out);
    return exists && rule_out->has_qos();
  }
  return false;
}

void SessionState::update_bearer_creation_req(
    const PolicyType policy_type, const std::string& rule_id,
    const SessionConfig& config, BearerUpdate& update) {
  if (!config.rat_specific_context.has_lte_context()) {
    return;
  }
  if (bearer_id_by_policy_.find(PolicyID(policy_type, rule_id)) !=
      bearer_id_by_policy_.end()) {
    // Policy already has a bearer
    return;
  }
  PolicyRule rule;
  if (!policy_has_qos(policy_type, rule_id, &rule)) {
    // Only create a bearer for policies with QoS
    return;
  }
  auto default_qci = FlowQos_Qci(
      config.rat_specific_context.lte_context().qos_info().qos_class_id());
  if (rule.qos().qci() == default_qci) {
    // This QCI is already covered by the default bearer
    return;
  }

  // If it is first time filling in the CreationReq, fill in other info
  if (!update.needs_creation) {
    update.needs_creation = true;
    update.create_req.mutable_sid()->CopyFrom(config.common_context.sid());
    update.create_req.set_ip_addr(config.common_context.ue_ipv4());
    update.create_req.set_link_bearer_id(
        config.rat_specific_context.lte_context().bearer_id());
  }
  update.create_req.mutable_policy_rules()->Add()->CopyFrom(rule);
  // We will add the new policyID to bearerID association, once we receive a
  // message from SGW.
}

void SessionState::update_bearer_deletion_req(
    const PolicyType policy_type, const std::string& rule_id,
    const SessionConfig& config, BearerUpdate& update,
    SessionStateUpdateCriteria& uc) {
  if (!config.rat_specific_context.has_lte_context()) {
    return;
  }
  if (bearer_id_by_policy_.find(PolicyID(policy_type, rule_id)) ==
      bearer_id_by_policy_.end()) {
    return;
  }
  // map change needs to be propagated to the store
  const auto bearer_id_to_delete =
      bearer_id_by_policy_[PolicyID(policy_type, rule_id)];
  bearer_id_by_policy_.erase(PolicyID(policy_type, rule_id));
  uc.is_bearer_mapping_updated = true;
  uc.bearer_id_by_policy       = bearer_id_by_policy_;

  // If it is first time filling in the DeletionReq, fill in other info
  if (!update.needs_deletion) {
    update.needs_deletion = true;
    auto& req             = update.delete_req;
    req.mutable_sid()->CopyFrom(config.common_context.sid());
    req.set_ip_addr(config.common_context.ue_ipv4());
    req.set_link_bearer_id(
        config.rat_specific_context.lte_context().bearer_id());
  }
  update.delete_req.mutable_eps_bearer_ids()->Add(bearer_id_to_delete);
}

RuleSetToApply::RuleSetToApply(const magma::lte::RuleSet& rule_set) {
  for (const auto& static_rule_install : rule_set.static_rules()) {
    static_rules.insert(static_rule_install.rule_id());
  }
  for (const auto& dynamic_rule_install : rule_set.dynamic_rules()) {
    dynamic_rules[dynamic_rule_install.policy_rule().id()] =
        dynamic_rule_install.policy_rule();
  }
}

void RuleSetToApply::combine_rule_set(const RuleSetToApply& other) {
  for (const auto& static_rule : other.static_rules) {
    static_rules.insert(static_rule);
  }
  for (const auto& dynamic_pair : other.dynamic_rules) {
    dynamic_rules[dynamic_pair.first] = dynamic_pair.second;
  }
}

RuleSetBySubscriber::RuleSetBySubscriber(
    const RulesPerSubscriber& rules_per_subscriber) {
  imsi = rules_per_subscriber.imsi();
  for (const auto& rule_set : rules_per_subscriber.rule_set()) {
    if (rule_set.apply_subscriber_wide()) {
      subscriber_wide_rule_set = RuleSetToApply(rule_set);
    } else {
      subscriber_wide_rule_set        = {};
      rule_set_by_apn[rule_set.apn()] = RuleSetToApply(rule_set);
    }
  }
}

std::experimental::optional<RuleSetToApply>
RuleSetBySubscriber::get_combined_rule_set_for_apn(const std::string& apn) {
  const bool apn_rule_set_exists =
      rule_set_by_apn.find(apn) != rule_set_by_apn.end();
  // Apply subscriber wide rule set if it exists. Also apply per-APN rule
  // set if it exists.
  if (apn_rule_set_exists && subscriber_wide_rule_set) {
    auto rule_set_to_apply = rule_set_by_apn[apn];
    rule_set_to_apply.combine_rule_set(*subscriber_wide_rule_set);
    return rule_set_to_apply;
  }
  if (subscriber_wide_rule_set) {
    return subscriber_wide_rule_set;
  }
  if (apn_rule_set_exists) {
    return rule_set_by_apn[apn];
  }
  return {};
}

void SessionState::update_data_usage_metrics(
    uint64_t bytes_tx, uint64_t bytes_rx) {
  const auto sid    = get_config().common_context.sid().id();
  const auto msisdn = get_config().common_context.msisdn();
  const auto apn    = get_config().common_context.apn();
  increment_counter(
      "ue_reported_usage", bytes_tx, size_t(4), LABEL_IMSI, sid.c_str(),
      LABEL_APN, apn.c_str(), LABEL_MSISDN, msisdn.c_str(), LABEL_DIRECTION,
      DIRECTION_UP);
  increment_counter(
      "ue_reported_usage", bytes_rx, size_t(4), LABEL_IMSI, sid.c_str(),
      LABEL_APN, apn.c_str(), LABEL_MSISDN, msisdn.c_str(), LABEL_DIRECTION,
      DIRECTION_DOWN);
}

}  // namespace magma
