/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WlClipboardCollection.h"

#include "base/Log.h"
#include "deskflow/ClipboardTypes.h"

#include <chrono>

namespace deskflow {

namespace {
// How long after the last leave request the sync waits before running.
// Jitter at the screen edge produces a stream of requests; the window is
// extended by each new request, so a burst of switches results in a
// single sync.
const int kDebounceMs = 200;
} // namespace

WlClipboardCollection::WlClipboardCollection()
{
  initialize();
  if (m_available) {
    m_worker = std::thread([this] { workerLoop(); });
  }
}

WlClipboardCollection::~WlClipboardCollection()
{
  cleanup();
}

bool WlClipboardCollection::isAvailable() const
{
  return m_available;
}

IClipboard *WlClipboardCollection::getClipboard(ClipboardID id) const
{
  if (!m_available || id >= m_clipboards.size()) {
    return nullptr;
  }

  return m_clipboards[id].get();
}

void WlClipboardCollection::setOnScreenQuery(std::function<bool()> query)
{
  std::scoped_lock<std::mutex> lock(m_mutex);
  m_onScreenQuery = std::move(query);
}

void WlClipboardCollection::requestLeaveSync()
{
  if (!m_available) {
    return;
  }
  {
    std::scoped_lock<std::mutex> lock(m_mutex);
    m_syncRequested = true;
  }
  m_cv.notify_all();
}

bool WlClipboardCollection::hasWork() const
{
  std::scoped_lock<std::mutex> lock(m_mutex);
  return m_syncRequested || m_syncInFlight || m_syncDone;
}

bool WlClipboardCollection::takeSyncResult(SyncResult &out)
{
  std::scoped_lock<std::mutex> lock(m_mutex);
  if (!m_syncDone) {
    return false;
  }
  m_syncDone = false;
  // The result is now delivered; any previously undelivered change is
  // superseded by (or re-sent through) it.
  m_undelivered = false;
  // Note: m_syncInFlight is deliberately NOT touched here. If the worker
  // already consumed a newer request while this (older) result sat in
  // m_syncDone, clearing the in-flight state here would stop the polling
  // timer and orphan the newer result.
  out = m_result;
  return true;
}

void WlClipboardCollection::workerLoop()
{
  for (;;) {
    std::unique_lock<std::mutex> lock(m_mutex);

    // The previous iteration's result (if any) is either already fetched
    // or still pending. Either way, no new request is pending here, so
    // the in-flight window for it is over. Do this BEFORE waiting so a
    // request that arrives in the meantime is not lost.
    m_syncInFlight = false;

    m_cv.wait(lock, [this] { return m_stop || m_syncRequested; });
    if (m_stop) {
      break;
    }
    m_syncRequested = false;
    m_syncInFlight = true;

    // Debounce: wait until the pointer is off the screen and a quiet
    // period has passed since the last request. New requests arriving in
    // the meantime extend the quiet period (coalescing jitter at the
    // screen edge into a single sync).
    bool cancelled = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDebounceMs);
    for (;;) {
      const bool offScreen = !m_onScreenQuery || !m_onScreenQuery();
      if (offScreen && std::chrono::steady_clock::now() >= deadline) {
        break;
      }
      if (m_cv.wait_for(lock, std::chrono::milliseconds(50),
                        [this] { return m_stop || m_syncRequested; })) {
        if (m_stop) {
          cancelled = true;
        } else {
          m_syncRequested = false;
          deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDebounceMs);
        }
      }
    }

    if (cancelled) {
      m_syncInFlight = false;
      break;
    }

    lock.unlock();

    // Run the checks. These spawn wl-paste and take a few hundred
    // milliseconds, hence the background thread.
    //
    // A clipboard "changed" if either its set of MIME types changed or
    // its text content changed (copying different text keeps the same
    // types, so the content must be compared too). A previously
    // undelivered change forces a re-report: the type/content state was
    // already advanced by that check, so without it the change would be
    // lost if the pointer returned to the screen before the result was
    // consumed.
    SyncResult result;
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
      if (!m_clipboards[id]) {
        continue;
      }

      const bool typesChanged = m_clipboards[id]->checkChangePosix();

      std::string text;
      bool textChanged = false;
      if (m_clipboards[id]->readPosix(IClipboard::Format::Text, text)) {
        textChanged = (text != m_clipboards[id]->getCachedData(IClipboard::Format::Text));
        // Store the fresh text so the server's subsequent read is a cache
        // hit. Other formats are read lazily on demand.
        m_clipboards[id]->storeData(IClipboard::Format::Text, text);
      }

      result.changed[id] = typesChanged || textChanged || m_undelivered;
    }

    bool anyChanged = false;
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
      anyChanged = anyChanged || result.changed[id];
    }

    lock.lock();
    m_result = result;
    m_syncDone = true;
    if (anyChanged) {
      m_undelivered = true;
    }
  }
}

void WlClipboardCollection::initialize()
{
  if (!WlClipboard::isAvailable()) {
    LOG_WARN("wl-clipboard tools not found, clipboard functionality disabled");
    return;
  }

  // Create clipboard instances for each clipboard type
  m_clipboards.resize(kClipboardEnd);

  try {
    // Primary clipboard (selection)
    m_clipboards[kClipboardSelection] = std::make_unique<WlClipboard>(kClipboardSelection);

    // Standard clipboard
    m_clipboards[kClipboardClipboard] = std::make_unique<WlClipboard>(kClipboardClipboard);

    m_available = true;
    LOG_DEBUG("initialized Wayland clipboard support");

  } catch (const std::exception &e) {
    LOG_ERR("failed to initialize clipboard: %s", e.what());
    cleanup();
    m_available = false;
  }
}

void WlClipboardCollection::cleanup()
{
  {
    std::scoped_lock<std::mutex> lock(m_mutex);
    m_stop = true;
    m_syncRequested = true; // wake the worker
  }
  m_cv.notify_all();

  if (m_worker.joinable()) {
    m_worker.join();
  }

  m_clipboards.clear();
  m_available = false;
}

} // namespace deskflow
