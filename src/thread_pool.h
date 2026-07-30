#pragma once

#include <cstddef>
#include <pthread.h>
#include <vector>
#include <deque>

using namespace std;

// One unit of work for a worker thread.
struct Work {
  // Function to execute.
  void (*f)(void*) = nullptr;

  // Argument passed to the function.
  void* arg = nullptr;
};

// Thread pool.
struct ThreadPool {

  // Worker threads.
  vector<pthread_t> threads;

  // Queue of pending work.
  deque<Work> queue;

  // Protects the work queue.
  pthread_mutex_t mu;

  // Worker threads wait on this
  // when the queue is empty.
  pthread_cond_t not_empty;
};

// Create worker threads.
void thread_pool_init(ThreadPool* tp, size_t num_threads);

// Add a task to the queue.
void thread_pool_queue(ThreadPool* tp, void (*f)(void*), void* arg);