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
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <QBuffer>
#include <QDate>
#include <QDateTime>
#include <QImage>
#include <QIODevice>
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
inline static const auto s_mimeTypePng = QStringLiteral("image/png");

// Additional HTML MIME type variants
const char *const s_mimeTypeHtmlUtf8 = "text/html;charset=UTF-8";
const char *const s_mimeTypeHtmlWindows = "HTML Format";

// Cache validity (milliseconds). The background sync worker refreshes the
// cache on every screen leave, so a long validity just means "the cache is
// good until the next leave" and keeps has()/get() from spawning wl-paste.
const int kCacheValidityMs = 10 * 60 * 1000;
const int kProcessTimeoutMs = 2000;

// posix_spawn(3) takes a path to the executable: unlike execvp(3) it
// does NOT search $PATH. The sync worker spawns wl-paste/wl-copy from a
// non-Qt thread, so resolve the tools to absolute paths once.
std::string resolveToolPath(const char *name)
{
  const char *pathVar = ::getenv("PATH");
  if (pathVar == nullptr) {
    return {};
  }
  std::string searchPath(pathVar);
  size_t begin = 0;
  for (;;) {
    const size_t end = searchPath.find(':', begin);
    const size_t len = (end == std::string::npos) ? std::string::npos : end - begin;
    const std::string dir = searchPath.substr(begin, len);
    if (!dir.empty()) {
      const std::string candidate = dir + '/' + name;
      if (::access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    if (end == std::string::npos) {
      break;
    }
    begin = end + 1;
  }
  return {};
}

const std::string &wlPastePath()
{
  static const std::string path = resolveToolPath("wl-paste");
  return path;
}

const std::string &wlCopyPath()
{
  static const std::string path = resolveToolPath("wl-copy");
  return path;
}

// Convert image data between formats via QImage (the input format is
// auto-detected from the data). Returns an empty string on failure.
// QImage/QBuffer need no event loop, so this is safe from any thread.
std::string convertImageData(const std::string &data, const char *targetFormat)
{
  if (data.empty()) {
    return {};
  }
  const QByteArray in(reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()));
  QImage image;
  if (!image.loadFromData(in)) {
    return {};
  }
  QBuffer buffer;
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, targetFormat)) {
    return {};
  }
  return std::string(buffer.data().constData(), buffer.data().size());
}

// Run a command to completion and capture its standard output.
//
// Uses vfork + dup2 + execv rather than posix_spawn: glibc's
// posix_spawn (clone3 with file actions) was observed to fail
// intermittently with EBADF on the dup2 action (glibc 2.43, Fedora 44),
// and the classic vfork path is fully deterministic, letting us check
// the result of every dup2.
//
// Uses only POSIX APIs (no Qt), so it is safe to call from any thread,
// in particular the background clipboard sync worker.
bool runToolCapture(const std::vector<std::string> &argv, int timeoutMs, std::string &out)
{
  int pipeFds[2];
  if (pipe(pipeFds) != 0) {
    LOG_WARN("pipe failed for %s: %s", argv[0].c_str(), strerror(errno));
    return false;
  }

  std::vector<char *> args;
  args.reserve(argv.size() + 1);
  for (const auto &arg : argv) {
    args.push_back(const_cast<char *>(arg.c_str()));
  }
  args.push_back(nullptr);

  // vfork suspends the calling thread until the child execs or exits,
  // so no other code in this thread can run in between. The child may
  // only perform async-signal-safe operations before exec.
  const pid_t pid = vfork();
  if (pid < 0) {
    LOG_WARN("vfork failed for %s: %s", argv[0].c_str(), strerror(errno));
    ::close(pipeFds[0]);
    ::close(pipeFds[1]);
    return false;
  }
  if (pid == 0) {
    // Child: stdout -> pipe, stderr -> /dev/null, then exec.
    bool ok = ::dup2(pipeFds[1], STDOUT_FILENO) == STDOUT_FILENO;
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (ok && devnull >= 0) {
      ok = ::dup2(devnull, STDERR_FILENO) == STDERR_FILENO;
      ::close(devnull);
    }
    ::close(pipeFds[0]);
    if (pipeFds[1] != STDOUT_FILENO) {
      ::close(pipeFds[1]);
    }
    if (!ok) {
      _exit(127);
    }
    ::execv(argv[0].c_str(), args.data());
    _exit(127);
  }

  ::close(pipeFds[1]);

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
      LOG_WARN("clipboard tool timed out: %s", argv[0].c_str());
      break;
    }
  }

  int status = 0;
  waitpid(pid, &status, 0);
  ::close(pipeFds[0]);

  if (!WIFEXITED(status) || WEXITSTATUS(status) == 127) {
    // 127 means the child bailed before exec (failed dup2 or missing
    // binary); it produced no usable output.
    LOG_WARN(
        "%s failed to start (status %d)", argv[0].c_str(),
        WIFEXITED(status) ? WEXITSTATUS(status) : (-WTERMSIG(status))
    );
    return false;
  }
  return true;
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

WlClipboard::PosixCheckResult WlClipboard::checkChangePosix()
{
  const std::string &pastePath = wlPastePath();
  if (pastePath.empty()) {
    LOG_WARN("wl-paste not found in PATH");
    return PosixCheckResult::Failed;
  }

  std::string raw;
  std::vector<std::string> argv = {pastePath, s_listTypes.toStdString()};
  if (!m_useClipboard) {
    argv.push_back(s_isPrimary.toStdString());
  }

  if (!runToolCapture(argv, kProcessTimeoutMs, raw)) {
    LOG_WARN("failed to read types of clipboard %d via wl-paste", m_id);
    return PosixCheckResult::Failed;
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

  return changed ? PosixCheckResult::Changed : PosixCheckResult::Unchanged;
}

bool WlClipboard::readPosix(Format format, std::string &data)
{
  const auto mimeType = formatToMimeType(format);
  if (mimeType.isEmpty()) {
    return false;
  }

  const std::string &pastePath = wlPastePath();
  if (pastePath.empty()) {
    LOG_WARN("wl-paste not found in PATH");
    return false;
  }

  std::vector<std::string> argv = {
      pastePath,
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

  QStringList args = {s_noNewLine, s_readType.arg(s_mimeTypeText)};
  if (!m_useClipboard)
    args.prepend(s_isPrimary);

  if (!runWlCopy(args, "")) {
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
    // wl-copy can only set one MIME type per invocation and the
    // clipboard already carries the text; HTML is skipped.
    return;
  }

  QString mimeType = formatToMimeType(format);
  std::string payload = data;
  if (format == Format::Bitmap) {
    // Wayland apps request image/png. Convert the protocol's BMP to
    // PNG, falling back to the raw BMP if conversion fails.
    const std::string png = convertImageData(data, "PNG");
    if (!png.empty()) {
      payload = png;
      mimeType = s_mimeTypePng;
    }
  }
  if (mimeType.isEmpty()) {
    LOG_WARN("unsupported clipboard format: %d", format);
    return;
  }

  QStringList args = {s_noNewLine, s_readType.arg(mimeType)};
  if (!m_useClipboard)
    args.prepend(s_isPrimary);

  if (!runWlCopy(args, payload)) {
    return;
  }

  // We just wrote this data to the clipboard; keep the cache in sync so
  // subsequent reads don't need to spawn wl-paste.
  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  updateOwnership(true);
  m_cachedData[static_cast<int>(format)] = data;
  m_cachedAvailable[static_cast<int>(format)] = !data.empty();
  if (format == Format::Bitmap) {
    m_imageData = payload;
    m_imageMimeType = mimeType.toStdString();
  }
  m_cached = true;
  m_cachedTime = getCurrentTime();
}

bool WlClipboard::runWlCopy(const QStringList &args, const std::string &data) const
{
  // wl-copy must run synchronously: IClipboard::copy() performs a sequence
  // of writes (empty, then one per format) and the Wayland clipboard ends
  // up with whatever finished last. Async launches would race and could
  // leave the clipboard emptied or with stale content.
  QProcess cmd;
  // Prefer the absolute path; QProcess would search PATH for a bare name,
  // but be explicit for consistency with the posix spawn path.
  const std::string &copyPath = wlCopyPath();
  cmd.setProgram(copyPath.empty() ? s_copyApp : QString::fromStdString(copyPath));
  cmd.setArguments(args);
  cmd.start();

  if (!cmd.waitForStarted(kProcessTimeoutMs)) {
    LOG_WARN("failed to start %s", s_copyApp.toStdString().c_str());
    return false;
  }
  // The data is fed on standard input: it may be binary (images), which
  // argv transfer would mangle (non-UTF-8) and which could exceed
  // argument length limits.
  cmd.write(QByteArray(data.data(), static_cast<int>(data.size())));
  cmd.closeWriteChannel();
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

  if (format == Format::Bitmap) {
    // No image type offered, or the worker hasn't read it yet: fall back
    // to converting whatever original image data we have.
    if (!m_imageData.empty()) {
      const std::string bmp =
          (m_imageMimeType == s_mimeTypeBmp.toStdString()) ? m_imageData
                                                           : convertImageData(m_imageData, "BMP");
      if (!bmp.empty()) {
        m_cachedData[static_cast<int>(format)] = bmp;
        m_cachedAvailable[static_cast<int>(format)] = true;
        m_cached = true;
        m_cachedTime = getCurrentTime();
        return bmp;
      }
    }
    return std::string();
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
            (currentFormat == HTML && available.startsWith(QStringLiteral("text/html"))) ||
            // Wayland apps offer image/png & co., never image/bmp; the
            // data is converted via QImage, so any image type counts.
            (currentFormat == Bitmap && available.startsWith(QStringLiteral("image/")))) {
          m_cachedAvailable[i] = true;
          break;
        }
      }
    }
  }
}

std::string WlClipboard::pickImageMimeType() const
{
  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  for (const QString &preferred : {s_mimeTypePng, s_mimeTypeBmp}) {
    if (m_lastTypes.contains(preferred)) {
      return preferred.toStdString();
    }
  }
  for (const QString &type : m_lastTypes) {
    if (type.startsWith(QStringLiteral("image/"))) {
      return type.toStdString();
    }
  }
  return {};
}

WlClipboard::PosixCheckResult WlClipboard::syncImagePosix()
{
  const std::string mimeType = pickImageMimeType();
  if (mimeType.empty()) {
    // No image type offered: drop any cached image. The types check
    // already reports the image appearing/disappearing, so nothing else
    // to do here.
    {
      std::scoped_lock<std::mutex> lock(m_cacheMutex);
      m_imageData.clear();
      m_imageMimeType.clear();
    }
    return PosixCheckResult::Unchanged;
  }

  const std::string &pastePath = wlPastePath();
  if (pastePath.empty()) {
    LOG_WARN("wl-paste not found in PATH");
    return PosixCheckResult::Failed;
  }

  std::vector<std::string> argv = {
      pastePath,
      s_noNewLine.toStdString(),
      s_readType.arg(QString::fromStdString(mimeType)).toStdString()
  };
  if (!m_useClipboard) {
    argv.push_back(s_isPrimary.toStdString());
  }

  std::string data;
  if (!runToolCapture(argv, kProcessTimeoutMs, data)) {
    LOG_WARN("failed to read image (%s) of clipboard %d via wl-paste", mimeType.c_str(), m_id);
    return PosixCheckResult::Failed;
  }

  bool changed;
  {
    std::scoped_lock<std::mutex> lock(m_cacheMutex);
    changed = (data != m_imageData);
  }
  if (changed) {
    storeImage(mimeType, data);
  }
  return changed ? PosixCheckResult::Changed : PosixCheckResult::Unchanged;
}

void WlClipboard::storeImage(const std::string &mimeType, const std::string &data)
{
  // Keep the original data so the image can be written back to the
  // Wayland clipboard with a type Wayland apps request (image/png), and
  // convert to BMP for the deskflow protocol.
  std::string bmp = data;
  if (mimeType != s_mimeTypeBmp.toStdString()) {
    bmp = convertImageData(data, "BMP");
  }

  std::scoped_lock<std::mutex> lock(m_cacheMutex);
  m_imageData = data;
  m_imageMimeType = mimeType;
  m_cachedData[static_cast<int>(IClipboard::Format::Bitmap)] = bmp;
  m_cachedAvailable[static_cast<int>(IClipboard::Format::Bitmap)] = !bmp.empty();
  m_cached = true;
  m_cachedTime = getCurrentTime();
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
  m_imageData.clear();
  m_imageMimeType.clear();
}
