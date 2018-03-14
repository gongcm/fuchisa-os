// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "garnet/bin/zxdb/client/process.h"

#include <map>
#include <memory>

#include "garnet/bin/zxdb/client/weak_thunk.h"
#include "garnet/public/lib/fxl/macros.h"

namespace zxdb {

class TargetImpl;
class ThreadImpl;

class ProcessImpl : public Process {
 public:
  ProcessImpl(TargetImpl* target, uint64_t koid, const std::string& name);
  ~ProcessImpl() override;

  // Process implementation:
  Target* GetTarget() const override;
  uint64_t GetKoid() const override;
  const std::string& GetName() const override;
  std::vector<Thread*> GetThreads() const override;
  void SyncThreads(std::function<void()> callback) override;
  void OnThreadStarting(const debug_ipc::ThreadRecord& record) override;
  void OnThreadExiting(const debug_ipc::ThreadRecord& record) override;

 private:
  // Syncs the threads_ list to the new list of threads passed in .
  void UpdateThreads(const std::vector<debug_ipc::ThreadRecord>& new_threads);

  Target* const target_;  // The target owns |this|.
  const uint64_t koid_;
  std::string name_;

  // Threads indexed by their thread koid.
  std::map<uint64_t, std::unique_ptr<ThreadImpl>> threads_;

  std::shared_ptr<WeakThunk<ProcessImpl>> weak_thunk_;
  // ^ Keep at the bottom to make sure it's destructed last.

  FXL_DISALLOW_COPY_AND_ASSIGN(ProcessImpl);
};

}  // namespace zxdb
