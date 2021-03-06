// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/child_broker.h"

#include "base/bind.h"
#include "base/logging.h"
#include "mojo/edk/embedder/embedder_internal.h"
#include "mojo/edk/embedder/platform_channel_pair.h"
#include "mojo/edk/system/broker_messages.h"
#include "mojo/edk/system/message_pipe_dispatcher.h"

namespace mojo {
namespace edk {

ChildBroker* ChildBroker::GetInstance() {
  return base::Singleton<
      ChildBroker, base::LeakySingletonTraits<ChildBroker>>::get();
}

void ChildBroker::SetChildBrokerHostHandle(ScopedPlatformHandle handle)  {
  ScopedPlatformHandle parent_async_channel_handle;
#if defined(OS_POSIX)
  parent_async_channel_handle = handle.Pass();
#else
  // On Windows we have two pipes to the parent. The first is for the token
  // exchange for creating and passing handles, since the child needs the
  // parent's help if it is sandboxed. The second is the same as POSIX, which is
  // used for multiplexing related messages. So on Windows, we send the second
  // pipe as the first string over the first one.
  parent_sync_channel_ = handle.Pass();

  HANDLE parent_handle = INVALID_HANDLE_VALUE;
  DWORD bytes_read = 0;
  BOOL rv = ReadFile(parent_sync_channel_.get().handle, &parent_handle,
                     sizeof(parent_handle), &bytes_read, NULL);
  CHECK(rv);
  parent_async_channel_handle.reset(PlatformHandle(parent_handle));
  sync_channel_lock_.Unlock();
#endif

  internal::g_io_thread_task_runner->PostTask(
      FROM_HERE,
      base::Bind(&ChildBroker::InitAsyncChannel, base::Unretained(this),
                 base::Passed(&parent_async_channel_handle)));
}

#if defined(OS_WIN)
void ChildBroker::CreatePlatformChannelPair(
    ScopedPlatformHandle* server, ScopedPlatformHandle* client) {
  sync_channel_lock_.Lock();
  CreatePlatformChannelPairNoLock(server, client);
  sync_channel_lock_.Unlock();
}

void ChildBroker::HandleToToken(const PlatformHandle* platform_handles,
                                size_t count,
                                uint64_t* tokens) {
  uint32_t size = kBrokerMessageHeaderSize +
                  static_cast<int>(count) * sizeof(HANDLE);
  std::vector<char> message_buffer(size);
  BrokerMessage* message = reinterpret_cast<BrokerMessage*>(&message_buffer[0]);
  message->size = size;
  message->id = HANDLE_TO_TOKEN;
  for (size_t i = 0; i < count; ++i)
    message->handles[i] = platform_handles[i].handle;

  uint32_t response_size = static_cast<int>(count) * sizeof(uint64_t);
  sync_channel_lock_.Lock();
  WriteAndReadResponse(message, tokens, response_size);
  sync_channel_lock_.Unlock();
}

void ChildBroker::TokenToHandle(const uint64_t* tokens,
                                size_t count,
                                PlatformHandle* handles) {
  uint32_t size = kBrokerMessageHeaderSize +
                  static_cast<int>(count) * sizeof(uint64_t);
  std::vector<char> message_buffer(size);
  BrokerMessage* message =
      reinterpret_cast<BrokerMessage*>(&message_buffer[0]);
  message->size = size;
  message->id = TOKEN_TO_HANDLE;
  memcpy(&message->tokens[0], tokens, count * sizeof(uint64_t));

  std::vector<HANDLE> handles_temp(count);
  uint32_t response_size =
      static_cast<uint32_t>(handles_temp.size()) * sizeof(HANDLE);
  sync_channel_lock_.Lock();
  if (WriteAndReadResponse(message, &handles_temp[0], response_size)) {
    for (uint32_t i = 0; i < count; ++i)
      handles[i].handle = handles_temp[i];
    sync_channel_lock_.Unlock();
  }
}
#endif

void ChildBroker::ConnectMessagePipe(uint64_t pipe_id,
                                     MessagePipeDispatcher* message_pipe) {
  DCHECK(internal::g_io_thread_task_runner->RunsTasksOnCurrentThread());

  ConnectMessagePipeMessage data;
  data.pipe_id = pipe_id;
  if (pending_connects_.find(pipe_id) != pending_connects_.end()) {
    if (!parent_async_channel_) {
      // On Windows, we can't create the local RoutedRawChannel yet because we
      // don't have parent_sync_channel_. Treat all platforms the same and just
      // queue this.
      CHECK(pending_inprocess_connects_.find(pipe_id) ==
            pending_inprocess_connects_.end());
      pending_inprocess_connects_[pipe_id] = message_pipe;
      return;
    }
    // Both ends of the message pipe are in the same process.
    // First, tell the browser side that to remove its bookkeeping for a pending
    // connect, since it'll never get the other side.

    data.type = CANCEL_CONNECT_MESSAGE_PIPE;
    scoped_ptr<MessageInTransit> message(new MessageInTransit(
        MessageInTransit::Type::MESSAGE, sizeof(data), &data));
    WriteAsyncMessage(message.Pass());

    if (!in_process_pipes_channel1_) {
      ScopedPlatformHandle server_handle, client_handle;
#if defined(OS_WIN)
      CreatePlatformChannelPairNoLock(&server_handle, &client_handle);
#else
      PlatformChannelPair channel_pair;
      server_handle = channel_pair.PassServerHandle();
      client_handle = channel_pair.PassClientHandle();
#endif
      in_process_pipes_channel1_ = new RoutedRawChannel(
          server_handle.Pass(),
          base::Bind(&ChildBroker::ChannelDestructed, base::Unretained(this)));
      in_process_pipes_channel2_ = new RoutedRawChannel(
          client_handle.Pass(),
          base::Bind(&ChildBroker::ChannelDestructed, base::Unretained(this)));
    }

    connected_pipes_[pending_connects_[pipe_id]] = in_process_pipes_channel1_;
    connected_pipes_[message_pipe] = in_process_pipes_channel2_;
    in_process_pipes_channel1_->AddRoute(pipe_id, pending_connects_[pipe_id]);
    in_process_pipes_channel2_->AddRoute(pipe_id, message_pipe);
    pending_connects_[pipe_id]->GotNonTransferableChannel(
        in_process_pipes_channel1_->channel());
    message_pipe->GotNonTransferableChannel(
        in_process_pipes_channel2_->channel());

    pending_connects_.erase(pipe_id);
    return;
  }

  data.type = CONNECT_MESSAGE_PIPE;
  scoped_ptr<MessageInTransit> message(new MessageInTransit(
      MessageInTransit::Type::MESSAGE, sizeof(data), &data));
  pending_connects_[pipe_id] = message_pipe;
  WriteAsyncMessage(message.Pass());
}

void ChildBroker::CloseMessagePipe(
    uint64_t pipe_id, MessagePipeDispatcher* message_pipe) {
  DCHECK(internal::g_io_thread_task_runner->RunsTasksOnCurrentThread());
  CHECK(connected_pipes_.find(message_pipe) != connected_pipes_.end());
  connected_pipes_[message_pipe]->RemoveRoute(pipe_id);
  connected_pipes_.erase(message_pipe);
}

ChildBroker::ChildBroker()
    : parent_async_channel_(nullptr),
      in_process_pipes_channel1_(nullptr),
      in_process_pipes_channel2_(nullptr) {
  DCHECK(!internal::g_broker);
  internal::g_broker = this;
#if defined(OS_WIN)
  // Block any threads from calling this until we have a pipe to the parent.
  sync_channel_lock_.Lock();
#endif
}

ChildBroker::~ChildBroker() {
}

void ChildBroker::OnReadMessage(
    const MessageInTransit::View& message_view,
    ScopedPlatformHandleVectorPtr platform_handles) {
  DCHECK(internal::g_io_thread_task_runner->RunsTasksOnCurrentThread());
  MultiplexMessages type =
      *static_cast<const MultiplexMessages*>(message_view.bytes());
  if (type == CONNECT_TO_PROCESS) {
    DCHECK_EQ(platform_handles->size(), 1u);
    ScopedPlatformHandle handle((*platform_handles.get())[0]);
    (*platform_handles.get())[0] = PlatformHandle();

    const ConnectToProcessMessage* message =
        static_cast<const ConnectToProcessMessage*>(message_view.bytes());

    CHECK(channels_.find(message->process_id) == channels_.end());
    channels_[message->process_id] = new RoutedRawChannel(
        handle.Pass(),
        base::Bind(&ChildBroker::ChannelDestructed, base::Unretained(this)));
  } else if (type == PEER_PIPE_CONNECTED) {
    DCHECK(!platform_handles);
    const PeerPipeConnectedMessage* message =
        static_cast<const PeerPipeConnectedMessage*>(message_view.bytes());

    uint64_t pipe_id = message->pipe_id;
    uint64_t peer_pid = message->process_id;

    CHECK(pending_connects_.find(pipe_id) != pending_connects_.end());
    MessagePipeDispatcher* pipe = pending_connects_[pipe_id];
    pending_connects_.erase(pipe_id);
    if (peer_pid == 0) {
      // The other side is in the parent process.
      connected_pipes_[pipe] = parent_async_channel_;
      parent_async_channel_->AddRoute(pipe_id, pipe);
      pipe->GotNonTransferableChannel(parent_async_channel_->channel());
    } else if (channels_.find(peer_pid) == channels_.end()) {
      // We saw the peer process die before we got the reply from the parent.
      pipe->OnError(ERROR_READ_SHUTDOWN);
    } else {
      CHECK(connected_pipes_.find(pipe) == connected_pipes_.end());
      connected_pipes_[pipe] = channels_[peer_pid];
      channels_[peer_pid]->AddRoute(pipe_id, pipe);
      pipe->GotNonTransferableChannel(channels_[peer_pid]->channel());
    }
  } else {
    NOTREACHED();
  }
}

void ChildBroker::OnError(Error error) {
  // The parent process shut down.
}

void ChildBroker::ChannelDestructed(RoutedRawChannel* channel) {
  DCHECK(internal::g_io_thread_task_runner->RunsTasksOnCurrentThread());
  for (auto it : channels_) {
    if (it.second == channel) {
      channels_.erase(it.first);
      break;
    }
  }
}

void ChildBroker::WriteAsyncMessage(scoped_ptr<MessageInTransit> message) {
  DCHECK(internal::g_io_thread_task_runner->RunsTasksOnCurrentThread());
  message->set_route_id(kBrokerRouteId);
  if (parent_async_channel_) {
    parent_async_channel_->channel()->WriteMessage(message.Pass());
  } else {
    async_channel_queue_.AddMessage(message.Pass());
  }
}

void ChildBroker::InitAsyncChannel(
    ScopedPlatformHandle parent_async_channel_handle) {
  DCHECK(internal::g_io_thread_task_runner->RunsTasksOnCurrentThread());

  parent_async_channel_ = new RoutedRawChannel(
      parent_async_channel_handle.Pass()  ,
      base::Bind(&ChildBroker::ChannelDestructed, base::Unretained(this)));
  parent_async_channel_->AddRoute(kBrokerRouteId, this);
  while (!async_channel_queue_.IsEmpty()) {
    parent_async_channel_->channel()->WriteMessage(
        async_channel_queue_.GetMessage());
  }

  while (!pending_inprocess_connects_.empty()) {
    ConnectMessagePipe(pending_inprocess_connects_.begin()->first,
                       pending_inprocess_connects_.begin()->second);
    pending_inprocess_connects_.erase(pending_inprocess_connects_.begin());
  }
}

#if defined(OS_WIN)

bool ChildBroker::WriteAndReadResponse(BrokerMessage* message,
                                       void* response,
                                       uint32_t response_size) {
  CHECK(parent_sync_channel_.is_valid());

  bool result = true;
  DWORD bytes_written = 0;
  // This will always write in one chunk per
  // https://msdn.microsoft.com/en-us/library/windows/desktop/aa365150.aspx.
  BOOL rv = WriteFile(parent_sync_channel_.get().handle, message, message->size,
                      &bytes_written, NULL);
  if (!rv || bytes_written != message->size) {
    LOG(ERROR) << "Child token serializer couldn't write message.";
    result = false;
  } else {
    while (response_size) {
      DWORD bytes_read = 0;
      rv = ReadFile(parent_sync_channel_.get().handle, response, response_size,
                    &bytes_read, NULL);
      if (!rv) {
        LOG(ERROR) << "Child token serializer couldn't read result.";
        result = false;
        break;
      }
      response_size -= bytes_read;
      response = static_cast<char*>(response) + bytes_read;
    }
  }

  return result;
}

void ChildBroker::CreatePlatformChannelPairNoLock(
    ScopedPlatformHandle* server, ScopedPlatformHandle* client) {
  BrokerMessage message;
  message.size = kBrokerMessageHeaderSize;
  message.id = CREATE_PLATFORM_CHANNEL_PAIR;

  uint32_t response_size = 2 * sizeof(HANDLE);
  HANDLE handles[2];
  if (WriteAndReadResponse(&message, handles, response_size)) {
    server->reset(PlatformHandle(handles[0]));
    client->reset(PlatformHandle(handles[1]));
  }
}

#endif

}  // namespace edk
}  // namespace mojo
