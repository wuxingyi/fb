#include "etcdapi.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

#include <grpc++/channel.h>
#include <grpc++/grpc++.h>

namespace etcdapi
{
  std::shared_ptr<grpc::Channel> make_channel(
      const std::string &address,
      const std::shared_ptr<grpc::ChannelCredentials> &channelCredentials)
  {
    const std::string substr("://");
    const auto i = address.find(substr);
    if (i == std::string::npos)
    {
      return grpc::CreateChannel(address, channelCredentials);
    }
    const std::string stripped_address = address.substr(i + substr.length());
    return grpc::CreateChannel(stripped_address, channelCredentials);
  }

  v3_client_t::v3_client_t(const std::string &address)
  {
    this->_channel = make_channel(address, grpc::InsecureChannelCredentials());
    this->_kvStub = etcdserverpb::KV::NewStub(_channel);
    this->_leaseStub = etcdserverpb::Lease::NewStub(_channel);
    this->_watcher = std::unique_ptr<watcher>(new watcher(std::move(_channel)));
  }

  v3_client_t::~v3_client_t() { _watcher->Stop(); }

  status_code_e v3_client_t::kv_put(const std::string &key,
                                    const std::string &value,
                                    leaseid_t lid)
  {
    etcdserverpb::PutRequest pr;
    pr.set_key(key);
    pr.set_value(value);
    pr.set_lease(lid);
    pr.set_prev_kv(false);

    etcdserverpb::PutResponse presponse;

    grpc::ClientContext context;
    grpc::Status status = _kvStub->Put(&context, pr, &presponse);

    return (status_code_e)status.error_code();
  }

  lease_grant_response_t v3_client_t::lease_grant(
      std::chrono::seconds ttl)
  {
    etcdserverpb::LeaseGrantRequest req;
    req.set_ttl(ttl.count());
    etcdserverpb::LeaseGrantResponse res;

    grpc::ClientContext context;
    grpc::Status status = _leaseStub->LeaseGrant(&context, req, &res);

    return lease_grant_response_t(
        (status_code_e)status.error_code(), res.id(),
        std::chrono::seconds(res.ttl()));
  }

  status_code_e v3_client_t::lease_revoke(
      leaseid_t lid)
  {
    etcdserverpb::LeaseRevokeRequest req;
    req.set_id(lid);

    etcdserverpb::LeaseRevokeResponse res;

    grpc::ClientContext context;
    grpc::Status status = _leaseStub->LeaseRevoke(&context, req, &res);

    return (status_code_e)status.error_code();
  }

  kv_range_response_t v3_client_t::kv_get(
      const std::string &key)
  {
    etcdserverpb::RangeRequest req;
    req.set_key(key);

    etcdserverpb::RangeResponse res;

    grpc::ClientContext context;
    grpc::Status status = _kvStub->Range(&context, req, &res);

    auto status_code = (status_code_e)status.error_code();

    if (!status.ok())
    {
      kv_range_response_t ret(status_code);
      return ret;
    }

    if (res.count() == 0)
    {
      return kv_range_response_t(status_code_e::NOT_FOUND);
    }

    return kv_range_response_t(status_code, res.kvs(0).value());
  }

  status_code_e v3_client_t::kv_delete(const std::string &key)
  {
    etcdserverpb::DeleteRangeRequest req;
    req.set_key(key);
    req.set_prev_kv(false);

    etcdserverpb::DeleteRangeResponse res;

    grpc::ClientContext context;
    grpc::Status status = _kvStub->DeleteRange(&context, req, &res);

    return (status_code_e)status.error_code();
  }

  kv_list_response_t v3_client_t::kv_list(
      const std::string &keyPrefix)
  {
    etcdserverpb::RangeRequest req;
    req.set_key(keyPrefix);

    etcdserverpb::RangeResponse res;

    grpc::ClientContext context;
    grpc::Status status = _kvStub->Range(&context, req, &res);

    auto status_code = (status_code_e)status.error_code();

    if (!status.ok())
    {
      kv_list_response_t ret(status_code);
      return ret;
    }

    kv_list_response_t::kv_pairs kvs;
    kvs.reserve(res.count());

    for (int i = 0; i < res.count(); ++i)
    {
      const auto &kv = res.kvs(i);
      kvs.push_back(std::make_pair(kv.key(), kv.value()));
    }

    return kv_list_response_t(status_code, std::move(kvs));
  }

  bool v3_client_t::StartWatch() { return _watcher->Start(); }

  void v3_client_t::StopWatch() { _watcher->Stop(); }

  bool v3_client_t::AddWatchPrefix(
      const std::string &prefix, OnKeyAddedFunc onKeyAdded,
      OnKeyRemovedFunc onKeyRemoved)
  {
    return _watcher->add_prefix(prefix, std::move(onKeyAdded),
                                std::move(onKeyRemoved));
  }

  bool v3_client_t::RemoveWatchPrefix(const std::string &prefix)
  {
    return _watcher->remove_prefix(prefix);
  }

  std::shared_ptr<v3_client_t> create_v3_client(
      const std::string &address)
  {
    return std::make_shared<v3_client_t>(address);
  }

  void watcher::Stop()
  {
    using namespace std::chrono;
    if (!_thread_running.load())
    {
      return;
    }

    _watch_context.TryCancel();

    _watch_completion_queue.Shutdown();
    if (_watch_thread.joinable())
    {
      _watch_thread.join();
    }
    this->_watchStub = nullptr;
  }

  watch_data *watcher::find_created_watch_withlock(int64_t watch_id)
  {
    std::lock_guard<decltype(_watch_thread_mutex)> lock(_watch_thread_mutex);
    auto it = std::find_if(_createdWatches.begin(), _createdWatches.end(),
                           [=](const watch_data *data) -> bool
                           {
                             return data->watch_id == watch_id;
                           });
    if (it == _createdWatches.end())
    {
      return nullptr;
    }
    else
    {
      return *it;
    }
  }

void watcher::Thread_Start() {
  using namespace std::chrono;

  for (;;) {
    void* tag;
    bool ok;
    if (!_watch_completion_queue.Next(&tag, &ok)) {
      break;
    }

    // We got an event from the completion queue.
    // Now we see what type it is, process it and then delete the struct.

    if (!ok) {
      _watch_context.TryCancel();
      _watch_completion_queue.Shutdown();
      continue;
    }

    watch_data* watchData = reinterpret_cast<watch_data*>(tag);

    switch (watchData->tag) {
      case watch_type_e::Create: {
        // The watch is not created yet, since the server has to confirm
        // the creation. Therefore we place the watch data into a pending
        // queue.
        _pending_watch_create_data = watchData;
        break;
      }
      case watch_type_e::Cancel: {
        break;
      }
      case watch_type_e::Read: {
        if (_watchResponse.created()) {
          _pending_watch_create_data->watch_id = _watchResponse.watch_id();

          _pending_watch_create_data->on_create();
          _pending_watch_create_data->on_create = nullptr;

          std::lock_guard<decltype(_watch_thread_mutex)> lock(
              _watch_thread_mutex);
          _createdWatches.push_back(_pending_watch_create_data);
          _pending_watch_create_data = nullptr;
        } else if (_watchResponse.canceled()) {
          assert(_pending_watch_cancel_data->watch_id ==
                 _watchResponse.watch_id());

          _pending_watch_cancel_data->on_cancel();
          delete _pending_watch_cancel_data;
          _pending_watch_cancel_data = nullptr;
        } else {
          auto events = _watchResponse.events();
          auto createdWatchData =
              find_created_watch_withlock(_watchResponse.watch_id());

          // Loop over all events and call the respective listener
          for (const auto& ev : events) {
            if (ev.type() == mvccpb::Event_EventType_PUT) {
              createdWatchData->listener.onKeyAdded(ev.kv().key(),
                                                    ev.kv().value());
            } else {
              createdWatchData->listener.onKeyRemoved(ev.kv().key());
            }
          }
        }

        // We read again from the server after we got a message.
        _watch_stream->Read(&_watchResponse, watchData);
        break;
      }
      default: {
        break;
      }
    }
  }

  _thread_running.store(false);
}

bool watcher::CreateWatch(const std::string& prefix, listener_t listener) {
  using namespace etcdserverpb;
  // We get the rangeEnd. We currently always treat the key as a prefix.
  std::string rangeEnd(prefix);
  int ascii = rangeEnd[prefix.size() - 1];
  rangeEnd.back() = ascii + 1;

  auto createReq = new WatchCreateRequest();
  createReq->set_key(prefix);
  createReq->set_range_end(rangeEnd);
  createReq->set_start_revision(0);
  createReq->set_prev_kv(false);
  createReq->set_progress_notify(false);

  WatchRequest req;
  req.set_allocated_create_request(createReq);

  auto createPromise = std::make_shared<std::promise<bool>>();
  std::future<bool> createFuture = createPromise->get_future();

  auto watchData =
      new watch_data(watch_type_e::Create, prefix, std::move(listener),
                     [createPromise]() { createPromise->set_value(true); });

  _watch_stream->Write(req, watchData);

  auto status = createFuture.wait_for(std::chrono::seconds(1));
  if (status == std::future_status::timeout) {
    return false;
  }

  return true;
}

bool watcher::CancelWatch(const std::string& prefix) {
  using namespace etcdserverpb;

  // First we try to remove the prefix from the created watches map
  {
    std::lock_guard<decltype(_watch_thread_mutex)> lock(_watch_thread_mutex);
    auto it = std::find_if(
        _createdWatches.begin(), _createdWatches.end(),
        [&](const watch_data* data) -> bool { return data->prefix == prefix; });
    if (it == _createdWatches.end()) {
      return false;
    }
    _pending_watch_cancel_data = *it;
    _createdWatches.erase(it);
  }

  auto cancelPromise = std::make_shared<std::promise<void>>();
  std::future<void> cancelFuture = cancelPromise->get_future();

  // We change the current tag to cancel, otherwise we will not know
  // what to do once we get the struct from the completion queue.
  _pending_watch_cancel_data->tag = watch_type_e::Cancel;
  _pending_watch_cancel_data->on_cancel = [=]() { cancelPromise->set_value(); };

  auto cancelReq = new WatchCancelRequest();
  cancelReq->set_watch_id(_pending_watch_cancel_data->watch_id);

  WatchRequest req;
  req.set_allocated_cancel_request(cancelReq);

  _watch_stream->Write(req, _pending_watch_cancel_data);

  auto status = cancelFuture.wait_for(std::chrono::seconds(1));
  if (status == std::future_status::timeout) {
    return false;
  }

  return true;
}

bool watcher::add_prefix(const std::string &prefix,
                         OnKeyAddedFunc onKeyAdded,
                         OnKeyRemovedFunc onKeyRemoved)
{
  if (!this->_thread_running.load()) {
    return false;
  }
  listener_t listener(std::move(onKeyAdded), std::move(onKeyRemoved));
  return CreateWatch(prefix, std::move(listener));
}

bool watcher::remove_prefix(const std::string& prefix) {
  if (!this->_thread_running.load()) {
    return false;
  }
  return CancelWatch(prefix);
}

bool watcher::Start() {
  if (_thread_running.load()) {
    return false;
  }

  if (_channel->GetState(false) ==
      grpc_connectivity_state::GRPC_CHANNEL_SHUTDOWN) {
    return false;
  }

  _watchStub = etcdserverpb::Watch::NewStub(_channel);
  //_watch_completion_queue = grpc::CompletionQueue();
  _watch_stream =
      _watchStub->AsyncWatch(&_watch_context, &_watch_completion_queue,
                             reinterpret_cast<void*>(watch_type_e::Start));

  bool ok;
  void* tag;
  if (!_watch_completion_queue.Next(&tag, &ok)) {
    return false;
  }

  if (!ok) {
    return false;
  }

  // Start reading from the watch stream
  auto watchData = new watch_data(watch_type_e::Read);
  _watch_stream->Read(&_watchResponse, static_cast<void*>(watchData));

  _thread_running.store(true);
  _watch_thread = std::thread(std::bind(&watcher::Thread_Start, this));
  return true;
}

} // namespace etcdapi