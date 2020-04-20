// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/route_graph.h"

#include <algorithm>

#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/lib/logging/logging.h"

namespace media::audio {

RouteGraph::RouteGraph(const DeviceConfig& device_config, LinkMatrix* link_matrix)
    : link_matrix_(*link_matrix), device_config_(device_config) {
  FX_DCHECK(link_matrix);
}

RouteGraph::~RouteGraph() {
  if (throttle_release_fence_) {
    throttle_release_fence_->complete_ok();
  }
}

void RouteGraph::SetThrottleOutput(ThreadingModel* threading_model,
                                   std::shared_ptr<AudioOutput> throttle_output) {
  fit::bridge<void, void> bridge;
  threading_model->FidlDomain().ScheduleTask(bridge.consumer.promise().then(
      [throttle_output](fit::result<void, void>& _) { return throttle_output->Shutdown(); }));

  threading_model->FidlDomain().executor()->schedule_task(
      throttle_output->Startup().or_else([throttle_output](zx_status_t& error) {
        FX_PLOGS(ERROR, error) << "Failed to initialize the throttle output";
        return throttle_output->Shutdown();
      }));

  throttle_release_fence_ = {std::move(bridge.completer)};
  throttle_output_ = throttle_output;
  AddDevice(throttle_output_.get());
}

void RouteGraph::AddDevice(AudioDevice* device) {
  TRACE_DURATION("audio", "RouteGraph::AddDevice");
  AUD_VLOG(TRACE) << "Added device to route graph: " << device;

  devices_.push_front(device);
  UpdateGraphForDeviceChange();
}

void RouteGraph::RemoveDevice(AudioDevice* device) {
  TRACE_DURATION("audio", "RouteGraph::RemoveDevice");
  AUD_VLOG(TRACE) << "Removing device from graph: " << device;

  auto it = std::find(devices_.begin(), devices_.end(), device);
  if (it == devices_.end()) {
    FX_LOGS(WARNING) << "Attempted to remove unregistered device from the route graph.";
    return;
  }

  devices_.erase(it);
  UpdateGraphForDeviceChange();
}

void RouteGraph::AddRenderer(std::unique_ptr<AudioObject> renderer) {
  TRACE_DURATION("audio", "RouteGraph::AddRenderer");
  FX_DCHECK(throttle_output_);
  FX_DCHECK(renderer->is_audio_renderer());
  AUD_VLOG(TRACE) << "Adding renderer route graph: " << renderer.get();

  renderers_.insert(
      {renderer.get(), RoutableOwnedObject{std::shared_ptr<AudioObject>(renderer.release()), {}}});
}

void RouteGraph::SetRendererRoutingProfile(const AudioObject& renderer, RoutingProfile profile) {
  TRACE_DURATION("audio", "RouteGraph::SetRendererRoutingProfile");
  FX_DCHECK(renderer.is_audio_renderer());
  FX_DCHECK(renderer.format_valid() || !profile.routable)
      << "AudioRenderer without PCM format was added to route graph";
  AUD_VLOG(TRACE) << "Setting renderer route profile: " << &renderer;

  auto it = renderers_.find(&renderer);
  if (it == renderers_.end()) {
    FX_LOGS(WARNING) << "Tried to set routing policy for an unregistered renderer.";
    return;
  }

  it->second.profile = std::move(profile);
  if (!it->second.profile.routable || !it->second.profile.usage.is_render_usage()) {
    link_matrix_.Unlink(*it->second.ref);
    return;
  }

  auto output = TargetForUsage(it->second.profile.usage);
  if (!output.is_linkable()) {
    FX_LOGS(WARNING) << "Tried to route AudioRenderer, but no device available for usage "
                     << it->second.profile.usage.ToString();
    link_matrix_.Unlink(*it->second.ref);
    return;
  }

  if (link_matrix_.AreLinked(*it->second.ref, *output.device)) {
    return;
  }

  link_matrix_.Unlink(*it->second.ref);

  link_matrix_.LinkObjects(it->second.ref, output.device->shared_from_this(), output.transform);
}

void RouteGraph::RemoveRenderer(const AudioObject& renderer) {
  TRACE_DURATION("audio", "RouteGraph::RemoveRenderer");
  FX_DCHECK(renderer.is_audio_renderer());
  AUD_VLOG(TRACE) << "Removing renderer from route graph: " << &renderer;

  auto it = renderers_.find(&renderer);
  if (it == renderers_.end()) {
    AUD_VLOG(TRACE) << "Renderer " << &renderer << " was not present in graph.";
    return;
  }

  link_matrix_.Unlink(*it->second.ref);
  renderers_.erase(it);
}

void RouteGraph::AddCapturer(std::unique_ptr<AudioObject> capturer) {
  TRACE_DURATION("audio", "RouteGraph::AddCapturer");
  FX_DCHECK(capturer->is_audio_capturer());
  AUD_VLOG(TRACE) << "Adding capturer to route graph: " << capturer.get();

  capturers_.insert(
      {capturer.get(), RoutableOwnedObject{std::shared_ptr<AudioObject>(capturer.release()), {}}});
}

void RouteGraph::SetCapturerRoutingProfile(const AudioObject& capturer, RoutingProfile profile) {
  TRACE_DURATION("audio", "RouteGraph::SetCapturerRoutingProfile");
  FX_DCHECK(capturer.is_audio_capturer());
  AUD_VLOG(TRACE) << "Setting capturer route profile: " << &capturer;

  auto it = capturers_.find(&capturer);
  if (it == capturers_.end()) {
    FX_LOGS(WARNING) << "Tried to set routing policy for an unregistered renderer.";
    return;
  }

  it->second.profile = std::move(profile);
  if (!it->second.profile.routable || !it->second.profile.usage.is_capture_usage()) {
    link_matrix_.Unlink(*it->second.ref);
    return;
  }

  auto target = TargetForUsage(it->second.profile.usage);
  if (!target.is_linkable()) {
    FX_LOGS(WARNING) << "Tried to route AudioCapturer, but no device available for usage "
                     << it->second.profile.usage.ToString();
    link_matrix_.Unlink(*it->second.ref);
    return;
  }

  if (link_matrix_.AreLinked(*target.device, *it->second.ref)) {
    return;
  }

  link_matrix_.Unlink(*it->second.ref);

  link_matrix_.LinkObjects(target.device->shared_from_this(), it->second.ref, target.transform);
}

void RouteGraph::RemoveCapturer(const AudioObject& capturer) {
  TRACE_DURATION("audio", "RouteGraph::RemoveCapturer");
  FX_DCHECK(capturer.is_audio_capturer());
  AUD_VLOG(TRACE) << "Removing capturer from route graph: " << &capturer;

  auto it = capturers_.find(&capturer);
  if (it == capturers_.end()) {
    AUD_VLOG(TRACE) << "Capturer " << &capturer << " was not present in graph.";
    return;
  }

  link_matrix_.Unlink(*it->second.ref);
  capturers_.erase(it);
}

void RouteGraph::UpdateGraphForDeviceChange() {
  TRACE_DURATION("audio", "RouteGraph::UpdateGraphForDeviceChange");
  auto [targets, unlink_command] = CalculateTargets();
  targets_ = targets;
  Unlink(unlink_command);

  {
    TRACE_DURATION("audio", "RouteGraph::UpdateGraphForDeviceChange.renderers");
    std::for_each(renderers_.begin(), renderers_.end(), [this](auto& renderer) {
      Target target;
      if (!renderer.second.profile.routable ||
          !((target = TargetForUsage(renderer.second.profile.usage)).is_linkable()) ||
          link_matrix_.DestLinkCount(*renderer.second.ref) > 0u) {
        return;
      }

      link_matrix_.LinkObjects(renderer.second.ref, target.device->shared_from_this(),
                               target.transform);
    });
  }

  {
    TRACE_DURATION("audio", "RouteGraph::UpdateGraphForDeviceChange.capturers");
    std::for_each(capturers_.begin(), capturers_.end(), [this](auto& capturer) {
      Target target;
      if (!capturer.second.profile.routable ||
          !((target = TargetForUsage(capturer.second.profile.usage)).is_linkable()) ||
          link_matrix_.DestLinkCount(*capturer.second.ref) > 0u) {
        return;
      }

      link_matrix_.LinkObjects(target.device->shared_from_this(), capturer.second.ref,
                               target.transform);
    });
  }
}

std::pair<RouteGraph::Targets, RouteGraph::UnlinkCommand> RouteGraph::CalculateTargets() const {
  TRACE_DURATION("audio", "RouteGraph::CalculateTargets");
  // We generate a new set of targets.
  // We generate an unlink command to unlink anything linked to a target which has changed.

  Targets new_targets = {};
  UnlinkCommand unlink;
  for (const auto& usage : kStreamUsages) {
    const auto idx = HashStreamUsage(usage);
    new_targets[idx] = [this, usage]() {
      for (auto device : devices_) {
        if (device == throttle_output_.get()) {
          continue;
        }

        const auto& profile = LookupDeviceProfile(device);
        if (profile.supports_usage(usage)) {
          return Target(device, profile.loudness_transform());
        }
      }

      if (usage.is_render_usage()) {
        return Target(throttle_output_.get(),
                      LookupDeviceProfile(throttle_output_.get()).loudness_transform());
      } else {
        return Target();
      }
    }();

    unlink[idx] = targets_[idx].device != new_targets[idx].device;
  }

  return {new_targets, unlink};
}

void RouteGraph::Unlink(UnlinkCommand unlink_command) {
  TRACE_DURATION("audio", "RouteGraph::Unlink");
  std::for_each(renderers_.begin(), renderers_.end(), [this, &unlink_command](auto& renderer) {
    auto usage = renderer.second.profile.usage;
    if (!usage.is_empty() && unlink_command[HashStreamUsage(usage)]) {
      link_matrix_.Unlink(*renderer.second.ref);
    }
  });
  std::for_each(capturers_.begin(), capturers_.end(), [this, &unlink_command](auto& capturer) {
    auto usage = capturer.second.profile.usage;
    if (!usage.is_empty() && unlink_command[HashStreamUsage(usage)]) {
      link_matrix_.Unlink(*capturer.second.ref);
    }
  });
}

RouteGraph::Target RouteGraph::TargetForUsage(const StreamUsage& usage) const {
  if (usage.is_empty()) {
    return Target();
  }
  return targets_[HashStreamUsage(usage)];
}

const DeviceConfig::DeviceProfile& RouteGraph::LookupDeviceProfile(AudioDevice* device) const {
  auto driver = device->driver();
  if (device->is_output()) {
    if (!driver) {
      return device_config_.default_output_device_profile();
    }
    const auto& device_id = driver->persistent_unique_id();
    return device_config_.output_device_profile(device_id);
  } else {
    if (!driver) {
      return device_config_.default_input_device_profile();
    }
    const auto& device_id = driver->persistent_unique_id();
    return device_config_.input_device_profile(device_id);
  }
}

}  // namespace media::audio
