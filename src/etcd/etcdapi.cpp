#include "etcdapi.h"

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

static const char* kStatusCodeNames[] = {
#define ETCD_STATUS_CODE(e, s) s
    ETCD_STATUS_CODES
#undef ETCD_STATUS_CODE
};

const char* Etcd::StatusCodeStr(StatusCode code) {
  return kStatusCodeNames[static_cast<int>(code)];
}

std::shared_ptr<grpc::Channel> MakeChannel(
    const std::string& address,
    const std::shared_ptr<grpc::ChannelCredentials>& channelCredentials) {
  const std::string substr("://");
  const auto i = address.find(substr);
  if (i == std::string::npos) {
    return grpc::CreateChannel(address, channelCredentials);
  }
  const std::string stripped_address = address.substr(i + substr.length());
  return grpc::CreateChannel(stripped_address, channelCredentials);
}

Etcd::Client::Client(const std::string& address) {
  this->_channel = MakeChannel(address, grpc::InsecureChannelCredentials());
  this->_kvStub = etcdserverpb::KV::NewStub(_channel);
  this->_watcher = std::unique_ptr<Watcher>(new Watcher(std::move(_channel)));
  // this->_leaseStub = etcdserverpb::Lease::NewStub(_channel);
}

Etcd::Client::~Client() { _watcher->Stop(); }

Etcd::StatusCode Etcd::Client::Put(const std::string& key,
                                   const std::string& value,
                                   Etcd::LeaseId leaseId) {
  etcdserverpb::PutRequest putRequest;
  putRequest.set_key(key);
  putRequest.set_value(value);
  putRequest.set_lease(leaseId);
  putRequest.set_prev_kv(false);

  etcdserverpb::PutResponse putResponse;

  grpc::ClientContext context;
  grpc::Status status = _kvStub->Put(&context, putRequest, &putResponse);

  if (!status.ok()) {
    return (Etcd::StatusCode)status.error_code();
  }

  return Etcd::StatusCode::Ok;
}

Etcd::LeaseGrantResponse Etcd::Client::LeaseGrant(std::chrono::seconds ttl) {
  etcdserverpb::LeaseGrantRequest req;
  req.set_ttl(ttl.count());
  etcdserverpb::LeaseGrantResponse res;

  grpc::ClientContext context;
  grpc::Status status = _leaseStub->LeaseGrant(&context, req, &res);

  if (!status.ok()) {
    auto ret =
        Etcd::LeaseGrantResponse((Etcd::StatusCode)status.error_code(),
                                 res.id(), std::chrono::seconds(res.ttl()));

    return ret;
  }

  return Etcd::LeaseGrantResponse(Etcd::StatusCode::Ok, res.id(),
                                  std::chrono::seconds(res.ttl()));
}

Etcd::StatusCode Etcd::Client::LeaseRevoke(Etcd::LeaseId leaseId) {
  etcdserverpb::LeaseRevokeRequest req;
  req.set_id(leaseId);

  etcdserverpb::LeaseRevokeResponse res;

  grpc::ClientContext context;
  grpc::Status status = _leaseStub->LeaseRevoke(&context, req, &res);

  if (!status.ok()) {
    auto ret = (Etcd::StatusCode)status.error_code();
    return ret;
  }

  return Etcd::StatusCode::Ok;
}

Etcd::GetResponse Etcd::Client::Get(const std::string& key) {
  etcdserverpb::RangeRequest req;
  req.set_key(key);

  etcdserverpb::RangeResponse res;

  grpc::ClientContext context;
  grpc::Status status = _kvStub->Range(&context, req, &res);

  auto statusCode = (Etcd::StatusCode)status.error_code();

  if (!status.ok()) {
    Etcd::GetResponse ret(statusCode);
    return ret;
  }

  if (res.count() == 0) {
    return Etcd::GetResponse(Etcd::StatusCode::NotFound);
  }

  return Etcd::GetResponse(statusCode, res.kvs(0).value());
}

Etcd::StatusCode Etcd::Client::Delete(const std::string& key) {
  etcdserverpb::DeleteRangeRequest req;
  req.set_key(key);
  req.set_prev_kv(false);

  etcdserverpb::DeleteRangeResponse res;

  grpc::ClientContext context;
  grpc::Status status = _kvStub->DeleteRange(&context, req, &res);

  if (!status.ok()) {
    auto ret = (Etcd::StatusCode)status.error_code();
    return ret;
  }

  return Etcd::StatusCode::Ok;
}

Etcd::ListResponse Etcd::Client::List(const std::string& keyPrefix) {
  etcdserverpb::RangeRequest req;
  req.set_key(keyPrefix);

  etcdserverpb::RangeResponse res;

  grpc::ClientContext context;
  grpc::Status status = _kvStub->Range(&context, req, &res);

  auto statusCode = (Etcd::StatusCode)status.error_code();

  if (!status.ok()) {
    Etcd::ListResponse ret(statusCode);
    return ret;
  }

  Etcd::ListResponse::KeyValuePairs kvs;
  kvs.reserve(res.count());

  for (int i = 0; i < res.count(); ++i) {
    const auto& kv = res.kvs(i);
    kvs.push_back(std::make_pair(kv.key(), kv.value()));
  }

  return Etcd::ListResponse(statusCode, std::move(kvs));
}

bool Etcd::Client::StartWatch() { return _watcher->Start(); }

void Etcd::Client::StopWatch() { _watcher->Stop(); }

bool Etcd::Client::AddWatchPrefix(const std::string& prefix,
                                  Etcd::OnKeyAddedFunc onKeyAdded,
                                  Etcd::OnKeyRemovedFunc onKeyRemoved) {
  return _watcher->AddPrefix(prefix, std::move(onKeyAdded),
                             std::move(onKeyRemoved));
}

bool Etcd::Client::RemoveWatchPrefix(const std::string& prefix) {
  return _watcher->RemovePrefix(prefix);
}

std::shared_ptr<Etcd::Client> Etcd::Client_Create(const std::string& address) {
  return std::make_shared<Client>(address);
}

namespace Etcd {
void Watcher::Stop() {
  using namespace std::chrono;
  if (!_threadRunning.load()) {
    return;
  }

  _watchContext.TryCancel();

  _watchCompletionQueue.Shutdown();
  if (_watchThread.joinable()) {
    _watchThread.join();
  }
  this->_watchStub = nullptr;
}

WatchData* Watcher::FindCreatedWatchWithLock(int64_t watchId) {
  std::lock_guard<decltype(_watchThreadMutex)> lock(_watchThreadMutex);
  auto it = std::find_if(
      _createdWatches.begin(), _createdWatches.end(),
      [=](const WatchData* data) -> bool { return data->watchId == watchId; });
  if (it == _createdWatches.end()) {
    return nullptr;
  } else {
    return *it;
  }
}

void Watcher::Thread_Start() {
  using namespace std::chrono;

  for (;;) {
    void* tag;
    bool ok;
    if (!_watchCompletionQueue.Next(&tag, &ok)) {
      break;
    }

    // We got an event from the completion queue.
    // Now we see what type it is, process it and then delete the struct.

    if (!ok) {
      _watchContext.TryCancel();
      _watchCompletionQueue.Shutdown();
      continue;
    }

    WatchData* watchData = reinterpret_cast<WatchData*>(tag);

    switch (watchData->tag) {
      case WatchTag::Create: {
        // The watch is not created yet, since the server has to confirm
        // the creation. Therefore we place the watch data into a pending
        // queue.
        _pendingWatchCreateData = watchData;
        break;
      }
      case WatchTag::Cancel: {
        break;
      }
      case WatchTag::Read: {
        if (_watchResponse.created()) {
          _pendingWatchCreateData->watchId = _watchResponse.watch_id();

          _pendingWatchCreateData->onCreate();
          _pendingWatchCreateData->onCreate = nullptr;

          std::lock_guard<decltype(_watchThreadMutex)> lock(_watchThreadMutex);
          _createdWatches.push_back(_pendingWatchCreateData);
          _pendingWatchCreateData = nullptr;
        } else if (_watchResponse.canceled()) {
          assert(_pendingWatchCancelData->watchId == _watchResponse.watch_id());

          _pendingWatchCancelData->onCancel();
          delete _pendingWatchCancelData;
          _pendingWatchCancelData = nullptr;
        } else {
          auto events = _watchResponse.events();
          auto createdWatchData =
              FindCreatedWatchWithLock(_watchResponse.watch_id());

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
        _watchStream->Read(&_watchResponse, watchData);
        break;
      }
      default: {
        break;
      }
    }
  }

  _threadRunning.store(false);
}

bool Watcher::CreateWatch(const std::string& prefix, Listener listener) {
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
      new WatchData(WatchTag::Create, prefix, std::move(listener),
                    [createPromise]() { createPromise->set_value(true); });

  _watchStream->Write(req, watchData);

  auto status = createFuture.wait_for(std::chrono::seconds(1));
  if (status == std::future_status::timeout) {
    return false;
  }

  return true;
}

bool Watcher::CancelWatch(const std::string& prefix) {
  using namespace etcdserverpb;

  // First we try to remove the prefix from the created watches map
  {
    std::lock_guard<decltype(_watchThreadMutex)> lock(_watchThreadMutex);
    auto it = std::find_if(
        _createdWatches.begin(), _createdWatches.end(),
        [&](const WatchData* data) -> bool { return data->prefix == prefix; });
    if (it == _createdWatches.end()) {
      return false;
    }
    _pendingWatchCancelData = *it;
    _createdWatches.erase(it);
  }

  auto cancelPromise = std::make_shared<std::promise<void>>();
  std::future<void> cancelFuture = cancelPromise->get_future();

  // We change the current tag to cancel, otherwise we will not know
  // what to do once we get the struct from the completion queue.
  _pendingWatchCancelData->tag = WatchTag::Cancel;
  _pendingWatchCancelData->onCancel = [=]() { cancelPromise->set_value(); };

  auto cancelReq = new WatchCancelRequest();
  cancelReq->set_watch_id(_pendingWatchCancelData->watchId);

  WatchRequest req;
  req.set_allocated_cancel_request(cancelReq);

  _watchStream->Write(req, _pendingWatchCancelData);

  auto status = cancelFuture.wait_for(std::chrono::seconds(1));
  if (status == std::future_status::timeout) {
    return false;
  }

  return true;
}

bool Watcher::AddPrefix(const std::string& prefix,
                        Etcd::OnKeyAddedFunc onKeyAdded,
                        Etcd::OnKeyRemovedFunc onKeyRemoved) {
  if (!this->_threadRunning.load()) {
    return false;
  }
  Listener listener(std::move(onKeyAdded), std::move(onKeyRemoved));
  return CreateWatch(prefix, std::move(listener));
}

bool Watcher::RemovePrefix(const std::string& prefix) {
  if (!this->_threadRunning.load()) {
    return false;
  }
  return CancelWatch(prefix);
}

bool Watcher::Start() {
  if (_threadRunning.load()) {
    return false;
  }

  if (_channel->GetState(false) ==
      grpc_connectivity_state::GRPC_CHANNEL_SHUTDOWN) {
    return false;
  }

  _watchStub = etcdserverpb::Watch::NewStub(_channel);
  //_watchCompletionQueue = grpc::CompletionQueue();
  _watchStream =
      _watchStub->AsyncWatch(&_watchContext, &_watchCompletionQueue,
                             reinterpret_cast<void*>(WatchTag::Start));

  bool ok;
  void* tag;
  if (!_watchCompletionQueue.Next(&tag, &ok)) {
    return false;
  }

  if (!ok) {
    return false;
  }

  // Start reading from the watch stream
  auto watchData = new WatchData(WatchTag::Read);
  _watchStream->Read(&_watchResponse, static_cast<void*>(watchData));

  _threadRunning.store(true);
  _watchThread = std::thread(std::bind(&Watcher::Thread_Start, this));
  return true;
}

}  // namespace Etcd