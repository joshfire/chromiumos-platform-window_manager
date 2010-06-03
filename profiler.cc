// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "window_manager/profiler.h"

#include <cstring>
#include <stack>

#include <sys/time.h>
#include <stdio.h>

#include "base/file_path.h"
#include "base/file_util.h"
#include "base/hash_tables.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "base/time.h"
#include "window_manager/profiler_data.h"

namespace window_manager {

using base::TimeTicks;
using file_util::CloseFile;
using file_util::OpenFile;

//
// Static constants and functions
//
static inline int64_t Now() {
  return TimeTicks::Now().ToInternalValue();
}

//
// Marker definition
//
Marker::Marker(Profiler* profiler, const char* name)
    : profiler_(profiler),
      symbol_id_(0) {
  symbol_id_ = profiler->AddSymbol(name);
}

void Marker::Tap() {
  profiler_->AddSample(symbol_id_, Now(), profiler::MARK_FLAG_TAP);
}

void Marker::Begin() {
  profiler_->AddSample(symbol_id_, Now(), profiler::MARK_FLAG_BEGIN);
}

void Marker::End() {
  profiler_->AddSample(symbol_id_, Now(), profiler::MARK_FLAG_END);
}

//
// DynamicMarker definition
//
DynamicMarker::DynamicMarker()
    : profiler_(NULL) {
}

unsigned int DynamicMarker::GetSymbolId(const char* name) {
  if (symbol_table_.find(name) == symbol_table_.end()) {
    unsigned int symbol_id =  profiler_->AddSymbol(name);
    symbol_table_[name] = symbol_id;
    return symbol_id;
  } else {
    return symbol_table_[name];
  }
}

void DynamicMarker::Tap(const char* name) {
  profiler_->AddSample(GetSymbolId(name), Now(), profiler::MARK_FLAG_TAP);
}

void DynamicMarker::Begin(const char* name) {
  unsigned int symbol_id = GetSymbolId(name);
  recent_symbol_ids_.push(symbol_id);
  profiler_->AddSample(symbol_id, Now(), profiler::MARK_FLAG_BEGIN);
}

void DynamicMarker::End() {
  unsigned int symbol_id = recent_symbol_ids_.top();
  profiler_->AddSample(symbol_id, Now(), profiler::MARK_FLAG_END);
  recent_symbol_ids_.pop();
}

//
// Profiler definition
//
Profiler::Profiler()
    : profiler_writer_(NULL),
      status_(STATUS_STOP),
      max_num_symbols_(0),
      max_num_samples_(0),
      num_symbols_(0),
      num_samples_(0),
      symbols_(NULL),
      samples_(NULL) {
}

Profiler::~Profiler() {
  Stop();
}

void Profiler::Start(ProfilerWriter* profiler_writer,
                     unsigned int max_num_symbols,
                     unsigned int max_num_samples) {
  if (status_ != STATUS_STOP) {
    LOG(WARNING) << "the profiler has already started";
  } else if (profiler_writer == NULL) {
    LOG(WARNING) << "profiler writer cannot be NULL";
  } else if (max_num_symbols == 0 || max_num_samples == 0) {
    LOG(WARNING) << "the maximum # of symbols and samples must > 0";
  } else {
    profiler_writer_ = profiler_writer;
    max_num_symbols_ = max_num_symbols;
    max_num_samples_ = max_num_samples;
    status_ = STATUS_RUN;

    symbols_.reset(new profiler::Symbol[max_num_symbols_]);
    samples_.reset(new profiler::Sample[max_num_samples_]);

    memset(symbols_.get(), 0, sizeof(symbols_[0]) * max_num_symbols_);
    memset(samples_.get(), 0, sizeof(samples_[0]) * max_num_samples_);
  }
}

void Profiler::Pause() {
  if (status_ == STATUS_RUN) {
    status_ = STATUS_SUSPEND;
    Flush();
  }
}

void Profiler::Resume() {
  if (status_ == STATUS_SUSPEND) {
    status_ = STATUS_RUN;
  }
}

void Profiler::Stop() {
  if (status_ == STATUS_STOP) {
    LOG(WARNING) << "the profiler was not started";
    return;
  }
  Flush();
  max_num_symbols_ = 0;
  max_num_samples_ = 0;
  symbols_.reset(NULL);
  samples_.reset(NULL);
  status_ = STATUS_STOP;
}

void Profiler::Flush() {
  if (status_ != STATUS_STOP && num_samples_ != 0) {
    profiler_writer_->Update(*this);
    num_samples_ = 0;
  }
}

unsigned int Profiler::AddSymbol(const char* name) {
  if (status_ == STATUS_STOP || num_symbols_ == max_num_symbols_) {
    return max_num_symbols_;
  }
  strncpy(symbols_[num_symbols_].name, name,
          sizeof(symbols_[num_symbols_].name) - 1);

  return num_symbols_++;
}

void Profiler::AddSample(unsigned int symbol_id, int64_t time,
                         profiler::MarkFlag flag) {
  if (status_ != STATUS_RUN) {
    return;
  }
  if (symbol_id >= num_symbols_) {
    LOG(WARNING) << "symbol id provided exceeds number of symbols";
    return;
  }
  samples_[num_samples_].symbol_id = symbol_id;
  samples_[num_samples_].flag = flag;
  samples_[num_samples_].time = time;
  if (++num_samples_ == max_num_samples_) {
    Flush();
  }
}

//
// ProfilerWriter definition
//
ProfilerWriter::ProfilerWriter(FilePath file_path)
    : num_written_samples_(0),
      num_written_symbols_(0),
      file_path_(file_path) {
}

void ProfilerWriter::Update(const Profiler& profiler) {
  FILE* fp = NULL;
  if (num_written_samples_ == 0) {
    fp = OpenFile(file_path_, "wb");
  } else {
    fp = OpenFile(file_path_, "r+b");
  }

  if (fp == NULL) {
    LOG(WARNING) << "cannot open profile for writing";
    return;
  }

  num_written_samples_ += profiler.num_samples_;

  // overwrite header
  size_t result = 0;
  result = fwrite(&profiler.max_num_symbols_,
                  sizeof(profiler.max_num_symbols_), 1, fp);

  DCHECK_EQ(result, static_cast<size_t>(1));
  result = fwrite(&profiler.num_symbols_,
                  sizeof(profiler.num_symbols_), 1, fp);
  DCHECK_EQ(result, static_cast<size_t>(1));
  result = fwrite(&num_written_samples_, sizeof(num_written_samples_), 1, fp);
  DCHECK_EQ(result, static_cast<size_t>(1));

  if (num_written_symbols_ != profiler.num_symbols_) {
    // overwrite symbols
    result = fwrite(profiler.symbols_.get(), sizeof(profiler.symbols_[0]),
                    profiler.max_num_symbols_, fp);
    DCHECK_EQ(result, profiler.max_num_symbols_);
    num_written_symbols_ = profiler.num_symbols_;
  }

  // append samples
  fseek(fp, 0, SEEK_END);
  result = fwrite(profiler.samples_.get(), sizeof(profiler.samples_[0]),
                  profiler.num_samples_, fp);
  DCHECK_EQ(result, profiler.num_samples_);

  CloseFile(fp);
}

}  // namespace window_manager
