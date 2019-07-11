// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_BACKTRACE_CACHE_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_BACKTRACE_CACHE_H_

#include <map>

#include "src/developer/debug/zxdb/client/thread_observer.h"
#include "src/developer/debug/zxdb/symbols/file_line.h"

namespace zxdb {

// This class is meant as a storage for backtraces generated by thread
// exceptions. An instance will listen for those exceptions and attempt to get
// the backtrace if possible. This can fail as getting a backtrace is
// potentially asynchronous and the thread could be gone by then.
//
// This class doesn't know what thread it is tracking, it is for it's owner to
// correctly track that information and surface it in an useful manner.

class Location;
class Stack;
class TargetSymbols;

struct Backtrace {
  struct Frame {
    uint64_t address = 0;  // 0 is invalid.
    FileLine file_line;
    std::string function_name;
  };
  std::vector<Frame> frames;
};

class BacktraceCache final : public ThreadObserver {
 public:
  BacktraceCache();
  virtual ~BacktraceCache();

  fxl::WeakPtr<BacktraceCache> GetWeakPtr();

  void OnThreadStopped(Thread* thread, debug_ipc::NotifyException::Type type,
                       const std::vector<fxl::WeakPtr<Breakpoint>>& hit_breakpoints) override;

  const std::vector<Backtrace>& backtraces() const { return backtraces_; }

  void set_should_cache(bool s) { should_cache_ = s; }

 private:
  void StoreBacktrace(const Stack&);

  std::vector<Backtrace> backtraces_;
  bool should_cache_ = false;

  fxl::WeakPtrFactory<BacktraceCache> weak_factory_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_BACKTRACE_CACHE_H_
