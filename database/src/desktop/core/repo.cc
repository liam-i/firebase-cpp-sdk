// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "database/src/desktop/core/repo.h"
#include "app/src/callback.h"
#include "app/src/log.h"
#include "app/src/scheduler.h"
#include "app/src/variant_util.h"
#include "database/src/desktop/connection/persistent_connection.h"
#include "database/src/desktop/core/server_values.h"
#include "database/src/desktop/core/value_event_registration.h"
#include "database/src/desktop/core/web_socket_listen_provider.h"
#include "database/src/desktop/core/write_tree.h"
#include "database/src/desktop/database_desktop.h"
#include "database/src/desktop/database_reference_desktop.h"
#include "database/src/desktop/mutable_data_desktop.h"
#include "database/src/desktop/persistence/in_memory_persistence_storage_engine.h"
#include "database/src/desktop/query_desktop.h"
#include "database/src/desktop/transaction_data.h"
#include "database/src/desktop/util_desktop.h"
#include "database/src/include/firebase/database/common.h"
#include "database/src/include/firebase/database/transaction.h"

namespace firebase {

using callback::NewCallback;

namespace database {
namespace internal {

scheduler::Scheduler Repo::s_scheduler_;

// Transaction Response class to pass to PersistentConnection.
// This is used to capture all the data to use when ResponseCallback is
// triggered.
class TransactionResponse : public connection::Response {
 public:
  TransactionResponse(const Repo::ThisRef& repo, const Path& path,
                      const std::vector<TransactionDataPtr> queue,
                      ResponseCallback callback)
      : connection::Response(callback),
        repo_ref_(repo),
        path_(path),
        queue_(queue) {}

  Repo::ThisRef& repo_ref() { return repo_ref_; }
  const Path& path() { return path_; }
  std::vector<TransactionDataPtr>& queue() { return queue_; }

 private:
  // Database reference to call HandleTransactionResponse()
  Repo::ThisRef repo_ref_;

  // Database path for this write request
  Path path_;

  // All the transaction used for this write request
  std::vector<TransactionDataPtr> queue_;
};

Repo::Repo(App* app, DatabaseInternal* database, const char* url)
    : database_(database),
      host_info_(),
      connection_(),
      next_write_id_(0),
      safe_this_(this) {
  ParseUrl parser;
  if (parser.Parse(url) != ParseUrl::kParseOk) {
    LogError("Database Url is not valid: %s", url);
    return;
  }

  host_info_ = connection::HostInfo(parser.hostname.c_str(), parser.ns.c_str(),
                                    parser.secure);
  url_ = host_info_.ToString();

  connection_.reset(new connection::PersistentConnection(app, host_info_, this,
                                                         &s_scheduler_));
  connection_->ScheduleInitialize();

  // Kick off any expensive additional initialization
  s_scheduler_.Schedule(NewCallback(
      [](ThisRef ref) {
        ThisRefLock lock(&ref);
        if (lock.GetReference() != nullptr) {
          lock.GetReference()->DeferredInitialization();
        }
      },
      safe_this_));
}

Repo::~Repo() { safe_this_.ClearReference(); }

void Repo::AddEventCallback(UniquePtr<EventRegistration> event_registration) {
  PostEvents(server_sync_tree_->AddEventRegistration(Move(event_registration)));
}

void Repo::RemoveEventCallback(void* listener_ptr,
                               const QuerySpec& query_spec) {
  PostEvents(server_sync_tree_->RemoveEventRegistration(
      query_spec, listener_ptr, kErrorNone));
}

class OnDisconnectResponse : public connection::Response {
 public:
  OnDisconnectResponse(Repo* repo, const SafeFutureHandle<void>& handle,
                       ReferenceCountedFutureImpl* ref_future, const Path& path,
                       const Variant& data, ResponseCallback callback)
      : connection::Response(callback),
        repo_(repo),
        handle_(handle),
        ref_future_(ref_future),
        path_(path),
        data_(data) {
    assert(ref_future != nullptr);
  }

  void MarkComplete() {
    if (!HasError()) {
      ref_future_->Complete(handle_, kErrorNone);
    } else {
      ref_future_->Complete(handle_, GetErrorCode(), GetErrorMessage().c_str());
    }
  }

  Repo* repo() const { return repo_; }
  const Path& path() const { return path_; }
  const Variant& data() const { return data_; }

 private:
  Repo* repo_;
  SafeFutureHandle<void> handle_;
  ReferenceCountedFutureImpl* ref_future_;
  Path path_;
  Variant data_;
};

void Repo::OnDisconnectSetValue(const SafeFutureHandle<void>& handle,
                                ReferenceCountedFutureImpl* ref_future,
                                const Path& path, const Variant& data) {
  connection::ResponsePtr response = MakeShared<OnDisconnectResponse>(
      this, handle, ref_future, path, data,
      [](const connection::ResponsePtr& ptr) {
        OnDisconnectResponse* response =
            static_cast<OnDisconnectResponse*>(ptr.get());
        assert(response);
        if (!response->HasError()) {
          response->repo()->on_disconnect_.Remember(response->path(),
                                                    response->data());
        }
        response->MarkComplete();
      });

  Repo::scheduler().Schedule(NewCallback(
      [](ThisRef ref, connection::ResponsePtr ptr) {
        ThisRefLock lock(&ref);
        if (lock.GetReference() != nullptr) {
          OnDisconnectResponse* response =
              static_cast<OnDisconnectResponse*>(ptr.get());
          assert(response);
          lock.GetReference()->connection()->OnDisconnectPut(
              response->path(), response->data(), ptr);
        }
      },
      safe_this_, response));
}

void Repo::OnDisconnectCancel(const SafeFutureHandle<void>& handle,
                              ReferenceCountedFutureImpl* ref_future,
                              const Path& path) {
  connection::ResponsePtr response = MakeShared<OnDisconnectResponse>(
      this, handle, ref_future, path, Variant::Null(),
      [](const connection::ResponsePtr& ptr) {
        OnDisconnectResponse* response =
            static_cast<OnDisconnectResponse*>(ptr.get());
        assert(response);
        if (!response->HasError()) {
          response->repo()->on_disconnect_.Forget(response->path());
        }
        response->MarkComplete();
      });

  Repo::scheduler().Schedule(NewCallback(
      [](ThisRef ref, connection::ResponsePtr ptr) {
        ThisRefLock lock(&ref);
        if (lock.GetReference() != nullptr) {
          OnDisconnectResponse* response =
              static_cast<OnDisconnectResponse*>(ptr.get());
          assert(response);
          lock.GetReference()->connection()->OnDisconnectCancel(
              response->path(), ptr);
        }
      },
      safe_this_, response));
}

void Repo::OnDisconnectUpdate(const SafeFutureHandle<void>& handle,
                              ReferenceCountedFutureImpl* ref_future,
                              const Path& path, const Variant& data) {
  connection::ResponsePtr response = MakeShared<OnDisconnectResponse>(
      this, handle, ref_future, path, data,
      [](const connection::ResponsePtr& ptr) {
        OnDisconnectResponse* response =
            static_cast<OnDisconnectResponse*>(ptr.get());
        assert(response);
        if (!response->HasError()) {
          if (response->data().is_map()) {
            for (const auto& kvp : response->data().map()) {
              const Variant& key = kvp.first;
              const Variant& value = kvp.second;
              response->repo()->on_disconnect_.Remember(
                  response->path().GetChild(key.AsString().string_value()),
                  value);
            }
          }
        }
        response->MarkComplete();
      });

  Repo::scheduler().Schedule(NewCallback(
      [](ThisRef ref, connection::ResponsePtr ptr) {
        ThisRefLock lock(&ref);
        if (lock.GetReference() != nullptr) {
          OnDisconnectResponse* response =
              static_cast<OnDisconnectResponse*>(ptr.get());
          assert(response);
          lock.GetReference()->connection()->OnDisconnectMerge(
              response->path(), response->data(), ptr);
        }
      },
      safe_this_, response));
}

void Repo::PurgeOutstandingWrites() {
  std::vector<Event> events = server_sync_tree_->RemoveAllWrites();
  PostEvents(events);
  // Abort any transactions
  AbortTransactions(Path(), kErrorWriteCanceled);
  // Remove outstanding writes from connection
  connection_->PurgeOutstandingWrites();
}

// Transaction Response class to pass to PersistentConnection.
// This is used to capture all the data to use when ResponseCallback is
// triggered.
class SetValueResponse : public connection::Response {
 public:
  SetValueResponse(const DatabaseInternal::ThisRef& database, const Path& path,
                   WriteId write_id, ReferenceCountedFutureImpl* api,
                   SafeFutureHandle<void> handle, ResponseCallback callback)
      : connection::Response(callback),
        database_ref_(database),
        path_(path),
        write_id_(write_id),
        api_(api),
        handle_(handle) {}

  DatabaseInternal::ThisRef& database_ref() { return database_ref_; }
  const Path& path() { return path_; }
  WriteId write_id() { return write_id_; }
  ReferenceCountedFutureImpl* api() { return api_; }
  SafeFutureHandle<void> handle() { return handle_; }

 private:
  // Database reference
  DatabaseInternal::ThisRef database_ref_;

  // Database path for this write request
  Path path_;

  WriteId write_id_;

  ReferenceCountedFutureImpl* api_;

  SafeFutureHandle<void> handle_;
};

void Repo::SetValue(const Path& path, const Variant& new_data_unresolved,
                    ReferenceCountedFutureImpl* api,
                    SafeFutureHandle<void> handle) {
  Variant server_values = GenerateServerValues();
  Variant new_data =
      ResolveDeferredValueSnapshot(new_data_unresolved, server_values);

  WriteId write_id = GetNextWriteId();
  std::vector<Event> events = server_sync_tree_->ApplyUserOverwrite(
      path, new_data_unresolved, new_data, write_id, kOverwriteVisible,
      kPersist);
  PostEvents(events);

  connection_->Put(
      path, new_data_unresolved,
      MakeShared<SetValueResponse>(
          DatabaseInternal::ThisRef(database_), path, write_id, api, handle,
          [](const connection::ResponsePtr& ptr) {
            auto* response = static_cast<SetValueResponse*>(ptr.get());
            DatabaseInternal::ThisRefLock lock(&response->database_ref());
            DatabaseInternal* database = lock.GetReference();
            Repo* repo = database->repo();
            repo->AckWriteAndRerunTransactions(response->write_id(),
                                               response->path(),
                                               response->GetErrorCode());
            response->api()->Complete(
                response->handle(), response->GetErrorCode(),
                GetErrorMessage(response->GetErrorCode()));
          }));

  Path affected_path = AbortTransactions(path, kErrorOverriddenBySet);
  RerunTransactions(affected_path);
}

void Repo::UpdateChildren(const Path& path, const Variant& data,
                          ReferenceCountedFutureImpl* api,
                          SafeFutureHandle<void> handle) {
  CompoundWrite updates = CompoundWrite::FromVariantMerge(data);
  if (updates.IsEmpty()) {
    // Dispatch on complete.
    api->Complete(handle, kErrorNone, "");
    return;
  }

  // Start with our existing data and merge each child into it.
  // std::map<std::string, Variant> serverValues =
  const CompoundWrite& resolved = updates;

  WriteId write_id = GetNextWriteId();
  std::vector<Event> events = server_sync_tree_->ApplyUserMerge(
      path, updates, resolved, write_id, kPersist);
  PostEvents(events);

  connection_->Merge(
      path, data,
      MakeShared<SetValueResponse>(
          DatabaseInternal::ThisRef(database_), path, write_id, api, handle,
          [](const connection::ResponsePtr& ptr) {
            auto* response = static_cast<SetValueResponse*>(ptr.get());
            DatabaseInternal::ThisRefLock lock(&response->database_ref());
            DatabaseInternal* database = lock.GetReference();
            Repo* repo = database->repo();
            repo->AckWriteAndRerunTransactions(response->write_id(),
                                               response->path(),
                                               response->GetErrorCode());
            response->api()->Complete(
                response->handle(), response->GetErrorCode(),
                GetErrorMessage(response->GetErrorCode()));
          }));

  updates.write_tree().CallOnEach(
      Path(), [this](const Path& path_from_root, const Variant& variant) {
        Path affected_path =
            this->AbortTransactions(path_from_root, kErrorOverriddenBySet);
        this->RerunTransactions(affected_path);
      });
}

void Repo::AckWriteAndRerunTransactions(WriteId write_id, const Path& path,
                                        Error error) {
  if (error == kErrorWriteCanceled) {
    // This write was already removed, we just need to ignore it...
    return;
  }

  bool success = (error == kErrorNone);
  AckStatus ack_status = success ? kAckConfirm : kAckRevert;
  std::vector<Event> events =
      server_sync_tree_->AckUserWrite(write_id, ack_status, kPersist);
  if (events.size() > 0) {
    RerunTransactions(path);
  }
  PostEvents(events);
}

// Defers any initialization that is potentially expensive (e.g. disk access).
void Repo::DeferredInitialization() {
  UniquePtr<WriteTree> pending_write_tree = MakeUnique<WriteTree>();
  UniquePtr<PersistenceStorageEngine> persistence_storage_engine =
      MakeUnique<InMemoryPersistenceStorageEngine>();
  UniquePtr<TrackedQueryManager> tracked_query_manager =
      MakeUnique<TrackedQueryManager>(persistence_storage_engine.get());
  UniquePtr<PersistenceManager> persistence_manager =
      MakeUnique<PersistenceManager>(std::move(persistence_storage_engine),
                                     std::move(tracked_query_manager));
  UniquePtr<ListenProvider> listen_provider =
      MakeUnique<WebSocketListenProvider>(connection_.get());
  server_sync_tree_ = MakeUnique<SyncTree>(std::move(pending_write_tree),
                                           std::move(persistence_manager),
                                           std::move(listen_provider));
}

void Repo::PostEvents(const std::vector<Event>& events) {
  for (const Event& event : events) {
    if (event.type != kEventTypeError) {
      event.event_registration->FireEvent(event);
    } else {
      event.event_registration->FireCancelEvent(event.error);
    }
  }
}

static std::map<Path, Variant> VariantToPathMap(const Variant& data) {
  std::map<Path, Variant> path_map;
  if (data.is_map()) {
    for (std::pair<Variant, Variant> key_value : data.map()) {
      Variant key_string_variant;
      const char* key;
      if (key_value.first.is_string()) {
        key = key_value.first.string_value();
      } else {
        key_string_variant = key_value.first.AsString();
        key = key_string_variant.string_value();
      }
      const Variant& value = key_value.second;
      path_map.insert(std::make_pair(Path(key), std::move(value)));
    }
  }
  return path_map;
}

void Repo::OnConnect() {
  //
}

void Repo::OnDisconnect() {
  Variant server_values = GenerateServerValues();
  SparseSnapshotTree resolved_tree =
      ResolveDeferredValueTree(on_disconnect_, server_values);
  std::vector<Event> events;

  resolved_tree.ForEachTree(Path(), [this, &events](const Path& prefix_path,
                                                    const Variant& node) {
    Extend(&events, server_sync_tree_->ApplyServerOverwrite(prefix_path, node));
    Path affected_path = AbortTransactions(prefix_path, kErrorOverriddenBySet);
    RerunTransactions(affected_path);
  });

  on_disconnect_.Clear();

  PostEvents(events);
}

void Repo::OnAuthStatus(bool auth_ok) {
  //
}

void Repo::OnServerInfoUpdate(int64_t timestamp_delta) {
  //
}

void Repo::OnDataUpdate(const Path& path, const Variant& data, bool is_merge,
                        const connection::PersistentConnection::Tag& tag) {
  std::vector<Event> events;
  if (is_merge) {
    std::map<Path, Variant> changed_children = VariantToPathMap(data);
    events = server_sync_tree_->ApplyServerMerge(path, changed_children);
  } else {
    events = server_sync_tree_->ApplyServerOverwrite(path, data);
  }
  if (events.size() > 0) {
    // Since we have a listener outstanding for each transaction, receiving any
    // events is a proxy for some change having occurred.
    RerunTransactions(path);
  }
  PostEvents(events);
}

void Repo::SetKeepSynchronized(const QuerySpec& query_spec,
                               bool keep_synchronized) {
  server_sync_tree_->SetKeepSynchronized(query_spec, keep_synchronized);
}

class NoopListener : public ValueListener {
 public:
  virtual ~NoopListener() {}
  void OnValueChanged(const DataSnapshot& snapshot) { (void)snapshot; }
  void OnCancelled(const Error& error, const char* error_message) {
    (void)error;
    (void)error_message;
  }
};

void Repo::StartTransaction(const Path& path,
                            DoTransactionWithContext transaction_function,
                            void* context, void (*delete_context)(void*),
                            bool trigger_local_events,
                            ReferenceCountedFutureImpl* api,
                            SafeFutureHandle<DataSnapshot> handle) {
  // Make sure we're listening on this node.
  // Note: we can't do this asynchronously. To preserve event ordering, it has
  // to be done in this block.  This is ok, this block is guaranteed to be our
  // own event loop
  DatabaseReferenceInternal* ref_impl =
      new DatabaseReferenceInternal(database_, path);
  DatabaseReference watch_ref(ref_impl);
  UniquePtr<NoopListener> listener = MakeUnique<NoopListener>();
  QuerySpec query_spec(path);
  AddEventCallback(MakeUnique<ValueEventRegistration>(database_, listener.get(),
                                                      query_spec));

  TransactionDataPtr transaction_data = MakeShared<TransactionData>(
      handle, api, query_spec.path, transaction_function, context,
      delete_context, trigger_local_events, std::move(listener));

  // Run transaction initially.
  Variant current_state = GetLatestState(path);
  transaction_data->current_input_snapshot = current_state;
  MutableDataInternal* mutable_data_impl =
      new MutableDataInternal(database_, current_state);
  MutableData mutable_current(mutable_data_impl);

  TransactionResult result = transaction_function(&mutable_current, context);
  if (result != kTransactionResultSuccess) {
    // Abort the transaction.
    transaction_data->current_output_snapshot_raw = Variant::Null();
    transaction_data->current_output_snapshot_resolved = Variant::Null();
    transaction_data->status = TransactionData::kStatusNeedsAbort;
    transaction_data->ref_future->Complete(transaction_data->future_handle,
                                           kErrorWriteCanceled);
  } else {
    // Mark as run and add to our queue.
    transaction_data->status = TransactionData::kStatusRun;

    auto* queue_node = transaction_queue_tree_.GetOrMakeSubtree(path);
    if (!queue_node->value().has_value()) {
      queue_node->set_value(std::vector<TransactionDataPtr>());
    }
    queue_node->value()->push_back(transaction_data);

    Variant server_values = GenerateServerValues();
    const Variant* new_node_unresolved = mutable_data_impl->GetNode();
    Variant new_node_resolved =
        ResolveDeferredValueSnapshot(*new_node_unresolved, server_values);

    transaction_data->current_output_snapshot_raw = *new_node_unresolved;
    transaction_data->current_output_snapshot_resolved = new_node_resolved;
    transaction_data->current_write_id = GetNextWriteId();

    std::vector<Event> events = server_sync_tree_->ApplyUserOverwrite(
        path, *new_node_unresolved, new_node_resolved,
        transaction_data->current_write_id,
        trigger_local_events ? kOverwriteVisible : kOverwriteInvisible,
        kDoNotPersist);

    PostEvents(events);

    SendAllReadyTransactions();
  }
}

void Repo::SendAllReadyTransactions() {
  PruneCompletedTransactions(&transaction_queue_tree_);
  SendReadyTransactions(&transaction_queue_tree_);
}

void Repo::SendReadyTransactions(Tree<std::vector<TransactionDataPtr>>* node) {
  Optional<std::vector<TransactionDataPtr>> queue = node->value();
  if (queue.has_value()) {
    queue = BuildTransactionQueue(node);
    assert(queue->size() != 0);

    bool all_run = true;
    for (const TransactionDataPtr& transaction : *queue) {
      if (transaction->status != TransactionData::kStatusRun) {
        all_run = false;
        break;
      }
    }
    // If they're all run (and not sent), we can send them.  Else, we must wait.
    if (all_run) {
      SendTransactionQueue(*queue, node->GetPath());
    }
  } else {
    for (auto& child : node->children()) {
      SendReadyTransactions(&child.second);
    }
  }
}

Path Repo::AbortTransactions(const Path& path, Error reason) {
  Path affected_path = GetAncestorTransactionNode(path)->GetPath();

  Tree<std::vector<TransactionDataPtr>>* transaction_node =
      transaction_queue_tree_.GetOrMakeSubtree(path);

  transaction_node->CallOnEachAncestor(
      [this, reason](Tree<std::vector<TransactionDataPtr>>* tree) {
        AbortTransactionsAtNode(tree, reason);
        return false;
      });

  AbortTransactionsAtNode(transaction_node, reason);

  transaction_node->CallOnEachDescendant(
      [this, reason](Tree<std::vector<TransactionDataPtr>>* tree) {
        AbortTransactionsAtNode(tree, reason);
      });

  return affected_path;
}

void Repo::AbortTransactionsAtNode(Tree<std::vector<TransactionDataPtr>>* node,
                                   Error reason) {
  Optional<std::vector<TransactionDataPtr>>& queue = node->value();
  std::vector<Event> events;

  if (queue.has_value()) {
    struct FutureToComplete {
      FutureToComplete(const TransactionDataPtr& transaction, Error abort_error)
          : transaction(transaction), abort_error(abort_error) {}
      TransactionDataPtr transaction;
      Error abort_error;
    };
    std::vector<FutureToComplete> futures_to_complete;

    Error abort_error;
    if (reason == kErrorOverriddenBySet) {
      abort_error = kErrorOverriddenBySet;
    } else {
      FIREBASE_DEV_ASSERT_MESSAGE(reason == kErrorWriteCanceled,
                                  "Unknown transaction abort reason");
      abort_error = kErrorWriteCanceled;
    }

    std::vector<TransactionDataPtr>::iterator last_sent = queue->begin() - 1;
    for (auto iter = queue->begin(); iter != queue->end(); ++iter) {
      const TransactionDataPtr& transaction = *iter;
      if (transaction->status == TransactionData::kStatusSentNeedsAbort) {
        // No-op. Already marked
      } else if (transaction->status == TransactionData::kStatusSent) {
        FIREBASE_DEV_ASSERT_MESSAGE(
            last_sent == iter - 1,
            "All sent items should be at beginning of queue.");
        last_sent = iter;
        // Mark transaction for abort when it comes back.
        transaction->status = TransactionData::kStatusSentNeedsAbort;
        transaction->abort_reason = abort_error;
      } else {
        FIREBASE_DEV_ASSERT_MESSAGE(
            transaction->status == TransactionData::kStatusRun,
            "Unexpected transaction status in abort");
        // We can abort this immediately.
        RemoveEventCallback(transaction->outstanding_listener.get(),
                            QuerySpec(transaction->path));
        if (reason == kErrorOverriddenBySet) {
          Extend(&events,
                 server_sync_tree_->AckUserWrite(transaction->current_write_id,
                                                 kAckRevert, kDoNotPersist));
        } else {
          FIREBASE_DEV_ASSERT_MESSAGE(reason == kErrorWriteCanceled,
                                      "Unknown transaction abort reason");
          // If it was cancelled, it was already removed from the sync tree
        }
        futures_to_complete.push_back(
            FutureToComplete(transaction, abort_error));
      }
    }

    if (last_sent == queue->begin() - 1) {
      // We're not waiting for any sent transactions. We can clear the queue
      node->value().reset();
    } else {
      // Remove the transactions we aborted
      queue->erase(queue->begin(), last_sent + 1);
    }

    // Now fire the callbacks.
    PostEvents(events);

    for (auto& future_to_complete : futures_to_complete) {
      TransactionDataPtr& transaction = future_to_complete.transaction;
      Error abort_error = future_to_complete.abort_error;

      transaction->ref_future->Complete(transaction->future_handle, abort_error,
                                        GetErrorMessage(abort_error));
    }
  }
}

WriteId Repo::GetNextWriteId() { return next_write_id_++; }

Path Repo::RerunTransactions(const Path& changed_path) {
  Tree<std::vector<TransactionDataPtr>>* root_most_transaction_node =
      GetAncestorTransactionNode(changed_path);
  Path path = root_most_transaction_node->GetPath();

  std::vector<TransactionDataPtr> queue =
      BuildTransactionQueue(root_most_transaction_node);
  RerunTransactionQueue(queue, path);

  return path;
}

void Repo::SendTransactionQueue(const std::vector<TransactionDataPtr>& queue,
                                const Path& path) {
  assert(!queue.empty());
  LogDebug("SendTransactionQueue @ %s (# of transaction : %d)", path.c_str(),
           static_cast<int>(queue.size()));

  std::vector<WriteId> sets_to_ignore;
  for (const TransactionDataPtr& transaction : queue) {
    sets_to_ignore.push_back(transaction->current_write_id);
  }

  // Get the value of the location before the change.  Get it from the
  // listener of the first transaction for sake of ease.
  Variant latest_state = GetLatestState(path, sets_to_ignore);
  Variant snap_to_send = latest_state;
  if (HasVector(latest_state)) {
    ConvertVectorToMap(&latest_state);
  }

  std::string hash;
  GetHash(latest_state, &hash);

  // Get the final result from all the transaction from current location and
  // child location.
  // Note that this part will not be correct until SyncTree is ready
  // because the transactions which run on child location always get
  // out-of-date local cache until server pushes the update
  for (const TransactionDataPtr& transaction : queue) {
    FIREBASE_DEV_ASSERT_MESSAGE(
        transaction->status == TransactionData::kStatusRun,
        "Cannot send a transaction that is not running!");
    transaction->status = TransactionData::kStatusSent;
    ++transaction->retry_count;
    Optional<Path> relative_path = Path::GetRelative(path, transaction->path);
    FIREBASE_DEV_ASSERT(relative_path.has_value());
    SetVariantAtPath(&snap_to_send, *relative_path,
                     transaction->current_output_snapshot_raw);
  }

  connection::ResponsePtr response = MakeShared<TransactionResponse>(
      safe_this_, path, queue, [](const connection::ResponsePtr& ptr) {
        TransactionResponse* response =
            static_cast<TransactionResponse*>(ptr.get());

        FIREBASE_DEV_ASSERT(response);

        ThisRefLock lock(&response->repo_ref());
        if (lock.GetReference() != nullptr) {
          lock.GetReference()->HandleTransactionResponse(ptr);
        }
      });

  connection_->CompareAndPut(path, snap_to_send, hash, response);
}

void Repo::HandleTransactionResponse(const connection::ResponsePtr& ptr) {
  TransactionResponse* response = static_cast<TransactionResponse*>(ptr.get());
  const Path& path = response->path();

  assert(response);

  std::vector<Event> events;

  if (!response->HasError()) {
    struct FutureToComplete {
      FutureToComplete(const TransactionDataPtr& transaction,
                       const Variant& node)
          : transaction(transaction), node(node) {}
      TransactionDataPtr transaction;
      Variant node;
    };
    std::vector<FutureToComplete> futures_to_complete;
    futures_to_complete.reserve(response->queue().size());

    for (auto& transaction : response->queue()) {
      transaction->status = TransactionData::kStatusComplete;

      events = server_sync_tree_->AckUserWrite(transaction->current_write_id,
                                               kAckConfirm, kDoNotPersist);

      futures_to_complete.push_back(FutureToComplete(
          transaction, transaction->current_output_snapshot_resolved));

      RemoveEventCallback(transaction->outstanding_listener.get(),
                          QuerySpec(path));
    }

    // Now remove the completed transactions.
    PruneCompletedTransactions(transaction_queue_tree_.GetChild(path));

    // There may be pending transactions that we can now send
    SendAllReadyTransactions();

    // Fire the appropriate events to the listeners.
    PostEvents(events);

    // Finally, complete the futures.
    for (auto& future_to_complete : futures_to_complete) {
      TransactionDataPtr& transaction = future_to_complete.transaction;
      const Variant& node = future_to_complete.node;

      DataSnapshot snapshot(
          new DataSnapshotInternal(database_, transaction->path, node));
      transaction->ref_future->CompleteWithResult(transaction->future_handle,
                                                  kErrorNone, snapshot);
    }
  } else {
    // Transactions are no longer sent. Update their status appropriately.
    if (response->GetErrorCode() == kErrorDataStale) {
      for (auto& transaction : response->queue()) {
        if (transaction->status == TransactionData::kStatusSentNeedsAbort) {
          transaction->status = TransactionData::kStatusNeedsAbort;
        } else {
          transaction->status = TransactionData::kStatusRun;
        }
      }
    } else {
      for (auto& transaction : response->queue()) {
        transaction->status = TransactionData::kStatusNeedsAbort;
        transaction->abort_reason = kErrorUnknownError;
      }
    }

    RerunTransactions(response->path());
  }
}

void Repo::RerunTransactionQueue(const std::vector<TransactionDataPtr>& queue,
                                 const Path& path) {
  LogDebug("RerunTransactionQueue @ %s (# of transaction : %d)", path.c_str(),
           static_cast<int>(queue.size()));

  if (queue.empty()) {
    // Nothing to do!
    return;
  }

  struct FutureToComplete {
    FutureToComplete(TransactionDataPtr transaction, Error abort_reason,
                     Variant node)
        : transaction(transaction), abort_reason(abort_reason), node(node) {}
    TransactionDataPtr transaction;
    Error abort_reason;
    Variant node;
  };
  std::vector<FutureToComplete> futures_to_complete;

  std::vector<WriteId> sets_to_ignore;
  for (const TransactionDataPtr& transaction : queue) {
    sets_to_ignore.push_back(transaction->current_write_id);
  }

  for (const TransactionDataPtr& transaction : queue) {
    Optional<Path> relative_path = Path::GetRelative(path, transaction->path);
    assert(relative_path.has_value());

    bool abort_transaction = false;
    Error abort_reason = kErrorNone;
    std::vector<Event> events;

    if (transaction->status == TransactionData::kStatusNeedsAbort) {
      abort_transaction = true;
      abort_reason = transaction->abort_reason;
      if (abort_reason != kErrorWriteCanceled) {
        Extend(&events,
               server_sync_tree_->AckUserWrite(transaction->current_write_id,
                                               kAckRevert, kDoNotPersist));
      }
    } else if (transaction->status == TransactionData::kStatusRun) {
      if (transaction->retry_count >= TransactionData::kTransactionMaxRetries) {
        abort_transaction = true;
        abort_reason = kErrorMaxRetries;
        Extend(&events,
               server_sync_tree_->AckUserWrite(transaction->current_write_id,
                                               kAckRevert, kDoNotPersist));
      } else {
        // This code rerun a transaction
        Variant current_input =
            GetLatestState(transaction->path, sets_to_ignore);
        // TODO(chkuang): Make sure the local cache does not contain vector.
        //                Gently convert everything for now.
        if (HasVector(current_input)) {
          ConvertVectorToMap(&current_input);
        }

        transaction->current_input_snapshot = current_input;

        MutableDataInternal* mutable_data_impl =
            new MutableDataInternal(database_, current_input);
        MutableData mutable_data(mutable_data_impl);
        Error error = kErrorNone;
        TransactionResult result = transaction->transaction_function(
            &mutable_data, transaction->context);
        if (result == kTransactionResultSuccess) {
          WriteId old_write_id = transaction->current_write_id;

          Variant server_values = GenerateServerValues();
          Variant* new_data_node = mutable_data_impl->GetNode();
          Variant new_node_resolved =
              ResolveDeferredValueSnapshot(*new_data_node, server_values);

          transaction->current_output_snapshot_raw = *new_data_node;
          transaction->current_output_snapshot_resolved = new_node_resolved;
          transaction->current_write_id = GetNextWriteId();

          sets_to_ignore.push_back(old_write_id);
          Extend(&events,
                 server_sync_tree_->ApplyUserOverwrite(
                     transaction->path, *new_data_node, new_node_resolved,
                     transaction->current_write_id,
                     transaction->trigger_local_events ? kOverwriteVisible
                                                       : kOverwriteInvisible,
                     kPersist));
          Extend(&events, server_sync_tree_->AckUserWrite(
                              old_write_id, kAckRevert, kDoNotPersist));
        } else {
          abort_transaction = true;
          abort_reason = error;
          Extend(&events,
                 server_sync_tree_->AckUserWrite(transaction->current_write_id,
                                                 kAckRevert, kDoNotPersist));
        }
      }
    }

    PostEvents(events);

    if (abort_transaction) {
      transaction->status = TransactionData::kStatusComplete;
      DatabaseReferenceInternal* database_ref_impl =
          new DatabaseReferenceInternal(database_, path);
      DatabaseReference ref(database_ref_impl);

      futures_to_complete.push_back(FutureToComplete(
          transaction, abort_reason, transaction->current_input_snapshot));

      // Removing a callback can trigger pruning which can muck with
      // merged_data/visible_data (as it prunes data). So defer removing the
      // callback until later.
      Repo::scheduler().Schedule(NewCallback(
          [](Repo* repo, TransactionDataPtr transaction) {
            repo->RemoveEventCallback(transaction->outstanding_listener.get(),
                                      QuerySpec(transaction->path));
          },
          this, transaction));
    }
  }
  PruneCompletedTransactions(&transaction_queue_tree_);

  for (auto& future_to_complete : futures_to_complete) {
    TransactionDataPtr& transaction = future_to_complete.transaction;
    Error& abort_reason = future_to_complete.abort_reason;
    Variant& node = future_to_complete.node;
    DataSnapshot snapshot(
        new DataSnapshotInternal(database_, transaction->path, node));
    transaction->ref_future->CompleteWithResult(transaction->future_handle,
                                                abort_reason, snapshot);
  }

  SendAllReadyTransactions();
}

Variant Repo::GetLatestState(const Path& path,
                             std::vector<WriteId> sets_to_ignore) {
  Optional<Variant> state =
      server_sync_tree_->CalcCompleteEventCache(path, sets_to_ignore);
  return state.has_value() ? *state : Variant::Null();
}

void Repo::PruneCompletedTransactions(
    Tree<std::vector<TransactionDataPtr>>* node) {
  Optional<std::vector<TransactionDataPtr>>& queue = node->value();
  if (queue.has_value()) {
    queue->erase(std::remove_if(queue->begin(), queue->end(),
                                [](const TransactionDataPtr& transaction) {
                                  return transaction->status ==
                                         TransactionData::kStatusComplete;
                                }),
                 queue->end());
    if (queue->empty()) {
      node->value().reset();
    }
  }
  for (auto& key_subtree_pair : node->children()) {
    auto& subtree = key_subtree_pair.second;
    PruneCompletedTransactions(&subtree);
  }
}

Tree<std::vector<TransactionDataPtr>>* Repo::GetAncestorTransactionNode(
    Path path) {
  Tree<std::vector<TransactionDataPtr>>* transaction_node =
      &transaction_queue_tree_;
  while (!path.empty() && !transaction_node->value().has_value()) {
    transaction_node =
        transaction_node->GetOrMakeSubtree(path.FrontDirectory());
    path = path.PopFrontDirectory();
  }
  return transaction_node;
}

std::vector<TransactionDataPtr> Repo::BuildTransactionQueue(
    Tree<std::vector<TransactionDataPtr>>* transaction_node) {
  std::vector<TransactionDataPtr> queue;
  AggregateTransactionQueues(&queue, transaction_node);

  std::sort(queue.begin(), queue.end(),
            [](const TransactionDataPtr& a, const TransactionDataPtr& b) {
              return *a < *b;
            });

  return queue;
}

void Repo::AggregateTransactionQueues(
    std::vector<TransactionDataPtr>* queue,
    Tree<std::vector<TransactionDataPtr>>* node) {
  Optional<std::vector<TransactionDataPtr>>& child_queue = node->value();
  if (child_queue.has_value()) {
    Extend(queue, *child_queue);
  }

  for (auto& key_subtree_pair : node->children()) {
    Tree<std::vector<TransactionDataPtr>>& subtree = key_subtree_pair.second;
    AggregateTransactionQueues(queue, &subtree);
  }
}

}  // namespace internal
}  // namespace database
}  // namespace firebase