#include <cassert>
#include "thread_pool.h"

// Worker thread.
//
// Every worker repeatedly:
// 1. Waits until work is available.
// 2. Removes one job from the queue.
// 3. Executes the job.
static void* worker(void* arg) {
  ThreadPool* tp = static_cast<ThreadPool*>(arg);

  while (true) {
    // lock the queue
    pthread_mutex_lock(&tp->mu);

    // if there is no work sleep
    // pthread_cond_wait automatically unlocks the mutex while sleeping and locks it again when awakened

    while (tp->queue.empty()) {
      pthread_cond_wait(&tp->not_empty, &tp->mu);
    }

    // removes one job
    Work work = tp->queue.front();

    tp->queue.pop_front();

    // unlock the queue

    pthread_mutex_unlock(&tp->mu);

    // execute the job
    work.f(work.arg);
  }
  return nullptr;
}

// initialize the threadpool
void thread_pool_init(ThreadPool* tp, size_t num_threads) {
  assert(num_threads > 0);

  // init synchronization primitives
  int rv = pthread_mutex_init(&tp->mu, nullptr);

  assert(rv == 0);

  // create worker threads

  tp->threads.resize(num_threads);

  for (size_t i = 0; i < num_threads; i++) {
    rv = pthread_create(
      &tp->threads[i],
      nullptr,
      &worker,
      tp
    );

    assert(rv == 0);
  }
}

// add one job to the queue
void thread_pool_queue(ThreadPool* tp, void (*f)(void*), void* arg) {
  // lock the queue
  pthread_mutex_lock(&tp->mu);

  // add the job
  tp->queue.push_back(Work{f, arg});

  // wake one sleeping worker
  pthread_cond_signal(&tp->not_empty);

  // unlock the queue 
  pthread_mutex_unlock(&tp->mu);
}