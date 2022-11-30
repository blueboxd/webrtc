/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_CONGESTION_CONTROLLER_GOOG_CC_PROBE_CONTROLLER_H_
#define MODULES_CONGESTION_CONTROLLER_GOOG_CC_PROBE_CONTROLLER_H_

#include <stdint.h>

#include <initializer_list>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/types/optional.h"
#include "api/field_trials_view.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/transport/network_control.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/timestamp.h"
#include "rtc_base/experiments/field_trial_parser.h"

namespace webrtc {

struct ProbeControllerConfig {
  explicit ProbeControllerConfig(const FieldTrialsView* key_value_config);
  ProbeControllerConfig(const ProbeControllerConfig&);
  ProbeControllerConfig& operator=(const ProbeControllerConfig&) = default;
  ~ProbeControllerConfig();

  // These parameters configure the initial probes. First we send one or two
  // probes of sizes p1 * start_bitrate_ and p2 * start_bitrate_.
  // Then whenever we get a bitrate estimate of at least further_probe_threshold
  // times the size of the last sent probe we'll send another one of size
  // step_size times the new estimate.
  FieldTrialParameter<double> first_exponential_probe_scale;
  FieldTrialOptional<double> second_exponential_probe_scale;
  FieldTrialParameter<double> further_exponential_probe_scale;
  FieldTrialParameter<double> further_probe_threshold;

  // Configures how often we send ALR probes and how big they are.
  FieldTrialParameter<TimeDelta> alr_probing_interval;
  FieldTrialParameter<double> alr_probe_scale;

  // Configures how often we send probes if NetworkStateEstimate is available.
  FieldTrialParameter<TimeDelta> network_state_estimate_probing_interval;
  // If the network state estimate increase more than this rate, a probe is sent
  // the next process interval.
  FieldTrialParameter<double> network_state_estimate_fast_rampup_rate;
  // If the network state estimate decreases more than this rate, a probe is
  // sent the next process interval.
  FieldTrialParameter<double> network_state_estimate_drop_down_rate;
  FieldTrialParameter<double> network_state_probe_scale;
  // Overrides min_probe_duration if network_state_estimate_probing_interval
  // is set and a network state estimate is known.
  FieldTrialParameter<TimeDelta> network_state_probe_duration;

  // Configures the probes emitted by changed to the allocated bitrate.
  FieldTrialOptional<double> first_allocation_probe_scale;
  FieldTrialOptional<double> second_allocation_probe_scale;
  FieldTrialFlag allocation_allow_further_probing;
  FieldTrialParameter<DataRate> allocation_probe_max;

  // The minimum number probing packets used.
  FieldTrialParameter<int> min_probe_packets_sent;
  // The minimum probing duration.
  FieldTrialParameter<TimeDelta> min_probe_duration;
  // Max limit the target rate of a probe to current estimate if BWE is loss
  // limited.
  FieldTrialParameter<bool> limit_probe_target_rate_to_loss_bwe;
  // Dont send a probe if min(estimate, network state estimate) is larger than
  // this fraction of the set max bitrate.
  FieldTrialParameter<double> skip_if_estimate_larger_than_fraction_of_max;
};

// This class controls initiation of probing to estimate initial channel
// capacity. There is also support for probing during a session when max
// bitrate is adjusted by an application.
class ProbeController {
 public:
  explicit ProbeController(const FieldTrialsView* key_value_config,
                           RtcEventLog* event_log);
  ~ProbeController();

  ProbeController(const ProbeController&) = delete;
  ProbeController& operator=(const ProbeController&) = delete;

  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig> SetBitrates(
      DataRate min_bitrate,
      DataRate start_bitrate,
      DataRate max_bitrate,
      Timestamp at_time);

  // The total bitrate, as opposed to the max bitrate, is the sum of the
  // configured bitrates for all active streams.
  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig>
  OnMaxTotalAllocatedBitrate(DataRate max_total_allocated_bitrate,
                             Timestamp at_time);

  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig> OnNetworkAvailability(
      NetworkAvailability msg);

  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig> SetEstimatedBitrate(
      DataRate bitrate,
      bool bwe_limited_due_to_packet_loss,
      Timestamp at_time);

  void EnablePeriodicAlrProbing(bool enable);

  void SetAlrStartTimeMs(absl::optional<int64_t> alr_start_time);
  void SetAlrEndedTimeMs(int64_t alr_end_time);

  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig> RequestProbe(
      Timestamp at_time);

  // Sets a new maximum probing bitrate, without generating a new probe cluster.
  void SetMaxBitrate(DataRate max_bitrate);
  void SetNetworkStateEstimate(webrtc::NetworkStateEstimate estimate);

  // Resets the ProbeController to a state equivalent to as if it was just
  // created EXCEPT for `enable_periodic_alr_probing_`.
  void Reset(Timestamp at_time);

  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig> Process(
      Timestamp at_time);

 private:
  enum class State {
    // Initial state where no probing has been triggered yet.
    kInit,
    // Waiting for probing results to continue further probing.
    kWaitingForProbingResult,
    // Probing is complete.
    kProbingComplete,
  };

  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig>
  InitiateExponentialProbing(Timestamp at_time);
  ABSL_MUST_USE_RESULT std::vector<ProbeClusterConfig> InitiateProbing(
      Timestamp now,
      std::vector<DataRate> bitrates_to_probe,
      bool probe_further);
  bool TimeForAlrProbe(Timestamp at_time) const;
  bool TimeForNetworkStateProbe(Timestamp at_time) const;

  bool network_available_;
  bool bwe_limited_due_to_packet_loss_;
  State state_;
  DataRate min_bitrate_to_probe_further_ = DataRate::PlusInfinity();
  Timestamp time_last_probing_initiated_ = Timestamp::MinusInfinity();
  DataRate estimated_bitrate_ = DataRate::Zero();
  bool send_probe_on_next_process_interval_;
  absl::optional<webrtc::NetworkStateEstimate> network_estimate_;
  DataRate start_bitrate_ = DataRate::Zero();
  DataRate max_bitrate_ = DataRate::PlusInfinity();
  Timestamp last_bwe_drop_probing_time_ = Timestamp::Zero();
  absl::optional<Timestamp> alr_start_time_;
  absl::optional<Timestamp> alr_end_time_;
  bool enable_periodic_alr_probing_;
  Timestamp time_of_last_large_drop_ = Timestamp::MinusInfinity();
  DataRate bitrate_before_last_large_drop_ = DataRate::Zero();
  DataRate max_total_allocated_bitrate_ = DataRate::Zero();

  const bool in_rapid_recovery_experiment_;
  // For WebRTC.BWE.MidCallProbing.* metric.
  bool mid_call_probing_waiting_for_result_;
  DataRate mid_call_probing_bitrate_ = DataRate::Zero();
  DataRate mid_call_probing_succcess_threshold_ = DataRate::Zero();
  RtcEventLog* event_log_;

  int32_t next_probe_cluster_id_ = 1;

  ProbeControllerConfig config_;
};

}  // namespace webrtc

#endif  // MODULES_CONGESTION_CONTROLLER_GOOG_CC_PROBE_CONTROLLER_H_
