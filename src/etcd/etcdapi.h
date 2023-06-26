#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "auth.grpc.pb.h"
#include "etcdserver.grpc.pb.h"
#include "kv.grpc.pb.h"
#include "rpc.grpc.pb.h"

#include <grpc++/channel.h>
#include <grpc++/grpc++.h>

#define ETCD_STATUS_CODES                                             \
  ETCD_STATUS_CODE(Ok = 0, "Ok"),                                     \
      ETCD_STATUS_CODE(Cancelled = 1, "Cancelled"),                   \
      ETCD_STATUS_CODE(Unknown = 2, "Unknown"),                       \
      ETCD_STATUS_CODE(InvalidArgument = 3, "InvalidArgument"),       \
      ETCD_STATUS_CODE(DeadlineExceeded = 4, "DeadlineExceeded"),     \
      ETCD_STATUS_CODE(NotFound = 5, "NotFound"),                     \
      ETCD_STATUS_CODE(AlreadyExists = 6, "AlreadyExists"),           \
      ETCD_STATUS_CODE(PermissionDenied = 7, "PermissionDenied"),     \
      ETCD_STATUS_CODE(Unauthenticated = 16, "Unauthenticated"),      \
      ETCD_STATUS_CODE(ResourceExhausted = 8, "ResourceExhausted"),   \
      ETCD_STATUS_CODE(FailedPrecondition = 9, "FailedPrecondition"), \
      ETCD_STATUS_CODE(Aborted = 10, "Aborted"),                      \
      ETCD_STATUS_CODE(OutOfRange = 11, "OutOfRange"),                \
      ETCD_STATUS_CODE(Unimplemented = 12, "Unimplemented"),          \
      ETCD_STATUS_CODE(Internal = 13, "Internal"),                    \
      ETCD_STATUS_CODE(Unavailable = 14, "Unavailable"),              \
      ETCD_STATUS_CODE(DataLoss = 15, "DataLoss"),

namespace Etcd {
class Watcher;

using LeaseId = int64_t;

// This is copied from gRPC in order to hide the dependency from the user.
enum class StatusCode : int {
#define ETCD_STATUS_CODE(e, s) e
  ETCD_STATUS_CODES
#undef ETCD_STATUS_CODE
};

const char* StatusCodeStr(StatusCode code);

struct LeaseGrantResponse {
  StatusCode statusCode;
  LeaseId id;
  std::chrono::seconds ttl;

  LeaseGrantResponse(StatusCode code, LeaseId id, std::chrono::seconds ttl)
      : statusCode(code), id(id), ttl(ttl) {}

  bool IsOk() const { return statusCode == StatusCode::Ok; }
};

struct GetResponse {
  StatusCode statusCode;
  std::string value;

  GetResponse(StatusCode code, const std::string val)
      : statusCode(code), value(val) {}

  explicit GetResponse(StatusCode code) : statusCode(code) {}

  bool IsOk() const { return statusCode == StatusCode::Ok; }
};

struct ListResponse {
  using KeyValuePairs = std::vector<std::pair<std::string, std::string>>;
  StatusCode statusCode;
  KeyValuePairs kvs;

  ListResponse(StatusCode code, const KeyValuePairs& kvs)
      : statusCode(code), kvs(kvs) {}

  explicit ListResponse(StatusCode code) : statusCode(code) {}

  bool IsOk() const { return statusCode == StatusCode::Ok; }
};

using OnKeyAddedFunc =
    std::function<void(const std::string&, const std::string&)>;
using OnKeyRemovedFunc = std::function<void(const std::string&)>;

class Client {
 public:
  Client(const std::string& address);
  ~Client();
  StatusCode Put(const std::string& key, const std::string& value,
                 LeaseId leaseId);
  StatusCode Delete(const std::string& key);
  LeaseGrantResponse LeaseGrant(std::chrono::seconds ttl);
  StatusCode LeaseRevoke(LeaseId leaseId);
  GetResponse Get(const std::string& key);
  ListResponse List(const std::string& keyPrefix = "");

  bool StartWatch();
  bool AddWatchPrefix(const std::string& prefix, OnKeyAddedFunc onKeyAdded,
                      OnKeyRemovedFunc onKeyRemoved);
  bool RemoveWatchPrefix(const std::string& prefix);
  void StopWatch();

 private:
  std::shared_ptr<grpc::Channel> _channel;
  std::shared_ptr<etcdserverpb::KV::Stub> _kvStub;
  std::shared_ptr<etcdserverpb::Lease::Stub> _leaseStub;
  std::unique_ptr<Etcd::Watcher> _watcher;
};

std::shared_ptr<Client> Client_Create(const std::string& address);

using WatchStream =
    std::unique_ptr<grpc::ClientAsyncReaderWriter<etcdserverpb::WatchRequest,
                                                  etcdserverpb::WatchResponse>>;

struct Listener {
  Etcd::OnKeyAddedFunc onKeyAdded;
  Etcd::OnKeyRemovedFunc onKeyRemoved;

  Listener() = default;

  Listener(Etcd::OnKeyAddedFunc onKeyAdded, Etcd::OnKeyRemovedFunc onKeyRemoved)
      : onKeyAdded(std::move(onKeyAdded)),
        onKeyRemoved(std::move(onKeyRemoved)) {}
};

enum class WatchTag {
  Start = 1,
  Create = 2,
  Cancel = 3,
  Read = 4,
};

enum class WatchJobType {
  Add,
  Remove,
  Unknown,
};

struct WatchData {
  WatchTag tag;
  std::string prefix;
  Listener listener;
  int64_t watchId;

  // Called whenever the watch is created
  std::function<void()> onCreate;
  // Called whenever the watch is canceled
  std::function<void()> onCancel;

  WatchData(WatchTag t, const std::string& p = "", Listener l = Listener(),
            std::function<void()> onCreate = nullptr)
      : tag(t),
        prefix(p),
        listener(std::move(l)),
        watchId(-1),
        onCreate(std::move(onCreate)) {}
};

class Watcher {
 private:
  void Thread_Start();

 public:
  Watcher(std::shared_ptr<grpc::Channel> channel)
      : _channel(channel),
        _pendingWatchCreateData(nullptr),
        _pendingWatchCancelData(nullptr),
        _threadRunning(false) {}

  bool Start();

  ~Watcher() = default;
  void Stop();
  bool AddPrefix(const std::string& prefix, Etcd::OnKeyAddedFunc onKeyAdded,
                 Etcd::OnKeyRemovedFunc onKeyRemoved);
  bool RemovePrefix(const std::string& prefix);

  static std::unique_ptr<Watcher> Create(
      std::shared_ptr<grpc::Channel> channel);
  WatchData* FindCreatedWatchWithLock(int64_t);
  bool CreateWatch(const std::string& prefix, Listener listener);
  bool CancelWatch(const std::string& prefix);

 private:
  std::shared_ptr<grpc::Channel> _channel;
  std::shared_ptr<etcdserverpb::Watch::Stub> _watchStub;
  etcdserverpb::WatchResponse _watchResponse;

  // Hold all of the watches that where actually created
  std::vector<WatchData*> _createdWatches;

  // The worker thread is responsible for watching over etcd
  // and sending keep alive requests.
  std::mutex _watchThreadMutex;
  WatchData* _pendingWatchCreateData;
  WatchData* _pendingWatchCancelData;
  std::thread _watchThread;
  std::atomic_bool _threadRunning;

  grpc::CompletionQueue _watchCompletionQueue;
  grpc::ClientContext _watchContext;
  WatchStream _watchStream;
};
}  // namespace Etcd