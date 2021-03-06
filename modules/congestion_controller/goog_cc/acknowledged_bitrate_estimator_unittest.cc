/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/congestion_controller/goog_cc/acknowledged_bitrate_estimator.h"

#include <utility>

#include "absl/memory/memory.h"
#include "api/transport/field_trial_based_config.h"
#include "rtc_base/fake_clock.h"
#include "test/gmock.h"
#include "test/gtest.h"

using ::testing::_;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Return;

namespace webrtc {

namespace {

constexpr int64_t kFirstArrivalTimeMs = 10;
constexpr int64_t kFirstSendTimeMs = 10;
constexpr uint16_t kSequenceNumber = 1;
constexpr size_t kPayloadSize = 10;

class MockBitrateEstimator : public BitrateEstimator {
 public:
  using BitrateEstimator::BitrateEstimator;
  MOCK_METHOD3(Update,
               void(Timestamp at_time, DataSize data_size, bool in_alr));
  MOCK_CONST_METHOD0(bitrate, absl::optional<DataRate>());
  MOCK_METHOD0(ExpectFastRateChange, void());
};

struct AcknowledgedBitrateEstimatorTestStates {
  FieldTrialBasedConfig field_trial_config;
  std::unique_ptr<AcknowledgedBitrateEstimator> acknowledged_bitrate_estimator;
  MockBitrateEstimator* mock_bitrate_estimator;
};

AcknowledgedBitrateEstimatorTestStates CreateTestStates() {
  AcknowledgedBitrateEstimatorTestStates states;
  auto mock_bitrate_estimator =
      absl::make_unique<MockBitrateEstimator>(&states.field_trial_config);
  states.mock_bitrate_estimator = mock_bitrate_estimator.get();
  states.acknowledged_bitrate_estimator =
      absl::make_unique<AcknowledgedBitrateEstimator>(
          &states.field_trial_config, std::move(mock_bitrate_estimator));
  return states;
}

std::vector<PacketResult> CreateFeedbackVector() {
  std::vector<PacketResult> packet_feedback_vector(2);
  packet_feedback_vector[0].receive_time = Timestamp::ms(kFirstArrivalTimeMs);
  packet_feedback_vector[0].sent_packet.send_time =
      Timestamp::ms(kFirstSendTimeMs);
  packet_feedback_vector[0].sent_packet.sequence_number = kSequenceNumber;
  packet_feedback_vector[0].sent_packet.size = DataSize::bytes(kPayloadSize);
  packet_feedback_vector[1].receive_time =
      Timestamp::ms(kFirstArrivalTimeMs + 10);
  packet_feedback_vector[1].sent_packet.send_time =
      Timestamp::ms(kFirstSendTimeMs + 10);
  packet_feedback_vector[1].sent_packet.sequence_number = kSequenceNumber;
  packet_feedback_vector[1].sent_packet.size =
      DataSize::bytes(kPayloadSize + 10);
  return packet_feedback_vector;
}

}  // anonymous namespace

TEST(TestAcknowledgedBitrateEstimator, UpdateBandwidth) {
  auto states = CreateTestStates();
  auto packet_feedback_vector = CreateFeedbackVector();
  {
    InSequence dummy;
    EXPECT_CALL(*states.mock_bitrate_estimator,
                Update(packet_feedback_vector[0].receive_time,
                       packet_feedback_vector[0].sent_packet.size,
                       /*in_alr*/ false))
        .Times(1);
    EXPECT_CALL(*states.mock_bitrate_estimator,
                Update(packet_feedback_vector[1].receive_time,
                       packet_feedback_vector[1].sent_packet.size,
                       /*in_alr*/ false))
        .Times(1);
  }
  states.acknowledged_bitrate_estimator->IncomingPacketFeedbackVector(
      packet_feedback_vector);
}

TEST(TestAcknowledgedBitrateEstimator, ExpectFastRateChangeWhenLeftAlr) {
  auto states = CreateTestStates();
  auto packet_feedback_vector = CreateFeedbackVector();
  {
    InSequence dummy;
    EXPECT_CALL(*states.mock_bitrate_estimator,
                Update(packet_feedback_vector[0].receive_time,
                       packet_feedback_vector[0].sent_packet.size,
                       /*in_alr*/ false))
        .Times(1);
    EXPECT_CALL(*states.mock_bitrate_estimator, ExpectFastRateChange())
        .Times(1);
    EXPECT_CALL(*states.mock_bitrate_estimator,
                Update(packet_feedback_vector[1].receive_time,
                       packet_feedback_vector[1].sent_packet.size,
                       /*in_alr*/ false))
        .Times(1);
  }
  states.acknowledged_bitrate_estimator->SetAlrEndedTime(
      Timestamp::ms(kFirstArrivalTimeMs + 1));
  states.acknowledged_bitrate_estimator->IncomingPacketFeedbackVector(
      packet_feedback_vector);
}

TEST(TestAcknowledgedBitrateEstimator, ReturnBitrate) {
  auto states = CreateTestStates();
  absl::optional<DataRate> return_value = DataRate::kbps(42);
  EXPECT_CALL(*states.mock_bitrate_estimator, bitrate())
      .Times(1)
      .WillOnce(Return(return_value));
  EXPECT_EQ(return_value, states.acknowledged_bitrate_estimator->bitrate());
}

}  // namespace webrtc*/
