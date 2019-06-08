/*****************************************************************************
 * ThreadPool.h
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <condition_variable>
#include <list>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include "IThreadPool.h"

namespace cloudstorage {

class ThreadPool : public IThreadPool {
 public:
  ThreadPool(uint32_t thread_count);
  ~ThreadPool() override;
  void schedule(const Task& f,
                const std::chrono::system_clock::time_point& when) override;

 private:
  void handleDelayedTasks();

  std::mutex mutex_;
  std::condition_variable worker_cv_;
  std::list<Task> tasks_;
  std::list<std::thread> workers_;
  std::set<std::pair<std::chrono::system_clock::time_point, Task>>
      delayed_tasks_;
  bool destroyed_;
  std::thread delayed_tasks_thread_;
};

}  // namespace cloudstorage

#endif  // THREADPOOL_H
