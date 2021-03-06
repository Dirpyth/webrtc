/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "media/engine/webrtc_media_engine.h"

#include <utility>

#include "absl/algorithm/container.h"
#include "absl/memory/memory.h"
#include "media/engine/webrtc_voice_engine.h"
#include "system_wrappers/include/field_trial.h"

#ifdef HAVE_WEBRTC_VIDEO
#include "media/engine/webrtc_video_engine.h"
#else
#include "media/engine/null_webrtc_video_engine.h"
#endif

namespace cricket {

std::unique_ptr<MediaEngineInterface> CreateMediaEngine(
    MediaEngineDependencies dependencies) {
  auto audio_engine = absl::make_unique<WebRtcVoiceEngine>(
      dependencies.task_queue_factory, std::move(dependencies.adm),
      std::move(dependencies.audio_encoder_factory),
      std::move(dependencies.audio_decoder_factory),
      std::move(dependencies.audio_mixer),
      std::move(dependencies.audio_processing));
#ifdef HAVE_WEBRTC_VIDEO
  auto video_engine = absl::make_unique<WebRtcVideoEngine>(
      std::move(dependencies.video_encoder_factory),
      std::move(dependencies.video_decoder_factory));
#else
  auto video_engine = absl::make_unique<NullWebRtcVideoEngine>();
#endif
  return absl::make_unique<CompositeMediaEngine>(std::move(audio_engine),
                                                 std::move(video_engine));
}

namespace {
// If this FieldTrial is enabled, we will not filter out the abs-send-time
// header extension when the TWCC extensions were also negotiated, but keep
// kAbsSendTimeUri also if kTransportSequenceNumberUri is present.
bool IsKeepAbsSendTimeExtensionFieldTrialEnabled() {
  return webrtc::field_trial::IsEnabled("WebRTC-KeepAbsSendTimeExtension");
}

// Remove mutually exclusive extensions with lower priority.
void DiscardRedundantExtensions(
    std::vector<webrtc::RtpExtension>* extensions,
    rtc::ArrayView<const char* const> extensions_decreasing_prio) {
  RTC_DCHECK(extensions);
  bool found = false;
  for (const char* uri : extensions_decreasing_prio) {
    auto it = absl::c_find_if(
        *extensions,
        [uri](const webrtc::RtpExtension& rhs) { return rhs.uri == uri; });
    if (it != extensions->end()) {
      if (found) {
        extensions->erase(it);
      }
      found = true;
    }
  }
}
}  // namespace

bool ValidateRtpExtensions(
    const std::vector<webrtc::RtpExtension>& extensions) {
  bool id_used[1 + webrtc::RtpExtension::kMaxId] = {false};
  for (const auto& extension : extensions) {
    if (extension.id < webrtc::RtpExtension::kMinId ||
        extension.id > webrtc::RtpExtension::kMaxId) {
      RTC_LOG(LS_ERROR) << "Bad RTP extension ID: " << extension.ToString();
      return false;
    }
    if (id_used[extension.id]) {
      RTC_LOG(LS_ERROR) << "Duplicate RTP extension ID: "
                        << extension.ToString();
      return false;
    }
    id_used[extension.id] = true;
  }
  return true;
}

std::vector<webrtc::RtpExtension> FilterRtpExtensions(
    const std::vector<webrtc::RtpExtension>& extensions,
    bool (*supported)(const std::string&),
    bool filter_redundant_extensions) {
  RTC_DCHECK(ValidateRtpExtensions(extensions));
  RTC_DCHECK(supported);
  std::vector<webrtc::RtpExtension> result;

  // Ignore any extensions that we don't recognize.
  for (const auto& extension : extensions) {
    if (supported(extension.uri)) {
      result.push_back(extension);
    } else {
      RTC_LOG(LS_WARNING) << "Unsupported RTP extension: "
                          << extension.ToString();
    }
  }

  // Sort by name, ascending (prioritise encryption), so that we don't reset
  // extensions if they were specified in a different order (also allows us
  // to use std::unique below).
  absl::c_sort(result, [](const webrtc::RtpExtension& rhs,
                          const webrtc::RtpExtension& lhs) {
    return rhs.encrypt == lhs.encrypt ? rhs.uri < lhs.uri
                                      : rhs.encrypt > lhs.encrypt;
  });

  // Remove unnecessary extensions (used on send side).
  if (filter_redundant_extensions) {
    auto it = std::unique(
        result.begin(), result.end(),
        [](const webrtc::RtpExtension& rhs, const webrtc::RtpExtension& lhs) {
          return rhs.uri == lhs.uri && rhs.encrypt == lhs.encrypt;
        });
    result.erase(it, result.end());

    // Keep just the highest priority extension of any in the following lists.
    if (IsKeepAbsSendTimeExtensionFieldTrialEnabled()) {
      static const char* const kBweExtensionPriorities[] = {
          webrtc::RtpExtension::kAbsSendTimeUri,
          webrtc::RtpExtension::kTimestampOffsetUri};
      DiscardRedundantExtensions(&result, kBweExtensionPriorities);
    } else {
      static const char* const kBweExtensionPriorities[] = {
          webrtc::RtpExtension::kTransportSequenceNumberUri,
          webrtc::RtpExtension::kAbsSendTimeUri,
          webrtc::RtpExtension::kTimestampOffsetUri};
      DiscardRedundantExtensions(&result, kBweExtensionPriorities);
    }
  }
  return result;
}

webrtc::BitrateConstraints GetBitrateConfigForCodec(const Codec& codec) {
  webrtc::BitrateConstraints config;
  int bitrate_kbps = 0;
  if (codec.GetParam(kCodecParamMinBitrate, &bitrate_kbps) &&
      bitrate_kbps > 0) {
    config.min_bitrate_bps = bitrate_kbps * 1000;
  } else {
    config.min_bitrate_bps = 0;
  }
  if (codec.GetParam(kCodecParamStartBitrate, &bitrate_kbps) &&
      bitrate_kbps > 0) {
    config.start_bitrate_bps = bitrate_kbps * 1000;
  } else {
    // Do not reconfigure start bitrate unless it's specified and positive.
    config.start_bitrate_bps = -1;
  }
  if (codec.GetParam(kCodecParamMaxBitrate, &bitrate_kbps) &&
      bitrate_kbps > 0) {
    config.max_bitrate_bps = bitrate_kbps * 1000;
  } else {
    config.max_bitrate_bps = -1;
  }
  return config;
}
}  // namespace cricket
