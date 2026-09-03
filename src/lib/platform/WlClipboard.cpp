/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/WlClipboard.h"

#include "base/Log.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <QDateTime>
#include <QProcess>
#include <QStandardPaths>

namespace {

inline static const auto s_copyApp = QStringLiteral("wl-copy");
inline static const auto s_pasteApp = QStringLiteral("wl-paste");

// wl-clipboard args
inline static const auto s_listTypes = QStringLiteral("--list-types");
inline static const auto s_isPrimary = QStringLiteral("--primary");
inline static const auto s_noNewLine = QStringLiteral("-n");
inline static const auto s_readType = QStringLiteral("-t%1");

// MIME types for different clipboard formats
inline static const auto s_mimeTypeText = QStringLiteral("text/plain;charset=utf-8");
inline static const auto s_mimeTypeHtml = QStringLiteral("text/html");
inline static const auto s_mimeTypeBmp = QStringLiteral("image/bmp");

// Additional HTML MIME type variants
const char *const s_mimeTypeHtmlUtf8 = "text/html;charset=UTF-8";
const char *const s_mimeTypeHtmlWindows = "HTML Format";

// Cache validity (milliseconds). The background sync worker refreshes the
// cache on every screen leave, so a long validity just means "the cache is
// good until the next leave" and keeps has()/get() from spawning wl-paste.
const int kCacheValidityMs = 10 * 60 * 1000;
const int kProcessTimeoutMs = 2000;

// Run a command to completion and capture its standard output.
//
// Uses only POSIX APIs (no Qt), so it is safe to call from any thread,
// in particular the background clipboard sync worker.
bool runToolCapture(const std::vector<std::string> &argv, int timeoutMs, std::string &out)
{
  int pipeFds[2];
  if (pipe(pipeFds) != 0) {
    return false;
  }

  int devnull = open("/dev/null", O_WRONLY);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
  if (devnull >= 0) {
    posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
  }
  posix_spawn_file_actions_destroy(&actions);
  if (devnull >= 0) {
    close(devnull);
  }

  std::vector<char *> args;
  args.reserve(argv.size() + 1);
  for (const auto &arg : argv) {
    args.push_back(const_cast<char *>(arg.c_str()));
  }
  args.push_back(nullptr);

  pid_t pid = -1;
  bool ok = false;
  if (posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, args.data(), environ) == 0) {
    close(pipeFds[1]);
    pipeFds[1] = -1;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      struct pollfd pfd;
      pfd.fd = pipeFds[0];
      pfd.events = POLLIN;
      pfd.revents = 0;

      const int pollResult = poll(&pfd, 1, 200);
      if (pollResult > 0) {
        char buffer[8192];
        const ssize_t bytes = read(pipeFds[0], buffer, sizeof(buffer));
        if (bytes > 0) {
          out.append(buffer, static_cast<size_t>(bytes));
        } else if (bytes == 0) {
          break; // EOF
        } else if (errno != EINTR) {
          break; // read error
        }
      } else if (pollResult < 0 && errno != EINTR) {
        break;
      }

      if (std::chrono::steady_clock::now() > deadline) {
        ::kill(pid, SIGKILL);
        break;
      }
    }
    ok = true;

    int status = 0;
    waitpid(pid, &status, 0);
  }

  if (pipeFds[1] >= 0) {
    close(pipeFds[1]);
  }
  close(pipeFds[0]);
  return ok;
}

} // namespace

WlClipboard::WlClipboard(ClipboardID id) : m_id(id), m_useClipboard(id == kClipboardClipboard)
{
  // Initialize cached data
  for (int i = 0; i < static_cast<int>(Format::TotalFormats); ++i) {
    m_cachedAvailable[i] = false;
  }

  // Seed the known type list so the first background check only reports
  // an actual change
  checkChangePosix();
}

WlClipboard::~WlClipboard() = default;

ClipboardID WlClipboard::getID() const
{
  return m_id;
}

bool WlClipboard::isAvailable()
{
  return !QStandardPaths::findExecutable(s_copyApp).isEmpty() && !QStandardPaths::findExecutable(s_pasteApp).isEmpty();
}

bool WlClipboard::checkChangePosix()
{
  std::string raw;
  std::vector<std::string> argv = {s_pasteApp.toStdString(), s_listTypes.toStdString()};
  if (!m_useClipboard) {
    argv.push_back(s_isPrimary.toStdString());
  }

  if (!runToolCapture(argv, kProcessTimeoutMs, raw)) {
    return false;
  }

  QStringList current;
  const static QChar newLine = QLatin1Char('\n');
  for (const auto &line : QString::fromLocal8Bit(raw).split(newLine)) {
    if (!line.isEmpty()) {
      current.append(line);
    }
  }

  bool changed;
  {
    std::scoped_lock<std::mutex> lock(m_cacheMutex);
    changed = (current != m_lastTypes);
    m_lastTypes = current;

    if (changed) {
      // Drop stale data, then re-derive format availability
      invalidateCache();
      updateOwnership(false);
    }

    // Refresh the availability cache so has()/get() don't have to spawn
    applyTypes(current);
    m_cached = true;
    m_cachedTime = getCurrentTime();
  }

  return changed;
}

bool WlClipboard::readPosix(Format format, std::string &data)
{
  const auto mimeType = formatToMimeType(format);
  if (mimeType.isEmpty()) {
    return false;
  }

  std::vector<std::string> argv = {
      s_pasteApp.toStdString(),
      s_noNewLine.toStdString(),
      s_readType.arg(mimeType).toStdString()
  };
  if (!m_useClipboard) {
    argv.push_back(s_isPrimary.toStdString());
  }

  return runToolCapture(argv, kProcessTimeoutMs, data);
}

void WlClipboard::storeData(Format format, const std::string &data)
{
  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  m_cachedData[static_cast<int>(format)] = data;
  m_cachedAvailable[static_cast<int>(format)] = !data.empty();
  m_cached = true;
  m_cachedTime = getCurrentTime();
}

std::string WlClipboard::getCachedData(Format format) const
{
  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  return m_cachedData[static_cast<int>(format)];
}

bool WlClipboard::empty()
{
  if (!m_open) {
    return false;
  }

  QStringList args = {s_noNewLine, ""};
  if (!m_useClipboard)
    args.prepend(s_isPrimary);

  if (!runWlCopy(args)) {
    return false;
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  updateOwnership(true);
  invalidateCache();
  return true;
}

void WlClipboard::add(Format format, const std::string &data)
{
  if (!m_open) {
    return;
  }

  if (format == Format::HTML) {
    return;
  }

  auto mimeType = formatToMimeType(format);
  if (mimeType.isEmpty()) {
    LOG_WARN("unsupported clipboard format: %d", format);
    return;
  }

  QStringList args = {s_noNewLine, s_readType.arg(mimeType), QString::fromStdString(data)};
  if (!m_useClipboard)
    args.prepend(s_isPrimary);

  if (!runWlCopy(args)) {
    return;
  }

  // We just wrote this data to the clipboard; keep the cache in sync so
  // subsequent reads don't need to spawn wl-paste.
  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  updateOwnership(true);
  m_cachedData[static_cast<int>(format)] = data;
  m_cachedAvailable[static_cast<int>(format)] = !data.empty();
  m_cached = true;
  m_cachedTime = getCurrentTime();
}

bool WlClipboard::runWlCopy(const QStringList &args) const
{
  // wl-copy must run synchronously: IClipboard::copy() performs a sequence
  // of writes (empty, then one per format) and the Wayland clipboard ends
  // up with whatever finished last. Async launches would race and could
  // leave the clipboard emptied or with stale content.
  QProcess cmd;
  cmd.setProgram(s_copyApp);
  cmd.setArguments(args);
  cmd.start();

  if (!cmd.waitForStarted(kProcessTimeoutMs)) {
    LOG_WARN("failed to start %s", s_copyApp.toStdString().c_str());
    return false;
  }
  if (!cmd.waitForFinished(kProcessTimeoutMs)) {
    cmd.kill();
    cmd.waitForFinished(500);
    LOG_WARN("%s timed out", s_copyApp.toStdString().c_str());
    return false;
  }
  if (cmd.exitStatus() != QProcess::NormalExit || cmd.exitCode() != 0) {
    LOG_WARN(
        "%s failed: %s", s_copyApp.toStdString().c_str(),
        cmd.readAllStandardError().toStdString().c_str()
    );
    return false;
  }
  return true;
}

bool WlClipboard::open(Time time) const
{
  if (m_open) {
    LOG_DEBUG("failed to open clipboard: already opened");
    return false;
  }

  m_open = true;
  m_time = time;

  return true;
}

void WlClipboard::close() const
{
  if (!m_open) {
    return;
  }

  LOG_DEBUG("close clipboard");

  // Note: the cache is deliberately NOT invalidated here. It is kept until
  // the background sync worker refreshes it or a local write invalidates
  // it, so the server can read the clipboard without spawning wl-paste.
  m_open = false;
}

IClipboard::Time WlClipboard::getTime() const
{
  return m_time;
}

bool WlClipboard::has(Format format) const
{
  if (!m_open) {
    return false;
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);

  // Check cache validity
  Time currentTime = getCurrentTime();
  if (m_cached && (currentTime - m_cachedTime) < kCacheValidityMs) {
    return m_cachedAvailable[static_cast<int>(format)];
  }

  // Cache expired: fall back to reading the type list. This spawns
  // wl-paste, so it should be rare (the sync worker refreshes the cache
  // on every screen leave).
  const auto availableTypes = getAvailableMimeTypes();
  if (availableTypes.isEmpty()) {
    // No types available - mark all formats as unavailable
    for (int i = 0; i < static_cast<int>(Format::TotalFormats); ++i) {
      m_cachedAvailable[i] = false;
      m_cachedData[i].clear();
    }
  } else {
    applyTypes(availableTypes);
  }

  m_cached = true;
  m_cachedTime = currentTime;

  return m_cachedAvailable[static_cast<int>(format)];
}

std::string WlClipboard::get(Format format) const
{
  if (!m_open) {
    return std::string();
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);

  // Return cached data if available and valid
  if (m_cached && m_cachedAvailable[static_cast<int>(format)] && !m_cachedData[static_cast<int>(format)].empty()) {
    return m_cachedData[static_cast<int>(format)];
  }

  auto mimeType = formatToMimeType(format);
  if (mimeType.isEmpty()) {
    return std::string();
  }

  QProcess cmd;
  cmd.setProgram(s_pasteApp);

  QStringList args = {s_noNewLine, s_readType.arg(mimeType)};
  if (!m_useClipboard)
    args.append(s_isPrimary);

  cmd.setArguments(args);
  cmd.start();
  cmd.waitForFinished();

  auto data = cmd.readAll().toStdString();

  // Update cache
  m_cachedData[static_cast<int>(format)] = data;
  m_cachedAvailable[static_cast<int>(format)] = !data.empty();
  m_cached = true;
  m_cachedTime = getCurrentTime();

  return data;
}

QString WlClipboard::formatToMimeType(Format format) const
{
  switch (format) {
    using enum IClipboard::Format;
  case Text:
    return s_mimeTypeText;
  case HTML:
    return s_mimeTypeHtml;
  case Bitmap:
    return s_mimeTypeBmp;
  default:
    return {};
  }
}

IClipboard::Format WlClipboard::mimeTypeToFormat(const QString &mimeType) const
{
  using enum IClipboard::Format;
  if (mimeType == s_mimeTypeText || mimeType == QStringLiteral("text/plain")) {
    return Text;
  }
  if (mimeType == s_mimeTypeHtml || mimeType == s_mimeTypeHtmlUtf8 || mimeType == s_mimeTypeHtmlWindows ||
      mimeType.contains("text/html")) {
    return HTML;
  }
  if (mimeType == s_mimeTypeBmp) {
    return Bitmap;
  }
  return Text; // Default fallback
}

QStringList WlClipboard::getAvailableMimeTypes() const
{
  QProcess cmd;
  cmd.setProgram(s_pasteApp);

  QStringList args = {s_listTypes};
  if (!m_useClipboard)
    args.append(s_isPrimary);

  cmd.setArguments(args);
  cmd.start();
  cmd.waitForFinished();

  const static QChar newLine = QLatin1Char('\n');
  return QString::fromLocal8Bit(cmd.readAll()).split(newLine);
}

void WlClipboard::applyTypes(const QStringList &types) const
{
  using enum IClipboard::Format;
  for (int i = 0; i < static_cast<int>(TotalFormats); ++i) {
    auto currentFormat = static_cast<Format>(i);
    const auto mimeType = formatToMimeType(currentFormat);

    m_cachedAvailable[i] = false;
    if (!mimeType.isEmpty()) {
      for (const auto &available : types) {
        if (available == mimeType ||
            (currentFormat == Text && available == QStringLiteral("text/plain")) ||
            (currentFormat == HTML && available.startsWith(QStringLiteral("text/html")))) {
          m_cachedAvailable[i] = true;
          break;
        }
      }
    }
  }
}

IClipboard::Time WlClipboard::getCurrentTime() const
{
  return static_cast<Time>(QDateTime::currentMSecsSinceEpoch());
}

bool WlClipboard::isOwned() const
{
  return m_owned;
}

void WlClipboard::updateOwnership(bool owned)
{
  m_owned = owned;
}

void WlClipboard::invalidateCache()
{
  m_cached = false;
  m_cachedTime = 0;
  for (int i = 0; i < static_cast<int>(Format::TotalFormats); ++i) {
    m_cachedData[i].clear();
    m_cachedAvailable[i] = false;
  }
}
