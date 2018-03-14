// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/zxdb/console/nouns.h"

#include <algorithm>
#include <utility>

#include "garnet/bin/zxdb/console/command.h"
#include "garnet/bin/zxdb/console/command_utils.h"
#include "garnet/bin/zxdb/console/console.h"
#include "garnet/bin/zxdb/console/console_context.h"
#include "garnet/bin/zxdb/console/output_buffer.h"
#include "garnet/bin/zxdb/client/err.h"
#include "garnet/bin/zxdb/client/process.h"
#include "garnet/bin/zxdb/client/session.h"
#include "garnet/bin/zxdb/client/system.h"
#include "garnet/public/lib/fxl/logging.h"
#include "garnet/public/lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

// Prints the thread list for the given process to the console.
void ListThreads(ConsoleContext* context, Process* process) {
  std::vector<Thread*> threads = process->GetThreads();
  int active_thread_id =
      context->GetActiveThreadIdForTarget(process->GetTarget());

  // Sort by ID.
  std::vector<std::pair<int, Thread*>> id_threads;
  for (Thread* thread : threads)
    id_threads.push_back(std::make_pair(context->IdForThread(thread), thread));
  std::sort(id_threads.begin(), id_threads.end());

  OutputBuffer out;
  for (const auto& pair : id_threads) {
    // "Current thread" marker.
    if (pair.first == active_thread_id)
      out.Append(">");
    else
      out.Append(" ");

    out.Append(DescribeThread(context, pair.second, true));
    out.Append("\n");
  }
  Console::get()->Output(std::move(out));
}

// Updates the thread list from the debugged process and asynchronously prints
// the result. When the user lists threads, we really don't want to be
// misleading and show out-of-date thread names which the developer might be
// relying on. Therefore, force a sync of the thread list from the target
// (which should be fast) before displaying the thread list.
void ScheduleListThreads(ConsoleContext* context, const Command& cmd) {
  Process* process = cmd.target()->GetProcess();
  if (!process) {
    Console::get()->Output(
        Err(ErrType::kInput, "Process not running, no threads."));
    return;
  }

  // Since the Process issues the callback, it's OK to capture the pointer.
  process->SyncThreads([process]() {
    ListThreads(&Console::get()->context(), process);
  });
}

// Returns true if the thread noun was specified and therefore handled.
bool HandleThread(ConsoleContext* context, const Command& cmd) {
  if (!cmd.HasNoun(Noun::kThread))
    return false;

  if (cmd.GetNounIndex(Noun::kThread) == Command::kNoIndex) {
    // Just "thread" or "process 2 thread" specified, this lists available
    // threads.
    ScheduleListThreads(context, cmd);
    return true;
  }

  // Explicit index provided, this switches the current context. The thread
  // should be already resolved to a valid pointer if it was specified on the
  // command line (otherwise the command would have been rejected before here).
  FXL_DCHECK(cmd.thread());
  context->SetActiveThreadForTarget(cmd.thread());
  // Setting the active thread also sets the active target.
  context->SetActiveTarget(cmd.target());
  return true;
}

void ListProcesses(ConsoleContext* context, const Command& cmd) {
  auto targets = context->session()->system().GetAllTargets();

  int active_target_id = context->GetActiveTargetId();

  // Sort by ID.
  std::vector<std::pair<int, Target*>> id_targets;
  for (auto& target : targets)
    id_targets.push_back(std::make_pair(context->IdForTarget(target), target));
  std::sort(id_targets.begin(), id_targets.end());

  OutputBuffer out;
  for (const auto& pair : id_targets) {
    // "Current process" marker.
    if (pair.first == active_target_id)
      out.Append(">");
    else
      out.Append(" ");

    out.Append(DescribeTarget(context, pair.second, true));
    out.Append("\n");
  }
  Console::get()->Output(std::move(out));
}

// Returns true if the process noun was specified and therefore handled.
bool HandleProcess(ConsoleContext* context, const Command& cmd) {
  if (!cmd.HasNoun(Noun::kProcess))
    return false;

  if (cmd.GetNounIndex(Noun::kProcess) == Command::kNoIndex) {
    // Just "process", this lists available processes.
    ListProcesses(context, cmd);
    return true;
  }

  // Explicit index provided, this switches the current context. The target
  // should be already resolved to a valid pointer if it was specified on the
  // command line (otherwise the command would have been rejected before here).
  context->SetActiveTarget(cmd.target());
  Console::get()->Output(DescribeTarget(context, cmd.target(), false));
  return true;
}

}  // namespace

Err ExecuteNoun(ConsoleContext* context, const Command& cmd) {
  Err result;
  // Work backwards in specificity (frame -> thread -> process).

  // TODO(brettw) frame.
  if (HandleThread(context, cmd))
    return result;
  if (HandleProcess(context, cmd))
    return result;
  return result;
}

}  // namespace zxdb
