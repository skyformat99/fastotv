/*  Copyright (C) 2014-2017 FastoGT. All right reserved.

    This file is part of FastoTV.

    FastoTV is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    FastoTV is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with FastoTV. If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <inttypes.h>

#include <common/error.h>
#include <common/sprintf.h>

#include "client_server_types.h"

#define END_OF_COMMAND "\r\n"

#define FAIL_COMMAND "fail"
#define SUCCESS_COMMAND "ok"

#define IS_EQUAL_COMMAND(BUF, CMD) BUF&& strncmp(BUF, CMD, sizeof(CMD) - 1) == 0

#define CID_FMT PRIu8

#define GENERATE_REQUEST_FMT(CMD) "%" CID_FMT " %s " CMD END_OF_COMMAND
#define GENERATE_REQUEST_FMT_ARGS(CMD, CMD_FMT) "%" CID_FMT " %s " CMD " " CMD_FMT END_OF_COMMAND

#define GENEATATE_SUCCESS_FMT(CMD, CMD_FMT) "%" CID_FMT " %s " SUCCESS_COMMAND " " CMD " " CMD_FMT END_OF_COMMAND
#define GENEATATE_FAIL_FMT(CMD, CMD_FMT) "%" CID_FMT " %s " FAIL_COMMAND " " CMD " " CMD_FMT END_OF_COMMAND

#define REQUEST_COMMAND 0
#define RESPONCE_COMMAND 1
#define APPROVE_COMMAND 2

// client commands
#define CLIENT_PING "client_ping"  // ping server
#define CLIENT_GET_SERVER_INFO "get_server_info"
#define CLIENT_GET_CHANNELS "get_channels"
#define CLIENT_GET_RUNTIME_CHANNEL_INFO "get_runtime_channel_info"
#define CLIENT_SEND_CHAT_MESSAGE "client_send_chat_message"

// server commands
#define SERVER_PING "server_ping"  // ping client
#define SERVER_WHO_ARE_YOU "who_are_you"
#define SERVER_GET_CLIENT_INFO "get_client_info"
#define SERVER_SEND_CHAT_MESSAGE "server_send_chat_message"

// request
// [uint8_t](0) [hex_string]seq [std::string]command

// responce
// [uint8_t](1) [hex_string]seq [OK|FAIL] [std::string]command args ...

// approve
// [uint8_t](2) [hex_string]seq [OK|FAIL] [std::string]command args ...

namespace fastotv {

typedef std::string cmd_seq_t;
typedef uint8_t cmd_id_t;

std::string CmdIdToString(cmd_id_t id);

common::Error StableCommand(const std::string& command, std::string* stabled_command);
common::Error ParseCommand(const std::string& command, cmd_id_t* cmd_id, cmd_seq_t* seq_id, std::string* cmd_str);

template <cmd_id_t cmd_id>
class InnerCmd {
 public:
  InnerCmd(cmd_seq_t id, const std::string& cmd) : id_(id), cmd_(cmd) {}

  static cmd_id_t GetType() { return cmd_id; }

  cmd_seq_t GetId() const { return id_; }

  const std::string& GetCmd() const { return cmd_; }

 private:
  const cmd_seq_t id_;
  const std::string cmd_;
};

typedef InnerCmd<REQUEST_COMMAND> cmd_request_t;
typedef InnerCmd<RESPONCE_COMMAND> cmd_responce_t;
typedef InnerCmd<APPROVE_COMMAND> cmd_approve_t;

template <typename... Args>
cmd_request_t MakeRequest(cmd_seq_t id, const char* cmd_fmt, Args... args) {
  std::string buff = common::MemSPrintf(cmd_fmt, REQUEST_COMMAND, id, args...);
  return cmd_request_t(id, buff);
}

template <typename... Args>
cmd_approve_t MakeApproveResponce(cmd_seq_t id, const char* cmd_fmt, Args... args) {
  std::string buff = common::MemSPrintf(cmd_fmt, APPROVE_COMMAND, id, args...);
  return cmd_approve_t(id, buff);
}

template <typename... Args>
cmd_responce_t MakeResponce(cmd_seq_t id, const char* cmd_fmt, Args... args) {
  std::string buff = common::MemSPrintf(cmd_fmt, RESPONCE_COMMAND, id, args...);
  return cmd_responce_t(id, buff);
}

}  // namespace fastotv
