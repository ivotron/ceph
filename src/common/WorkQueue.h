// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef CEPH_WORKQUEUE_H
#define CEPH_WORKQUEUE_H

#include "Mutex.h"
#include "Cond.h"
#include "Thread.h"
#include "common/config_obs.h"

class CephContext;

class ThreadPool : public md_config_obs_t {
  CephContext *cct;
  string name;
  string lockname;
  Mutex _lock;
  Cond _cond;
  bool _stop;
  int _pause;
  int _draining;
  Cond _wait_cond;

  struct WorkQueue_ {
    string name;
    time_t timeout_interval, suicide_interval;
    WorkQueue_(string n, time_t ti, time_t sti)
      : name(n), timeout_interval(ti), suicide_interval(sti)
    { }
    virtual ~WorkQueue_() {}
    virtual void _clear() = 0;
    virtual bool _empty() = 0;
    virtual void *_void_dequeue() = 0;
    virtual void _void_process(void *) = 0;
    virtual void _void_process_finish(void *) = 0;
  };

  // track thread pool size changes
  unsigned _num_threads;
  string _thread_num_option;
  const char **_conf_keys;

  const char **get_tracked_conf_keys() const {
    return _conf_keys;
  }
  void handle_conf_change(const struct md_config_t *conf,
			  const std::set <std::string> &changed);

public:
  template<class T>
  class BatchWorkQueue : public WorkQueue_ {
    ThreadPool *pool;

    virtual bool _enqueue(T *) = 0;
    virtual void _dequeue(T *) = 0;
    virtual void _dequeue(list<T*> *) = 0;
    virtual void _process(const list<T*> &) = 0;
    virtual void _process_finish(const list<T*> &) {}

    void *_void_dequeue() {
      list<T*> *out(new list<T*>);
      _dequeue(out);
      if (out->size()) {
	return (void *)out;
      } else {
	delete out;
	return 0;
      }
    }
    void _void_process(void *p) {
      _process(*((list<T*>*)p));
    }
    void _void_process_finish(void *p) {
      _process_finish(*(list<T*>*)p);
      delete (list<T*> *)p;
    }

  public:
    BatchWorkQueue(string n, time_t ti, time_t sti, ThreadPool* p)
      : WorkQueue_(n, ti, sti), pool(p) {
      pool->add_work_queue(this);
    }
    ~BatchWorkQueue() {
      pool->remove_work_queue(this);
    }

    bool queue(T *item) {
      pool->_lock.Lock();
      bool r = _enqueue(item);
      pool->_cond.SignalOne();
      pool->_lock.Unlock();
      return r;
    }
    void dequeue(T *item) {
      pool->_lock.Lock();
      _dequeue(item);
      pool->_lock.Unlock();
    }
    void clear() {
      pool->_lock.Lock();
      _clear();
      pool->_lock.Unlock();
    }

    void lock() {
      pool->lock();
    }
    void unlock() {
      pool->unlock();
    }
    void wake() {
      pool->wake();
    }
    void _wake() {
      pool->_wake();
    }
    void drain() {
      pool->drain(this);
    }

  };
  template<class T>
  class WorkQueue : public WorkQueue_ {
    ThreadPool *pool;
    
    virtual bool _enqueue(T *) = 0;
    virtual void _dequeue(T *) = 0;
    virtual T *_dequeue() = 0;
    virtual void _process(T *) = 0;
    virtual void _process_finish(T *) {}
    
    void *_void_dequeue() {
      return (void *)_dequeue();
    }
    void _void_process(void *p) {
      _process((T *)p);
    }
    void _void_process_finish(void *p) {
      _process_finish((T *)p);
    }

  public:
    WorkQueue(string n, time_t ti, time_t sti, ThreadPool* p) : WorkQueue_(n, ti, sti), pool(p) {
      pool->add_work_queue(this);
    }
    ~WorkQueue() {
      pool->remove_work_queue(this);
    }
    
    bool queue(T *item) {
      pool->_lock.Lock();
      bool r = _enqueue(item);
      pool->_cond.SignalOne();
      pool->_lock.Unlock();
      return r;
    }
    void dequeue(T *item) {
      pool->_lock.Lock();
      _dequeue(item);
      pool->_lock.Unlock();
    }
    void clear() {
      pool->_lock.Lock();
      _clear();
      pool->_lock.Unlock();
    }

    void lock() {
      pool->lock();
    }
    void unlock() {
      pool->unlock();
    }
    /// wake up the thread pool (without lock held)
    void wake() {
      pool->wake();
    }
    /// wake up the thread pool (with lock already held)
    void _wake() {
      pool->_wake();
    }
    void drain() {
      pool->drain(this);
    }

  };

private:
  vector<WorkQueue_*> work_queues;
  int last_work_queue;
 

  // threads
  struct WorkThread : public Thread {
    ThreadPool *pool;
    WorkThread(ThreadPool *p) : pool(p) {}
    void *entry() {
      pool->worker(this);
      return 0;
    }
  };
  
  set<WorkThread*> _threads;
  list<WorkThread*> _old_threads;  ///< need to be joined
  int processing;

  void start_threads();
  void join_old_threads();
  void worker(WorkThread *wt);

public:
  ThreadPool(CephContext *cct_, string nm, int n, const char *option = NULL);
  ~ThreadPool();

  /// return number of threads currently running
  int get_num_threads() {
    Mutex::Locker l(_lock);
    return _num_threads;
  }
  
  /// assign a work queue to this thread pool
  void add_work_queue(WorkQueue_* wq) {
    work_queues.push_back(wq);
  }
  /// remove a work queue from this thread pool
  void remove_work_queue(WorkQueue_* wq) {
    unsigned i = 0;
    while (work_queues[i] != wq)
      i++;
    for (i++; i < work_queues.size(); i++) 
      work_queues[i-1] = work_queues[i];
    assert(i == work_queues.size());
    work_queues.resize(i-1);
  }

  /// take thread pool lock
  void lock() {
    _lock.Lock();
  }
  /// release thread pool lock
  void unlock() {
    _lock.Unlock();
  }

  /// wait for a kick on this thread pool
  void wait(Cond &c) {
    c.Wait(_lock);
  }

  /// wake up a waiter (with lock already held)
  void _wake() {
    _cond.Signal();
  }
  /// wake up a waiter (without lock held)
  void wake() {
    Mutex::Locker l(_lock);
    _cond.Signal();
  }

  /// start thread pool thread
  void start();
  /// stop thread pool thread
  void stop(bool clear_after=true);
  /// pause thread pool (if it not already paused)
  void pause();
  /// pause initiation of new work
  void pause_new();
  /// resume work in thread pool.  must match each pause() call 1:1 to resume.
  void unpause();
  /// wait for all work to complete
  void drain(WorkQueue_* wq = 0);
};



#endif
