/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"
#include "deskflow/IClipboard.h"
#include "platform/WlClipboard.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace deskflow {

//! Clipboard manager for EiScreen
/*!
This class manages clipboard operations for the EiScreen implementation.
It automatically detects the best available clipboard backend and provides
a unified interface for clipboard operations.

Clipboard change detection is done by a background worker thread (see
requestLeaveSync): on compositors without ext-data-control (e.g.
GNOME/mutter) every wl-paste/wl-copy invocation briefly maps a popup
surface and steals focus, so the reads must not run on a timer or in the
screen-switch critical path. The worker runs them after the pointer has
left the screen and delivers the result to the caller via takeSyncResult.
*/
class WlClipboardCollection
{
public:
  //! Result of a background clipboard sync
  struct SyncResult
  {
    bool changed[kClipboardEnd] = {};
  };

  WlClipboardCollection();
  ~WlClipboardCollection();

  //! Check if clipboard functionality is available
  bool isAvailable() const;

  //! Get clipboard for specific ID
  IClipboard *getClipboard(ClipboardID id) const;

  //! Report whether the pointer is currently on the screen this
  //! collection belongs to. The sync worker waits for it to become false
  //! before running, so jitter at the screen edge doesn't trigger a wave
  //! of wl-paste invocations.
  void setOnScreenQuery(std::function<bool()> query);

  //! Request a background clipboard sync. Non-blocking; coalesces with a
  //! pending or running sync. Call when the pointer leaves the screen.
  void requestLeaveSync();

  //! True while a sync is requested, running, or has a result waiting
  bool hasWork() const;

  //! Take the result of a completed background sync. Returns false if no
  //! sync has completed since the last call.
  bool takeSyncResult(SyncResult &out);

private:
  //! Initialize clipboard backends
  void initialize();

  //! Cleanup clipboard backends
  void cleanup();

  //! Background worker loop
  void workerLoop();

private:
  std::vector<std::unique_ptr<WlClipboard>> m_clipboards;
  bool m_available = false;

  // Background sync worker
  std::thread m_worker;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop = false;
  bool m_syncRequested = false;
  // A request was consumed and its result is not yet delivered
  bool m_syncActive = false;
  bool m_syncDone = false;
  // A changed result was produced but not yet consumed (the pointer
  // returned to the screen in the meantime); forces the next sync to
  // report a change so the data isn't lost
  bool m_undelivered = false;
  SyncResult m_result;
  std::function<bool()> m_onScreenQuery;
};

} // namespace deskflow
