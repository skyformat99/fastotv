/*  Copyright (C) 2014-2016 FastoGT. All right reserved.

    This file is part of SiteOnYourDevice.

    SiteOnYourDevice is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    SiteOnYourDevice is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with SiteOnYourDevice.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "server/inner/inner_tcp_handler.h"

#include <string>
#include <vector>

extern "C" {
#include "sds.h"
}
#undef ERROR
#include <common/net/net.h>
#include <common/logger.h>
#include <common/threads/thread_manager.h>
#include <common/convert2string.h>

#include "server/server_host.h"
#include "server/server_commands.h"

#include "server/inner/inner_tcp_client.h"

namespace fasto {
namespace fastotv {
namespace server {
namespace inner {

class InnerTcpHandlerHost::InnerSubHandler : public RedisSubHandler {
 public:
  explicit InnerSubHandler(InnerTcpHandlerHost* parent) : parent_(parent) {}

  void processSubscribed(cmd_seq_t request_id, int argc, char* argv[]) {
    sds join = sdsempty();
    join = sdscatfmt(join, "%s ", request_id.c_str());
    for (int i = 0; i < argc; ++i) {
      join = sdscat(join, argv[i]);
      if (i != argc - 1) {
        join = sdscatlen(join, " ", 1);
      }
    }

    publish_command_out(join, sdslen(join));
    sdsfree(join);
  }

  void publish_command_out(const std::string& msg) {
    publish_command_out(msg.c_str(), msg.length());
  }

  void publish_command_out(const char* msg, size_t msg_len) {
    bool res = parent_->sub_commands_in_->publish_command_out(msg, msg_len);
    if (!res) {
      std::string err_str = common::MemSPrintf(
          "publish_command_out with args: msg = %s, msg_len = " PRIu64 " failed!", msg, msg_len);
      ERROR_LOG() << err_str;
    }
  }

  virtual void handleMessage(char* channel,
                             size_t channel_len,
                             char* msg,
                             size_t msg_len) override {
    // [std::string]site [hex_string]seq [std::string]command args ...
    // [hex_string]seq OK/FAIL [std::string]command args ..
    INFO_LOG() << "InnerSubHandler channel: " << channel << ", msg: " << msg;
    char* space = strchr(msg, ' ');
    if (!space) {
      const std::string resp = common::MemSPrintf("UNKNOWN COMMAND: %s", msg);
      WARNING_LOG() << resp;
      publish_command_out(resp);
      return;
    }

    *space = 0;

    char buff[MAX_COMMAND_SIZE] = {0};
    int len = snprintf(buff, sizeof(buff), STRINGIZE(REQUEST_COMMAND) " %s" END_OF_COMMAND,
                       space + 1);  // only REQUEST_COMMAND

    char* star_seq = NULL;
    cmd_id_t seq = strtoul(buff, &star_seq, 10);
    if (*star_seq != ' ') {
      std::string resp = common::MemSPrintf("PROBLEM EXTRACTING SEQUENCE: %s", space + 1);
      WARNING_LOG() << resp;
      publish_command_out(resp);
      return;
    }

    const char* id_ptr = strchr(star_seq + 1, ' ');
    if (!id_ptr) {
      std::string resp = common::MemSPrintf("PROBLEM EXTRACTING ID: %s", space + 1);
      WARNING_LOG() << resp;
      publish_command_out(resp);
      return;
    }

    size_t len_seq = id_ptr - (star_seq + 1);
    cmd_seq_t id = std::string(star_seq + 1, len_seq);
    const char* cmd = id_ptr;

    InnerTcpClient* fclient = parent_->parent_->findInnerConnectionByLogin(msg);
    if (!fclient) {
      int argc;
      sds* argv = sdssplitargs(cmd, &argc);
      char* command = argv[0];

      std::string resp =
          common::MemSPrintf(SERVER_COMMANDS_OUT_FAIL_2US(CAUSE_NOT_CONNECTED), id, command);
      publish_command_out(resp);
      sdsfreesplitres(argv, argc);
      return;
    }

    ssize_t nwrite = 0;
    common::Error err = fclient->TcpClient::write(buff, len, &nwrite);
    if (err && err->isError()) {
      int argc;
      sds* argv = sdssplitargs(cmd, &argc);
      char* command = argv[0];

      std::string resp =
          common::MemSPrintf(SERVER_COMMANDS_OUT_FAIL_2US(CAUSE_NOT_HANDLED), id, command);
      publish_command_out(resp);
      sdsfreesplitres(argv, argc);
      return;
    }

    auto cb = std::bind(&InnerSubHandler::processSubscribed, this, std::placeholders::_1,
                        std::placeholders::_2, std::placeholders::_3);
    fasto::fastotv::inner::RequestCallback rc(id, cb);
    parent_->subscribeRequest(rc);
  }

  InnerTcpHandlerHost* parent_;
};

InnerTcpHandlerHost::InnerTcpHandlerHost(ServerHost* parent)
    : parent_(parent),
      sub_commands_in_(NULL),
      handler_(NULL),
      ping_client_id_timer_(INVALID_TIMER_ID) {
  handler_ = new InnerSubHandler(this);
  sub_commands_in_ = new RedisSub(handler_);
  redis_subscribe_command_in_thread_ =
      THREAD_MANAGER()->CreateThread(&RedisSub::listen, sub_commands_in_);
}

InnerTcpHandlerHost::~InnerTcpHandlerHost() {
  sub_commands_in_->stop();
  redis_subscribe_command_in_thread_->Join();
  delete sub_commands_in_;
  delete handler_;
}

void InnerTcpHandlerHost::preLooped(tcp::ITcpLoop* server) {
  ping_client_id_timer_ = server->createTimer(ping_timeout_clients, ping_timeout_clients);
}

void InnerTcpHandlerHost::moved(tcp::TcpClient* client) {}

void InnerTcpHandlerHost::postLooped(tcp::ITcpLoop* server) {}

void InnerTcpHandlerHost::timerEmited(tcp::ITcpLoop* server, timer_id_t id) {
  if (ping_client_id_timer_ == id) {
    std::vector<tcp::TcpClient*> online_clients = server->clients();
    for (size_t i = 0; i < online_clients.size(); ++i) {
      tcp::TcpClient* client = online_clients[i];
      const cmd_request_t ping_request = make_request(PING_COMMAND_REQ);
      ssize_t nwrite = 0;
      common::Error err = client->write(ping_request.data(), ping_request.size(), &nwrite);
      INFO_LOG() << "Pinged sended " << nwrite << " byte, client[" << client->formatedName()
                 << "], from server[" << server->formatedName() << "], " << online_clients.size()
                 << " client(s) connected.";
      if (err && err->isError()) {
        DEBUG_MSG_ERROR(err);
        client->close();
        delete client;
      }
    }
  }
}

void InnerTcpHandlerHost::accepted(tcp::TcpClient* client) {
  ssize_t nwrite = 0;
  cmd_request_t whoareyou = make_request(SERVER_WHO_ARE_YOU_COMMAND_REQ);
  client->write(whoareyou.data(), whoareyou.size(), &nwrite);
}

void InnerTcpHandlerHost::closed(tcp::TcpClient* client) {
  bool isOk = parent_->unRegisterInnerConnectionByHost(client);
  if (isOk) {
    InnerTcpClient* iconnection = dynamic_cast<InnerTcpClient*>(client);
    if (iconnection) {
      UserAuthInfo hinf = iconnection->serverHostInfo();
      std::string hoststr = hinf.login;
      std::string connected_resp =
          common::MemSPrintf(SERVER_NOTIFY_CLIENT_DISCONNECTED_1S, hoststr);
      bool res = sub_commands_in_->publish_clients_state(connected_resp);
      if (!res) {
        std::string err_str = common::MemSPrintf(
            "publish_clients_state with args: connected_resp = %s failed!", connected_resp);
        ERROR_LOG() << err_str;
      }
    }
  }
}

void InnerTcpHandlerHost::dataReceived(tcp::TcpClient* client) {
  char buff[MAX_COMMAND_SIZE] = {0};
  ssize_t nread = 0;
  common::Error err = client->read(buff, MAX_COMMAND_SIZE, &nread);
  if ((err && err->isError()) || nread == 0) {
    client->close();
    delete client;
    return;
  }

  InnerTcpClient* iclient = dynamic_cast<InnerTcpClient*>(client);
  CHECK(iclient);

  handleInnerDataReceived(iclient, buff, nread);
}

void InnerTcpHandlerHost::dataReadyToWrite(tcp::TcpClient* client) {}

void InnerTcpHandlerHost::setStorageConfig(const redis_sub_configuration_t& config) {
  sub_commands_in_->setConfig(config);
  redis_subscribe_command_in_thread_->Start();
}

void InnerTcpHandlerHost::handleInnerRequestCommand(fastotv::inner::InnerClient* connection,
                                                    cmd_seq_t id,
                                                    int argc,
                                                    char* argv[]) {
  char* command = argv[0];

  if (IS_EQUAL_COMMAND(command, PING_COMMAND)) {
    cmd_responce_t pong = make_responce(id, PING_COMMAND_RESP_SUCCESS);
    ssize_t nwrite = 0;
    connection->write(pong, &nwrite);
  } else {
    WARNING_LOG() << "UNKNOWN COMMAND: " << command;
  }
}

void InnerTcpHandlerHost::handleInnerResponceCommand(fastotv::inner::InnerClient* connection,
                                                     cmd_seq_t id,
                                                     int argc,
                                                     char* argv[]) {
  ssize_t nwrite = 0;
  char* state_command = argv[0];

  if (IS_EQUAL_COMMAND(state_command, SUCCESS_COMMAND) && argc > 1) {
    char* command = argv[1];
    if (IS_EQUAL_COMMAND(command, PING_COMMAND)) {
      if (argc > 2) {
        const char* pong = argv[2];
        if (!pong) {
          cmd_approve_t resp =
              make_approve_responce(id, PING_COMMAND_APPROVE_FAIL_1S, CAUSE_INVALID_ARGS);
          connection->write(resp, &nwrite);
          goto fail;
        }

        cmd_approve_t resp = make_approve_responce(id, PING_COMMAND_APPROVE_SUCCESS);
        common::Error err = connection->write(resp, &nwrite);
        if (err && err->isError()) {
          goto fail;
        }
      } else {
        cmd_approve_t resp =
            make_approve_responce(id, PING_COMMAND_APPROVE_FAIL_1S, CAUSE_INVALID_ARGS);
        connection->write(resp, &nwrite);
      }
    } else if (IS_EQUAL_COMMAND(command, SERVER_WHO_ARE_YOU_COMMAND)) {
      if (argc > 2) {
        const char* uauthstr = argv[2];
        if (!uauthstr) {
          cmd_approve_t resp = make_approve_responce(id, SERVER_WHO_ARE_YOU_COMMAND_APPROVE_FAIL_1S,
                                                     CAUSE_INVALID_ARGS);
          connection->write(resp, &nwrite);
          goto fail;
        }

        UserAuthInfo uauth = common::ConvertFromString<UserAuthInfo>(uauthstr);
        if (!uauth.isValid()) {
          cmd_approve_t resp = make_approve_responce(id, SERVER_WHO_ARE_YOU_COMMAND_APPROVE_FAIL_1S,
                                                     CAUSE_INVALID_USER);
          connection->write(resp, &nwrite);
          goto fail;
        }

        bool isOk = parent_->findUser(uauth);
        if (!isOk) {
          cmd_approve_t resp = make_approve_responce(id, SERVER_WHO_ARE_YOU_COMMAND_APPROVE_FAIL_1S,
                                                     CAUSE_UNREGISTERED_USER);
          connection->write(resp, &nwrite);
          goto fail;
        }

        std::string login = uauth.login;
        InnerTcpClient* fclient = parent_->findInnerConnectionByLogin(login);
        if (fclient) {
          cmd_approve_t resp = make_approve_responce(id, SERVER_WHO_ARE_YOU_COMMAND_APPROVE_FAIL_1S,
                                                     CAUSE_DOUBLE_CONNECTION_HOST);
          connection->write(resp, &nwrite);
          goto fail;
        }

        cmd_approve_t resp = make_approve_responce(id, SERVER_WHO_ARE_YOU_COMMAND_APPROVE_SUCCESS);
        common::Error err = connection->write(resp, &nwrite);
        if (err && err->isError()) {
          cmd_approve_t resp2 = make_approve_responce(
              id, SERVER_WHO_ARE_YOU_COMMAND_APPROVE_FAIL_1S, err->description());
          connection->write(resp2, &nwrite);
          goto fail;
        }

        isOk = parent_->registerInnerConnectionByUser(uauth, connection);
        if (isOk) {
          std::string connected_resp = common::MemSPrintf(SERVER_NOTIFY_CLIENT_CONNECTED_1S, login);
          bool res = sub_commands_in_->publish_clients_state(connected_resp);
          if (!res) {
            std::string err_str = common::MemSPrintf(
                "publish_clients_state with args: connected_resp = %s failed!", connected_resp);
            ERROR_LOG() << err_str;
          }
        }
      } else {
        cmd_approve_t resp = make_approve_responce(id, SERVER_WHO_ARE_YOU_COMMAND_APPROVE_FAIL_1S,
                                                   CAUSE_INVALID_ARGS);
        connection->write(resp, &nwrite);
      }
    } else if (IS_EQUAL_COMMAND(command, SERVER_PLEASE_SYSTEM_INFO_COMMAND)) {
    } else if (IS_EQUAL_COMMAND(command, SERVER_PLEASE_CONFIG_COMMAND)) {
    } else {
      WARNING_LOG() << "UNKNOWN RESPONCE COMMAND: " << command;
    }
  } else if (IS_EQUAL_COMMAND(state_command, FAIL_COMMAND) && argc > 1) {
  } else {
    WARNING_LOG() << "UNKNOWN STATE COMMAND: " << state_command;
  }

  return;

fail:
  connection->close();
  delete connection;
}

void InnerTcpHandlerHost::handleInnerApproveCommand(fastotv::inner::InnerClient* connection,
                                                    cmd_seq_t id,
                                                    int argc,
                                                    char* argv[]) {
  char* command = argv[0];

  if (IS_EQUAL_COMMAND(command, SUCCESS_COMMAND)) {
    if (argc > 1) {
      const char* okrespcommand = argv[1];
      if (IS_EQUAL_COMMAND(okrespcommand, PING_COMMAND)) {
      } else if (IS_EQUAL_COMMAND(okrespcommand, SERVER_WHO_ARE_YOU_COMMAND)) {
      }
    }
  } else if (IS_EQUAL_COMMAND(command, FAIL_COMMAND)) {
  } else {
    WARNING_LOG() << "UNKNOWN COMMAND: " << command;
  }
}

}  // namespace inner
}  // namespace server
}  // namespace fastotv
}  // namespace fasto
