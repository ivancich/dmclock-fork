// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Copyright (C) 2016 Red Hat Inc.
 */


#pragma once


#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <deque>

#include "dmclock_recs.h"
#include "dmclock_server.h"

#include "test_simp_recs.h"


template<typename Q, typename CInfo,
	 typename ReqPm, typename RespPm,
	 typename AddInfo, typename Accum>
class TestServer {

  struct QueueItem {
    ClientId client;
    std::unique_ptr<TestRequest> request;
    AddInfo additional;

    QueueItem(const ClientId& _client,
	      std::unique_ptr<TestRequest>&& _request,
	      AddInfo _additional) :
      client(_client),
      request(std::move(_request)),
      additional(_additional)
    {
      // empty
    }
  }; // QueueItem

public:

  using ClientInfoFunc = std::function<CInfo(ClientId)>;

  using ClientRespFunc = std::function<void(ClientId,
					    const TestResponse&,
					    const RespPm&)>;

  using AccumFunc = std::function<void(Accum& accumulator,
				       const AddInfo& additional_info)>;

protected:

  const ServerId                 id;
  Q                              priority_queue;
  ClientRespFunc                 client_resp_f;
  int                            iops;
  size_t                         thread_pool_size;

  bool                           finishing;
  std::chrono::microseconds      op_time;

  std::mutex                     inner_queue_mtx;
  std::condition_variable        inner_queue_cv;
  std::deque<QueueItem>          inner_queue;

  std::thread*                   threads;

  using InnerQGuard = std::lock_guard<decltype(inner_queue_mtx)>;
  using Lock = std::unique_lock<std::mutex>;

  // data collection

  AccumFunc accum_f;
  Accum accumulator;

  uint32_t reservation_counter = 0;
  uint32_t proportion_counter = 0;

public:

  TestServer(ServerId _id,
	     int _iops,
	     size_t _thread_pool_size,
	     const ClientInfoFunc& _client_info_f,
	     const ClientRespFunc& _client_resp_f,
	     const AccumFunc& _accum_f,
	     bool use_soft_limit = false) :
    id(_id),
    priority_queue(_client_info_f,
		   std::bind(&TestServer::has_avail_thread, this),
		   std::bind(&TestServer::inner_post, this,
			     std::placeholders::_1,
			     std::placeholders::_2,
			     std::placeholders::_3),
		   use_soft_limit),
    client_resp_f(_client_resp_f),
    iops(_iops),
    thread_pool_size(_thread_pool_size),
    finishing(false),
    accum_f(_accum_f)
  {
    op_time =
      std::chrono::microseconds((int) (0.5 +
				       thread_pool_size * 1000000.0 / iops));
    std::chrono::milliseconds delay(1000);
    threads = new std::thread[thread_pool_size];
    for (size_t i = 0; i < thread_pool_size; ++i) {
      threads[i] = std::thread(&TestServer::run, this, delay);
    }
  }

  virtual ~TestServer() {
    Lock l(inner_queue_mtx);
    finishing = true;
    inner_queue_cv.notify_all();
    l.unlock();

    for (size_t i = 0; i < thread_pool_size; ++i) {
      threads[i].join();
    }

    delete[] threads;
  }

  void post(const TestRequest& request,
	    const ReqPm& req_params) {
    auto now = crimson::dmclock::get_time();
    priority_queue.add_request(request, req_params, now);
  }

  bool has_avail_thread() {
    InnerQGuard g(inner_queue_mtx);
    return inner_queue.size() <= thread_pool_size;
  }

  const Accum& get_accumulator() const { return accumulator; }

  const Q& get_priority_queue() const { return priority_queue; }

protected:

  void inner_post(const ClientId& client,
		  std::unique_ptr<TestRequest> request,
		  AddInfo additional) {
    Lock l(inner_queue_mtx);
    assert(!finishing);
    accum_f(accumulator, additional);
#if 0
    if (crimson::dmclock::PhaseType::reservation == phase) {
      ++reservation_counter;
    } else {
      ++proportion_counter;
    }
#endif
    inner_queue.emplace_back(QueueItem(client, std::move(request), additional));
    inner_queue_cv.notify_one();
  }
    

  void run(std::chrono::milliseconds check_period) {
    Lock l(inner_queue_mtx);
    while(true) {
      while(inner_queue.empty() && !finishing) {
	inner_queue_cv.wait_for(l, check_period);
      }
      if (!inner_queue.empty()) {
	auto& front = inner_queue.front();
	auto client = front.client;
	auto req = std::move(front.request);
	auto phase = front.additional;
	inner_queue.pop_front();

	l.unlock();

	// simulation operation by sleeping; then call function to
	// notify server of completion
	std::this_thread::sleep_for(op_time);

	TestResponse resp(req->epoch);
	sendResponse(client,
		     resp,
		     crimson::dmclock::RespParams<ServerId>(id, phase));

	priority_queue.request_completed();

	l.lock(); // in prep for next iteration of loop
      } else {
	break;
      }
    }
  }

  inline void sendResponse(const ClientId& client,
			   const TestResponse& resp,
			   const RespPm& resp_params) {
    client_resp_f(client, resp, resp_params);
  }
}; // class TestServer
