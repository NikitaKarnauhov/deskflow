/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"
#include "deskflow/IClipboard.h"

#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>

#include <QObject>
#include <QString>
#include <QStringList>

//! Wayland clipboard implementation using wl-copy/wl-paste
/*!
This class implements clipboard functionality for Wayland environments
by using the wl-clipboard utilities (wl-copy and wl-paste).

Reads (get/has) are served from a cache that is refreshed by a
background sync worker (see WlClipboardCollection) whenever the pointer
leaves the screen. On compositors without ext-data-control
(e.g. GNOME/mutter) every wl-paste/wl-copy invocation briefly maps a
popup surface and steals focus, so reads must not happen on a timer or
in the screen-switch critical path.
*/
class WlClipboard : public QObject, public IClipboard
{
  Q_OBJECT
public:
  explicit WlClipboard(ClipboardID id);
  WlClipboard(WlClipboard const &) = delete;
  WlClipboard(WlClipboard &&) = delete;
  ~WlClipboard() override;

  WlClipboard &operator=(WlClipboard const &) = delete;
  WlClipboard &operator=(WlClipboard &&) = delete;

  //! Get clipboard ID
  ClipboardID getID() const;

  //! Check if wl-clipboard tools are available
  static bool isAvailable();

  // The following methods use plain POSIX process APIs (no Qt event
  // loop) and may be called from the background sync worker thread.

  //! Compare the current MIME type list with the last seen one, update
  //! the cache and report whether the clipboard changed
  bool checkChangePosix();

  //! Read a format from the clipboard into \p data
  bool readPosix(Format format, std::string &data);

  //! Store freshly read data in the cache
  void storeData(Format format, const std::string &data);

  //! Get the currently cached data for a format (empty if none)
  std::string getCachedData(Format format) const;

  // IClipboard overrides
  bool empty() override;
  void add(Format format, const std::string &data) override;
  bool open(Time time) const override;
  void close() const override;
  Time getTime() const override;
  bool has(Format format) const override;
  std::string get(Format format) const override;

private:
  //! Convert IClipboard format to MIME type
  QString formatToMimeType(Format format) const;

  //! Convert MIME type to IClipboard format
  Format mimeTypeToFormat(const QString &mimeType) const;

  //! Get available MIME types from clipboard (spawns wl-paste; call from
  //! the server thread only)
  QStringList getAvailableMimeTypes() const;

  //! Update the format availability cache from a MIME type list
  void applyTypes(const QStringList &types) const;

  //! Get current clipboard serial/timestamp
  Time getCurrentTime() const;

  //! Check if we own the clipboard
  bool isOwned() const;

  //! Update our ownership status
  void updateOwnership(bool owned);

  //! Invalidate cached clipboard data
  void invalidateCache();

  //! Run wl-copy with the given arguments, waiting for it to finish.
  //! Writes must be synchronous so a sequence of them applies in order.
  bool runWlCopy(const QStringList &args) const;

private:
  ClipboardID m_id;
  mutable bool m_open = false;
  mutable Time m_time = 0;
  mutable Time m_cachedTime = 0;
  mutable bool m_owned = false;

  // Clipboard data and type cache. The cache is long-lived: it is only
  // refreshed by the background sync worker (checkChangePosix/storeData)
  // or invalidated by local writes, never by open/close, so that the
  // server can read the clipboard without spawning wl-paste.
  mutable std::mutex m_cacheMutex;
  mutable bool m_cached = false;
  mutable std::string m_cachedData[static_cast<int>(Format::TotalFormats)];
  mutable bool m_cachedAvailable[static_cast<int>(Format::TotalFormats)];

  // Last seen MIME type list, used for change detection. Touched by the
  // constructor (server thread, before the worker starts) and by the
  // background worker afterwards.
  QStringList m_lastTypes;

  // Clipboard selection type (true = clipboard, false = primary)
  bool m_useClipboard;
};
