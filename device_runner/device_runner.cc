// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <mojo/system/main.h>

#include "apps/modular/services/device/device_runner.mojom.h"
#include "apps/modular/services/device/device_shell.mojom.h"
#include "apps/modular/services/user/user_runner.mojom.h"
#include "apps/mozart/services/launcher/interfaces/launcher.mojom.h"
#include "apps/mozart/services/views/interfaces/view_provider.mojom.h"
#include "apps/mozart/services/views/interfaces/view_token.mojom.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/macros.h"
#include "mojo/public/cpp/application/application_impl_base.h"
#include "mojo/public/cpp/application/connect.h"
#include "mojo/public/cpp/application/run_application.h"
#include "mojo/public/cpp/application/service_provider_impl.h"
#include "mojo/public/cpp/bindings/strong_binding.h"

namespace modular {

using mojo::ApplicationImplBase;
using mojo::Array;
using mojo::GetProxy;
using mojo::InterfaceHandle;
using mojo::InterfacePtr;
using mojo::InterfaceRequest;
using mojo::ServiceProvider;
using mojo::Shell;
using mojo::StrongBinding;
using mojo::StructPtr;
using mojo::String;

Array<uint8_t> UserIdentityArray(const std::string& username) {
  Array<uint8_t> array = Array<uint8_t>::New(username.length());
  for (size_t i = 0; i < username.length(); i++) {
    array[i] = static_cast<uint8_t>(username[i]);
  }
  return array;
}

class DeviceRunnerImpl : public DeviceRunner {
 public:
  DeviceRunnerImpl(Shell* const shell, InterfaceRequest<DeviceRunner> service)
      : shell_(shell), binding_(this, std::move(service)) {}

  ~DeviceRunnerImpl() override = default;

 private:
  void Login(const String& username,
             InterfaceRequest<mozart::ViewOwner> view_owner_request) override {
    FTL_LOG(INFO) << "DeviceRunnerImpl::Login() " << username;

    // TODO(alhaad): Once we have a better understanding of lifecycle
    // management, revisit this.
    ConnectToService(shell_, "mojo:story_manager", GetProxy(&story_manager_));

    StructPtr<ledger::Identity> identity = ledger::Identity::New();
    identity->user_id = UserIdentityArray(username);
    // |app_id| must not be null so it will pass mojo validation and must not
    // be empty or we'll get ledger::Status::AUTHENTICATION_ERROR when
    // StoryManagerImpl calls GetLedger().
    // TODO(jimbe): When there's support from the Ledger, open the user here,
    // then pass down a handle that is restricted from opening other users.
    identity->app_id = Array<uint8_t>::New(1);

    story_manager_->Launch(
        std::move(identity), std::move(view_owner_request), [](bool success) {
          FTL_LOG(INFO) << "DeviceRunnerImpl::Login() StoryManager.Launch()";
        });
  }

  Shell* const shell_;
  StrongBinding<DeviceRunner> binding_;

  // Interface pointer to the |StoryManager| handle exposed by the Story
  // Manager. Currently, we maintain a single instance which means that
  // subsequent logins override previous ones.
  InterfacePtr<StoryManager> story_manager_;

  FTL_DISALLOW_COPY_AND_ASSIGN(DeviceRunnerImpl);
};

class DeviceRunnerApp : public ApplicationImplBase {
 public:
  DeviceRunnerApp() = default;

  ~DeviceRunnerApp() override = default;

 private:
  void OnInitialize() override {
    FTL_LOG(INFO) << "DeviceRunnerApp::OnInitialize()";

    ConnectToService(shell(), "mojo:launcher", GetProxy(&mozart_launcher_));

    InterfacePtr<mozart::ViewProvider> view_provider;
    ConnectToService(shell(), "mojo:dummy_device_shell",
                     GetProxy(&view_provider));

    InterfaceHandle<mozart::ViewOwner> root_view;

    InterfacePtr<ServiceProvider> device_shell_provider;
    view_provider->CreateView(GetProxy(&root_view),
                              GetProxy(&device_shell_provider));

    mozart_launcher_->Display(std::move(root_view));

    device_shell_provider->ConnectToService(
        DeviceShell::Name_, GetProxy(&device_shell_).PassMessagePipe());

    InterfaceHandle<DeviceRunner> device_runner_handle;
    new DeviceRunnerImpl(shell(), GetProxy(&device_runner_handle));
    device_shell_->SetDeviceRunner(std::move(device_runner_handle));
  }

  InterfacePtr<DeviceShell> device_shell_;
  InterfacePtr<mozart::Launcher> mozart_launcher_;
  FTL_DISALLOW_COPY_AND_ASSIGN(DeviceRunnerApp);
};

}  // namespace modular

MojoResult MojoMain(MojoHandle application_request) {
  FTL_LOG(INFO) << "device runner main";
  modular::DeviceRunnerApp app;
  return mojo::RunApplication(application_request, &app);
}
