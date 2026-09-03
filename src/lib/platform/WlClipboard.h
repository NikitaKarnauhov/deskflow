/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ClipboardTypes.h"
#include "deskflow/IClipboard.h"

#include <atomic>
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

  //! Poll the current MIME type list once and report whether it changed
  //! since the last check. Called when the pointer leaves the screen.
  bool refreshTypes();

  //! Check if clipboard has changed
  bool hasChanged() const;

  //! Reset the changed flag and clear cache
  void resetChanged();

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

  //! Get available MIME types from clipboard
  QStringList getAvailableMimeTypes() const;

  //! Compare the current MIME type list with the last seen one and update
  //! the change flag (unless reportChange is false, for initial seeding)
  void updateTypes(bool reportChange);

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
  mutable std::atomic<bool> m_hasChanged = false;

  // Cached clipboard data
  mutable std::mutex m_cacheMutex;
  mutable bool m_cached = false;
  mutable std::string m_cachedData[static_cast<int>(Format::TotalFormats)];
  mutable bool m_cachedAvailable[static_cast<int>(Format::TotalFormats)];

  // Last seen MIME type list, used for on-demand change detection
  mutable QStringList m_lastTypes;

  // Clipboard selection type (true = clipboard, false = primary)
  bool m_useClipboard;
};
