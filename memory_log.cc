// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/memory_log.h"

#include <stdio.h>

#include <ctime>
#include <iomanip>
#include <string>

#include <base/file_path.h>
#include <base/file_util.h>

#include "shill/logging.h"
#include "shill/shill_time.h"

namespace shill {

namespace {

// MemoryLog needs to be a 'leaky' singleton as it needs to survive to
// handle logging till the very end of the shill process. Making MemoryLog
// leaky is fine as it does not need to clean up or release any resource at
// destruction.
base::LazyInstance<MemoryLog>::Leaky g_memory_log = LAZY_INSTANCE_INITIALIZER;

// Can't get to these in base/logging.c.
const char *const kLogSeverityNames[logging::LOG_NUM_SEVERITIES] = {
  "INFO", "WARNING", "ERROR", "ERROR_REPORT", "FATAL"
};

// Size is one bigger to hold the null at the end.
const char kMemoryLogPrefix[] = "memlog: ";
const size_t kMemoryLogPrefixLength = arraysize(kMemoryLogPrefix) - 1;

logging::LogMessageHandlerFunction nested_message_handler = NULL;

bool LogMessageInterceptor(int severity,
                           const char *file,
                           int line,
                           size_t message_start,
                           const std::string &str) {
  if (message_start < str.size() &&
      strncmp(str.c_str() + message_start,
              kMemoryLogPrefix,
              kMemoryLogPrefixLength)) {
    // Stream this in to rewrite the prefix into memory log format.
    MemoryLogMessage(file,
                     line,
                     severity,
                     false).stream() << (str.c_str() + message_start);
  }

  // Signal that logging should continue processing this message.
  bool ret = false;
  if (nested_message_handler) {
    // If the nested handler wants to bypass logging, let it.
    ret = nested_message_handler(severity, file, line, message_start, str);
  }
  return ret;
}

}  // namespace


// static
MemoryLog *MemoryLog::GetInstance() {
  return g_memory_log.Pointer();
}

// static
void MemoryLog::InstallLogInterceptor() {
  nested_message_handler = logging::GetLogMessageHandler();
  logging::SetLogMessageHandler(LogMessageInterceptor);
}

// static
void MemoryLog::UninstallLogInterceptor() {
  if (logging::GetLogMessageHandler() == LogMessageInterceptor) {
    logging::SetLogMessageHandler(nested_message_handler);
  } else  {
    MLOG(ERROR) << "Tried to uninstall MemoryLog message handler, but found"
                << " another handler installed over top of MemoryLog's"
                << " handler.  Leaving all handlers installed.";
  }
}

MemoryLog::MemoryLog()
    : maximum_size_bytes_(kDefaultMaximumMemoryLogSizeInBytes),
      current_size_bytes_(0) { }

void MemoryLog::Append(const std::string &msg) {
  current_size_bytes_ += msg.size();
  log_.push_back(msg);
  ShrinkToTargetSize(maximum_size_bytes_);
}

void MemoryLog::Clear() {
  current_size_bytes_ = 0;
  log_.clear();
}

ssize_t MemoryLog::FlushToDisk(const std::string &file_path) {
  const FilePath file_name(file_path);
  FILE *f = file_util::OpenFile(file_name, "w");
  if (!f) {
    return -1;
  }

  file_util::ScopedFILE file_closer(f);
  ssize_t bytes_written = 0;
  std::deque<std::string>::iterator it;
  for (it = log_.begin(); it != log_.end(); it++) {
    bytes_written += fwrite(it->c_str(), 1, it->size(), f);
    if (ferror(f)) {
      return -1;
    }
  }

  return bytes_written;
}

void MemoryLog::SetMaximumSize(size_t size_in_bytes) {
  ShrinkToTargetSize(size_in_bytes);
  maximum_size_bytes_ = size_in_bytes;
}

void MemoryLog::ShrinkToTargetSize(size_t number_bytes) {
  while (current_size_bytes_ > number_bytes) {
    const std::string &front = log_.front();
    current_size_bytes_ -= front.size();
    log_.pop_front();
  }
}

MemoryLogMessage::MemoryLogMessage(const char *file,
                                   int line,
                                   logging::LogSeverity severity,
                                   bool propagate_down)
    : file_(file),
      line_(line),
      severity_(severity),
      propagate_down_(propagate_down),
      message_start_(0) {
  Init();
}

MemoryLogMessage::~MemoryLogMessage() {
  if (propagate_down_) {
    ::logging::LogMessage(file_, line_, severity_).stream()
        << &(stream_.str()[message_start_]);
  }
  stream_ << std::endl;
  MemoryLog::GetInstance()->Append(stream_.str());
}

// This owes heavily to the base/logging.cc:LogMessage implementation, but
// without as much customization.  Unfortunately, there isn't a good way to
// get into that code without drastically changing how base/logging works. It
// isn't exactly rocket science in any case.
void MemoryLogMessage::Init() {
  const char *filename = strrchr(file_, '/');
  if (!filename) {
    filename = file_;
  }

  // Just log a timestamp, number of ticks, severity, and a file name.
  struct timeval tv = {0};
  struct tm local_time = {0};
  Time::GetInstance()->GetTimeOfDay(&tv, NULL);
  localtime_r(&tv.tv_sec, &local_time);
  stream_ << std::setfill('0')
          << local_time.tm_year + 1900
          << '-' << std::setw(2) << local_time.tm_mon + 1
          << '-' << std::setw(2) << local_time.tm_mday
          << 'T' << std::setw(2) << local_time.tm_hour
          << ':' << std::setw(2) << local_time.tm_min
          << ':' << std::setw(2) << local_time.tm_sec
          << '.' << tv.tv_usec << ' ';

  if (severity_ >= 0)
    stream_ << kLogSeverityNames[severity_];
  else
    stream_ << "VERBOSE" << -severity_;
  stream_ << ":" << filename << "(" << line_ << ") ";

  message_start_ = stream_.tellp();

  // Let the memlog prefix hit the real logging.  This allows our interceptor
  // to discard messages that have already gotten into the memlog.
  stream_ << kMemoryLogPrefix;
}

}  // namespace shill
