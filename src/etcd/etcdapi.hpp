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

namespace etcdapi
{

  // borrowed from grpc status code
  // value is equal to theirs
  enum class status_code_e
  {
    /// Not an error; returned on success.
    OK = 0,

    /// The operation was cancelled (typically by the caller).
    CANCELLED = 1,

    /// Unknown error. An example of where this error may be returned is if a
    /// Status value received from another address space belongs to an error-space
    /// that is not known in this address space. Also errors raised by APIs that
    /// do not return enough error information may be converted to this error.
    UNKNOWN = 2,

    /// v3_client_t specified an invalid argument. Note that this differs from
    /// FAILED_PRECONDITION. INVALID_ARGUMENT indicates arguments that are
    /// problematic regardless of the state of the system (e.g., a malformed file
    /// name).
    INVALID_ARGUMENT = 3,

    /// Deadline expired before operation could complete. For operations that
    /// change the state of the system, this error may be returned even if the
    /// operation has completed successfully. For example, a successful response
    /// from a server could have been delayed long enough for the deadline to
    /// expire.
    DEADLINE_EXCEEDED = 4,

    /// Some requested entity (e.g., file or directory) was not found.
    NOT_FOUND = 5,

    /// Some entity that we attempted to create (e.g., file or directory) already
    /// exists.
    ALREADY_EXISTS = 6,

    /// The caller does not have permission to execute the specified operation.
    /// PERMISSION_DENIED must not be used for rejections caused by exhausting
    /// some resource (use RESOURCE_EXHAUSTED instead for those errors).
    /// PERMISSION_DENIED must not be used if the caller can not be identified
    /// (use UNAUTHENTICATED instead for those errors).
    PERMISSION_DENIED = 7,

    /// The request does not have valid authentication credentials for the
    /// operation.
    UNAUTHENTICATED = 16,

    /// Some resource has been exhausted, perhaps a per-user quota, or perhaps the
    /// entire file system is out of space.
    RESOURCE_EXHAUSTED = 8,

    /// Operation was rejected because the system is not in a state required for
    /// the operation's execution. For example, directory to be deleted may be
    /// non-empty, an rmdir operation is applied to a non-directory, etc.
    ///
    /// A litmus test that may help a service implementor in deciding
    /// between FAILED_PRECONDITION, ABORTED, and UNAVAILABLE:
    ///  (a) Use UNAVAILABLE if the client can retry just the failing call.
    ///  (b) Use ABORTED if the client should retry at a higher-level
    ///      (e.g., restarting a read-modify-write sequence).
    ///  (c) Use FAILED_PRECONDITION if the client should not retry until
    ///      the system state has been explicitly fixed. E.g., if an "rmdir"
    ///      fails because the directory is non-empty, FAILED_PRECONDITION
    ///      should be returned since the client should not retry unless
    ///      they have first fixed up the directory by deleting files from it.
    ///  (d) Use FAILED_PRECONDITION if the client performs conditional
    ///      REST Get/Update/Delete on a resource and the resource on the
    ///      server does not match the condition. E.g., conflicting
    ///      read-modify-write on the same resource.
    FAILED_PRECONDITION = 9,

    /// The operation was aborted, typically due to a concurrency issue like
    /// sequencer check failures, transaction aborts, etc.
    ///
    /// See litmus test above for deciding between FAILED_PRECONDITION, ABORTED,
    /// and UNAVAILABLE.
    ABORTED = 10,

    /// Operation was attempted past the valid range. E.g., seeking or reading
    /// past end of file.
    ///
    /// Unlike INVALID_ARGUMENT, this error indicates a problem that may be fixed
    /// if the system state changes. For example, a 32-bit file system will
    /// generate INVALID_ARGUMENT if asked to read at an offset that is not in the
    /// range [0,2^32-1], but it will generate OUT_OF_RANGE if asked to read from
    /// an offset past the current file size.
    ///
    /// There is a fair bit of overlap between FAILED_PRECONDITION and
    /// OUT_OF_RANGE. We recommend using OUT_OF_RANGE (the more specific error)
    /// when it applies so that callers who are iterating through a space can
    /// easily look for an OUT_OF_RANGE error to detect when they are done.
    OUT_OF_RANGE = 11,

    /// Operation is not implemented or not supported/enabled in this service.
    UNIMPLEMENTED = 12,

    /// Internal errors. Means some invariants expected by underlying System has
    /// been broken. If you see one of these errors, Something is very broken.
    INTERNAL = 13,

    /// The service is currently unavailable. This is a most likely a transient
    /// condition and may be corrected by retrying with a backoff.
    ///
    /// \warning Although data MIGHT not have been transmitted when this
    /// status occurs, there is NOT A GUARANTEE that the server has not seen
    /// anything. So in general it is unsafe to retry on this status code
    /// if the call is non-idempotent.
    ///
    /// See litmus test above for deciding between FAILED_PRECONDITION, ABORTED,
    /// and UNAVAILABLE.
    UNAVAILABLE = 14,

    /// Unrecoverable data loss or corruption.
    DATA_LOSS = 15,

    /// Force users to include a default branch:
    DO_NOT_USE = -1
  };

  class watcher;

  using leaseid_t = int64_t;

  struct lease_grant_response_t
  {
    status_code_e status_code;
    leaseid_t id;
    std::chrono::seconds ttl;

    lease_grant_response_t(status_code_e code, leaseid_t id,
                           std::chrono::seconds ttl)
        : status_code(code), id(id), ttl(ttl) {}

    bool is_ok() const { return status_code == status_code_e::OK; }
  };

  struct kv_range_response_t
  {
    status_code_e status_code;
    std::string value;

    kv_range_response_t(status_code_e code, const std::string val)
        : status_code(code), value(val) {}

    explicit kv_range_response_t(status_code_e code) : status_code(code) {}

    bool is_ok() const { return status_code == status_code_e::OK; }
  };

  struct kv_list_response_t
  {
    using kv_pairs = std::vector<std::pair<std::string, std::string>>;
    status_code_e status_code;
    kv_pairs kvs;

    kv_list_response_t(status_code_e code, const kv_pairs &kvs)
        : status_code(code), kvs(kvs) {}

    explicit kv_list_response_t(status_code_e code) : status_code(code) {}

    bool is_ok() const { return status_code == status_code_e::OK; }
  };

  using OnKeyAddedFunc =
      std::function<void(const std::string &, const std::string &)>;
  using OnKeyRemovedFunc = std::function<void(const std::string &)>;

  class v3_client_t
  {
  public:
    v3_client_t(const std::string &address);
    ~v3_client_t();
    status_code_e kv_put(const std::string &key, const std::string &value,
                         leaseid_t lid);
    status_code_e kv_delete(const std::string &key);
    lease_grant_response_t lease_grant(std::chrono::seconds ttl);
    status_code_e lease_revoke(leaseid_t lid);
    kv_range_response_t kv_get(const std::string &key);
    kv_list_response_t kv_list(const std::string &keyPrefix = "");

    bool StartWatch();
    bool AddWatchPrefix(const std::string &prefix, OnKeyAddedFunc onKeyAdded,
                        OnKeyRemovedFunc onKeyRemoved);
    bool RemoveWatchPrefix(const std::string &prefix);
    void StopWatch();

  private:
    std::shared_ptr<grpc::Channel> _channel;
    std::shared_ptr<etcdserverpb::KV::Stub> _kvStub;
    std::shared_ptr<etcdserverpb::Lease::Stub> _leaseStub;
    std::unique_ptr<watcher> _watcher;
  };

  std::shared_ptr<v3_client_t> create_v3_client(const std::string &address);

  struct listener_t
  {
    OnKeyAddedFunc onKeyAdded;
    OnKeyRemovedFunc onKeyRemoved;

    listener_t() = default;

    listener_t(OnKeyAddedFunc onKeyAdded,
               OnKeyRemovedFunc onKeyRemoved)
        : onKeyAdded(std::move(onKeyAdded)),
          onKeyRemoved(std::move(onKeyRemoved)) {}
  };

  enum class watch_type_e
  {
    Start = 1,
    Create = 2,
    Cancel = 3,
    Read = 4,
  };

  struct watch_data
  {
    watch_type_e tag;
    std::string prefix;
    listener_t listener;
    int64_t watch_id;

    // Called whenever the watch is created
    std::function<void()> on_create;
    // Called whenever the watch is canceled
    std::function<void()> on_cancel;

    watch_data(watch_type_e t, const std::string &p = "", listener_t l = listener_t(),
               std::function<void()> on_create = nullptr)
        : tag(t),
          prefix(p),
          listener(std::move(l)),
          watch_id(-1),
          on_create(std::move(on_create)) {}
  };
  using watch_stream =
      std::unique_ptr<grpc::ClientAsyncReaderWriter<etcdserverpb::WatchRequest,
                                                    etcdserverpb::WatchResponse>>;
  class watcher
  {
  private:
    void Thread_Start();

  public:
    watcher(std::shared_ptr<grpc::Channel> channel)
        : _channel(channel),
          _pending_watch_create_data(nullptr),
          _pending_watch_cancel_data(nullptr),
          _thread_running(false) {}

    bool Start();

    ~watcher() = default;
    void Stop();
    bool add_prefix(const std::string &prefix, OnKeyAddedFunc onKeyAdded,
                    OnKeyRemovedFunc onKeyRemoved);
    bool remove_prefix(const std::string &prefix);

    static std::unique_ptr<watcher> Create(
        std::shared_ptr<grpc::Channel> channel);
    watch_data *find_created_watch_withlock(int64_t);
    bool CreateWatch(const std::string &prefix, listener_t listener);
    bool CancelWatch(const std::string &prefix);

  private:
    std::shared_ptr<grpc::Channel> _channel;
    std::shared_ptr<etcdserverpb::Watch::Stub> _watchStub;
    etcdserverpb::WatchResponse _watchResponse;

    // Hold all of the watches that where actually created
    std::vector<watch_data *> _createdWatches;

    // The worker thread is responsible for watching over etcd
    // and sending keep alive requests.
    std::mutex _watch_thread_mutex;
    watch_data *_pending_watch_create_data;
    watch_data *_pending_watch_cancel_data;
    std::thread _watch_thread;
    std::atomic_bool _thread_running;

    grpc::CompletionQueue _watch_completion_queue;
    grpc::ClientContext _watch_context;
    watch_stream _watch_stream;
  };
} // namespace etcdapi