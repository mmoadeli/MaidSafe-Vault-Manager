/*  Copyright 2014 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/vault_manager/tcp_connection.h"

#include <condition_variable>

#include "boost/asio/error.hpp"
#include "boost/asio/read.hpp"
#include "boost/asio/write.hpp"

#include "maidsafe/common/log.h"
#include "maidsafe/common/on_scope_exit.h"
#include "maidsafe/common/utils.h"

namespace asio = boost::asio;
namespace ip = asio::ip;
namespace args = std::placeholders;

namespace maidsafe {

namespace vault_manager {

TcpConnection::TcpConnection(AsioService& asio_service)
    : io_service_(asio_service.service()),
      start_flag_(),
      socket_close_flag_(),
      socket_(io_service_),
      on_message_received_(),
      on_connection_closed_(),
      receiving_message_(),
      send_queue_() {
  static_assert((sizeof(DataSize)) == 4, "DataSize must be 4 bytes.");
  assert(!socket_.is_open());
  if (asio_service.ThreadCount() != 1U) {
    LOG(kError) << "This must be a single-threaded io_service, or an asio strand will be required.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }
}

TcpConnection::TcpConnection(AsioService& asio_service, uint16_t remote_port)
    : io_service_(asio_service.service()),
      start_flag_(),
      socket_close_flag_(),
      socket_(io_service_),
      on_message_received_(),
      on_connection_closed_(),
      receiving_message_(),
      send_queue_() {
  if (asio_service.ThreadCount() != 1U) {
    LOG(kError) << "This must be a single-threaded io_service, or an asio strand will be required.";
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_parameter));
  }

  boost::system::error_code ec;
  // Try IPv6 first.
  socket_.connect(ip::tcp::endpoint{ ip::address_v6::loopback(), remote_port }, ec);
  if (ec && ec == asio::error::make_error_code(asio::error::address_family_not_supported))
    // Try IPv4 now.
    socket_.connect(ip::tcp::endpoint{ ip::address_v4::loopback(), remote_port }, ec);
  if (!socket_.is_open()) {
    LOG(kError) << "Failed to connect to " << remote_port << ": " << ec.message();
    BOOST_THROW_EXCEPTION(MakeError(VaultManagerErrors::failed_to_connect));
  }
}

TcpConnectionPtr TcpConnection::MakeShared(AsioService& asio_service) {
  return TcpConnectionPtr{ new TcpConnection{ asio_service } };
}

TcpConnectionPtr TcpConnection::MakeShared(AsioService& asio_service, uint16_t remote_port) {
  return TcpConnectionPtr{ new TcpConnection{ asio_service, remote_port } };
}

void TcpConnection::Start(MessageReceivedFunctor on_message_received,
                          ConnectionClosedFunctor on_connection_closed) {
  std::call_once(start_flag_, [=] {
    on_message_received_ = on_message_received;
    on_connection_closed_ = on_connection_closed;
    TcpConnectionPtr this_ptr{ shared_from_this() };
    io_service_.dispatch([this_ptr] { this_ptr->ReadSize(); });
  });
}

void TcpConnection::Close() {
  TcpConnectionPtr this_ptr{ shared_from_this() };
  io_service_.post([this_ptr] { this_ptr->DoClose(); });
}

void TcpConnection::DoClose() {
  std::call_once(socket_close_flag_, [this] {
    boost::system::error_code ignored_ec;
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ignored_ec);
    socket_.close(ignored_ec);
    if (on_connection_closed_)
      on_connection_closed_();
  });
}

void TcpConnection::ReadSize() {
  TcpConnectionPtr this_ptr{ shared_from_this() };
  asio::async_read(socket_, asio::buffer(receiving_message_.size_buffer),
                   [this_ptr](const boost::system::error_code& ec, size_t bytes_transferred) {
    if (ec) {
      LOG(kInfo) << ec.message();
      return this_ptr->DoClose();
    }
    assert(bytes_transferred == 4U);
    static_cast<void>(bytes_transferred);

    DataSize data_size;
    data_size = (((((this_ptr->receiving_message_.size_buffer[0] << 8) |
                     this_ptr->receiving_message_.size_buffer[1]) << 8) |
                     this_ptr->receiving_message_.size_buffer[2]) << 8) |
                     this_ptr->receiving_message_.size_buffer[3];
    if (data_size > MaxMessageSize()) {
      LOG(kError) << "Incoming message size of " << data_size
                  << " bytes exceeds maximum allowed of " << MaxMessageSize() << " bytes.";
      this_ptr->receiving_message_.data_buffer.clear();
      return this_ptr->DoClose();
    }

    this_ptr->receiving_message_.data_buffer.resize(data_size);
    this_ptr->ReadData();
  });
}

void TcpConnection::ReadData() {
  TcpConnectionPtr this_ptr{ shared_from_this() };
  asio::async_read(socket_, asio::buffer(receiving_message_.data_buffer), io_service_.wrap(
                   [this_ptr](const boost::system::error_code& ec, size_t bytes_transferred) {
    if (ec) {
      LOG(kError) << "Failed to read message body: " << ec.message();
      return this_ptr->DoClose();
    }
    assert(bytes_transferred == this_ptr->receiving_message_.data_buffer.size());
    static_cast<void>(bytes_transferred);

    // Dispatch the message outside the strand.
    std::string data{ std::begin(this_ptr->receiving_message_.data_buffer),
                      std::end(this_ptr->receiving_message_.data_buffer) };
    this_ptr->io_service_.post([=] { this_ptr->on_message_received_(std::move(data)); });
    this_ptr->io_service_.dispatch([this_ptr] { this_ptr->ReadSize(); });
  }));
}

void TcpConnection::Send(std::string data) {
  SendingMessage message(EncodeData(std::move(data)));
  TcpConnectionPtr this_ptr{ shared_from_this() };
  io_service_.post([this_ptr, message] {
    bool currently_sending{ !this_ptr->send_queue_.empty() };
    this_ptr->send_queue_.emplace_back(std::move(message));
    if (!currently_sending)
      this_ptr->DoSend();
  });
}

void TcpConnection::DoSend() {
  std::array<asio::const_buffer, 2> buffers;
  buffers[0] = asio::buffer(send_queue_.front().size_buffer);
  buffers[1] = asio::buffer(send_queue_.front().data.data(), send_queue_.front().data.size());
  TcpConnectionPtr this_ptr{ shared_from_this() };
  asio::async_write(socket_, buffers, io_service_.wrap(
                    [this_ptr](const boost::system::error_code& ec, size_t bytes_transferred) {
    if (ec) {
      LOG(kError) << "Failed to send message: " << ec.message();
      return this_ptr->DoClose();
    }
    assert(bytes_transferred == this_ptr->send_queue_.front().size_buffer.size() +
                                this_ptr->send_queue_.front().data.size());
    static_cast<void>(bytes_transferred);

    this_ptr->send_queue_.pop_front();
    if (!this_ptr->send_queue_.empty())
      this_ptr->DoSend();
  }));
}

TcpConnection::SendingMessage TcpConnection::EncodeData(std::string data) const {
  if (data.empty())
    BOOST_THROW_EXCEPTION(MakeError(CommonErrors::invalid_string_size));
  if (data.size() > MaxMessageSize())
    BOOST_THROW_EXCEPTION(MakeError(VaultManagerErrors::ipc_message_too_large));

  SendingMessage message;
  for (int i = 0; i != 4; ++i)
    message.size_buffer[i] = static_cast<char>(data.size() >> (8 * (3 - i)));
  message.data = std::move(data);

  return message;
}

}  // namespace vault_manager

}  // namespace maidsafe
