// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef WINDOW_MANAGER_PROFILER_H_
#define WINDOW_MANAGER_PROFILER_H_

#include <string>
#include <stack>
#include "base/file_path.h"
#include "base/hash_tables.h"
#include "base/memory/scoped_ptr.h"
#include "base/memory/singleton.h"
#include "window_manager/profiler_data.h"

//
// IMPORTANT NOTE:
// Single instances of the Profiler and DynamicMarker objects are managed by
// base::Singleton.  For Singleton to work properly, an instance of
// base::AtExitManager must be created.  Check base/at_exit.h or
// base/memory/singleton.h for more details.
//
// PROFILE_BUILD needs to be defined for the profile code to be included.
//
// Profiler::Start and Profiler::Stop are used to signal start and stop of the
// profiler, both should be called only once throughout the program.
// Profiler::Start should be called before any of the other PROFILER_* macros
// are used.  Profiler::Stop is called at the very end, but it is optional since
// the destructor will call it again.  PROFILER_PAUSE / PROFILER_RESUME can be
// used to pause/resume the profiler once it is started.
//
// PROFILER_MARKER_BEGIN and PROFILER_MARKER_END are used in conjunction to
// mark a region for timing.  PROFILER_MARKER_END must match with a
// PROFILER_MARKER_BEGIN with the same marker name in the same scope.
// PROFILER_MARKER_CONTINUE can be used within the timed region if extra
// samples are needed with the same marker name.
//
// Usage {
//   PROFILER_MARKER_BEGIN(_timed_section_);
//   ...
//   PROFILER_MARKER_CONTINUE(_timed_section_);
//   ...
//   PROFILER_MARKER_END(_timed_section_);
// }
//
// PROFILER_MARKER_TAP is used to mark a single location for timing.  It is
// used independent of PROFILER_MARKER_BEGIN and PROFILER_MARKER_END.  The
// marker name used cannot match any other marker name within the same scope.
//
// Usage {
//   ...
//   PROFILER_MARKER_TAP(_time_point_1_);
//   ...
//   PROFILER_MARKER_TAP(_time_point_2_);
//   ...
// }
//
// PROFILER_DYNAMIC_MARKER_* macros are used to create dynamic markers.  They
// are not statically compiled into the program, and can be created while the
// program is running.  Use static markers whenever possible, they are
// generally faster.  Call set_profiler on the DynamicMarker instance before
// using the PROFILER_DYNAMIC_MARKER_* macros.
//

#if defined(PROFILE_BUILD)

#define PROFILER_PAUSE() \
  Singleton<window_manager::Profiler>()->Pause()

#define PROFILER_RESUME() \
  Singleton<window_manager::Profiler>()->Resume()

#define PROFILER_FLUSH() \
  Singleton<window_manager::Profiler>()->Flush()

#define PROFILER_MARKER_TAP(name) \
  do { \
    static window_manager::Marker _marker_##name( \
        Singleton<window_manager::Profiler>::get(), #name); \
    _marker_##name.Tap(); \
  } while (false)

#define PROFILER_MARKER_BEGIN(name) \
  static window_manager::Marker _marker_##name( \
      Singleton<window_manager::Profiler>::get(), #name); \
  _marker_##name.Begin()

#define PROFILER_MARKER_CONTINUE(name) \
  _marker_##name.Tap()

#define PROFILER_MARKER_END(name) \
  _marker_##name.End()

#define PROFILER_DYNAMIC_MARKER_TAP(name) \
  Singleton<window_manager::DynamicMarker>()->Tap(name)

#define PROFILER_DYNAMIC_MARKER_BEGIN(name) \
  Singleton<window_manager::DynamicMarker>()->Begin(name)

#define PROFILER_DYNAMIC_MARKER_END() \
  Singleton<window_manager::DynamicMarker>()->End()

#else

#define PROFILER_PAUSE() \
  do {} while (false)
#define PROFILER_RESUME() \
  do {} while (false)
#define PROFILER_FLUSH() \
  do {} while (false)
#define PROFILER_MARKER_TAP(name) \
  do {} while (false)
#define PROFILER_MARKER_BEGIN(name) \
  do {} while (false)
#define PROFILER_MARKER_CONTINUE(name) \
  do {} while (false)
#define PROFILER_MARKER_END(name) \
  do {} while (false)
#define PROFILER_DYNAMIC_MARKER_TAP(name) \
  do {} while (false)
#define PROFILER_DYNAMIC_MARKER_BEGIN(name) \
  do {} while (false)
#define PROFILER_DYNAMIC_MARKER_END() \
  do {} while (false)
#endif

namespace window_manager {

class DynamicMarker;
class Marker;
class Profiler;
class ProfilerWriter;
class ScopedMarker;

class Marker {
 public:
  Marker(Profiler* profiler, const char* name);
  void Tap();
  void Begin();
  void End();

 private:
  Profiler* profiler_;
  unsigned int symbol_id_;

  DISALLOW_COPY_AND_ASSIGN(Marker);
};

class DynamicMarker {
 public:
  void Tap(const char* name);
  void Begin(const char* name);
  void End();

  void set_profiler(Profiler* profiler) {
    profiler_ = profiler;
  }

 private:
  friend struct DefaultSingletonTraits<DynamicMarker>;

  DynamicMarker();
  unsigned int GetSymbolId(const char* name);

  Profiler* profiler_;
  std::stack<unsigned int> recent_symbol_ids_;
  base::hash_map<std::string, unsigned int> symbol_table_;

  DISALLOW_COPY_AND_ASSIGN(DynamicMarker);
};

class ScopedMarker {
 public:
  explicit ScopedMarker(const char* name) {
    PROFILER_DYNAMIC_MARKER_BEGIN(name);
  }

  ~ScopedMarker() {
    PROFILER_DYNAMIC_MARKER_END();
  }
 private:
  DISALLOW_COPY_AND_ASSIGN(ScopedMarker);
};

class Profiler {
 public:
  enum ProfilerStatus {
    STATUS_STOP = 0,
    STATUS_SUSPEND,
    STATUS_RUN
  };

  void Start(ProfilerWriter* profiler_writer, unsigned int max_num_symbols,
             unsigned int max_num_samples);
  void Pause();
  void Resume();
  void Stop();

  void Flush();
  unsigned int AddSymbol(const char* name);
  void AddSample(unsigned int symbol_id, int64_t time, profiler::MarkFlag flag);

  ProfilerStatus status() const {
    return status_;
  }

 private:
  friend struct DefaultSingletonTraits<Profiler>;
  friend class ProfilerWriter;

  Profiler();
  ~Profiler();

  ProfilerWriter* profiler_writer_;
  ProfilerStatus status_;
  unsigned int max_num_symbols_;
  unsigned int max_num_samples_;
  unsigned int num_symbols_;
  unsigned int num_samples_;
  scoped_array<profiler::Symbol> symbols_;
  scoped_array<profiler::Sample> samples_;

  DISALLOW_COPY_AND_ASSIGN(Profiler);
};

class ProfilerWriter {
 public:
  explicit ProfilerWriter(FilePath file_path);
  void Update(const Profiler& profiler);

 private:
  unsigned int num_written_samples_;
  unsigned int num_written_symbols_;
  FilePath file_path_;

  DISALLOW_COPY_AND_ASSIGN(ProfilerWriter);
};

}  // namespace window_manager

#endif  // WINDOW_MANAGER_PROFILER_H_
