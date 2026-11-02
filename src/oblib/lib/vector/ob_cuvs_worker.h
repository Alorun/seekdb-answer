/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_LIB_VECTOR_OB_CUVS_WORKER_H_
#define OCEANBASE_LIB_VECTOR_OB_CUVS_WORKER_H_

#include <condition_variable>
#include <mutex>
#include <pthread.h>

namespace oceanbase {
namespace common {
namespace obvsag {
namespace detail {

// cuVS needs more stack than an OB worker has. Keep one large-stack worker
// alive, so synchronous calls do not recreate a pthread and GPU thread state.
class ObCuvsWorker {
public:
  ObCuvsWorker() = default;
  ObCuvsWorker(const ObCuvsWorker &) = delete;
  ObCuvsWorker &operator=(const ObCuvsWorker &) = delete;

  ~ObCuvsWorker()
  {
    std::lock_guard<std::mutex> submit(submit_mu_);
    {
      std::lock_guard<std::mutex> state(state_mu_);
      stop_ = true;
    }
    work_cv_.notify_one();
    if (started_) { pthread_join(thread_, nullptr); }
  }

  bool run(void *(*fn)(void *), void *arg)
  {
    if (fn == nullptr) { return false; }
    // Retain the caller's synchronous lifetime and serialize submissions.
    std::lock_guard<std::mutex> submit(submit_mu_);
    std::unique_lock<std::mutex> state(state_mu_);
    if (!started_) {
      pthread_attr_t attr;
      if (pthread_attr_init(&attr) != 0) { return false; }
      int rc = pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
      if (rc == 0) { rc = pthread_create(&thread_, &attr, entry, this); }
      pthread_attr_destroy(&attr);
      if (rc != 0) { return false; }
      started_ = true;
    }
    if (stop_) { return false; }
    arg_ = arg;
    fn_ = fn;
    work_cv_.notify_one();
    done_cv_.wait(state, [this] { return fn_ == nullptr; });
    return job_ok_;
  }

private:
  static void *entry(void *self)
  {
    static_cast<ObCuvsWorker *>(self)->loop();
    return nullptr;
  }

  void loop()
  {
    std::unique_lock<std::mutex> state(state_mu_);
    for (;;) {
      work_cv_.wait(state, [this] { return stop_ || fn_ != nullptr; });
      if (stop_) { break; }
      void *(*fn)(void *) = fn_;
      void *arg = arg_;
      state.unlock();
      bool ok = true;
      try { fn(arg); } catch (...) { ok = false; }
      state.lock();
      job_ok_ = ok;
      fn_ = nullptr;
      arg_ = nullptr;
      done_cv_.notify_one();
    }
  }

  std::mutex submit_mu_;
  std::mutex state_mu_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  pthread_t thread_{};
  void *(*fn_)(void *) = nullptr;
  void *arg_ = nullptr;
  bool started_ = false;
  bool stop_ = false;
  bool job_ok_ = false;
};

} // namespace detail
} // namespace obvsag
} // namespace common
} // namespace oceanbase
#endif
