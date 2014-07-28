// Sandstorm - Personal Cloud Sandbox
// Copyright (c) 2014 Sandstorm Development Group, Inc. and contributors
// All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This header contains a proxy smtp server that is used to bridge smtp
// messages to Cap'n Proto objects. Used by sandstorm-http-bridge

#pragma once

#include <kj/debug.h>
#include <kj/async-io.h>
#include <capnp/serialize.h>
#include <gmime/gmime.h>
// requires libgpgme11-dev libgmime-2.6-dev libselinux1-dev
#include <sandstorm/email.capnp.h>

namespace sandstorm {
  namespace smtp {

  static const char * END_LINE = "\r\n";
  static const char * END_BODY = "\r\n.\r\n";

  #define STRING_AND_SIZE(str) str "\r\n", sizeof(str) + 1

  static GMimeMessage *
  parse_message (kj::StringPtr str)
  {
    g_mime_init(0); //only one time
            g_mime_charset_map_init();
            g_mime_iconv_init();
    auto newString = kj::str(str, END_BODY);
    GMimeMessage *message;
    GMimeParser *parser;
    GMimeStream *stream;

    /* create a stream to read from the file descriptor */
    stream = g_mime_stream_mem_new_with_buffer (newString.cStr(), newString.size());

    /* create a new parser object to parse the stream */
    parser = g_mime_parser_new_with_stream (stream);

    /* parse the message from the stream */
    message = g_mime_parser_construct_message (parser);

    /* free the parser */
    // g_object_unref (parser);

    return message;
  }

  inline kj::Maybe<size_t> findFirst(kj::StringPtr str, char c, size_t start = 0) {
    if (start >= str.size()) return nullptr;
    const char* pos = reinterpret_cast<const char*>(memchr(str.begin() + start, c, str.size() - start));
    if (pos == nullptr) {
      return nullptr;
    } else {
      return pos - str.begin();
    }
  };

  inline kj::Maybe<size_t> find(kj::StringPtr haystack, kj::StringPtr needle, size_t start = 0) {
    if (needle.size() == 0 || haystack.size() == 0) return nullptr;
    if (needle.size() > haystack.size()) return nullptr;
    while (true) {
      KJ_IF_MAYBE(pos, findFirst(haystack, needle[0], start)) {
        if (*pos + needle.size() > haystack.size()) return nullptr;
        if (haystack.slice(*pos, *pos + needle.size()) == needle.slice(0, needle.size())) {
          return *pos;
        } else {
          start = *pos + 1;
        }
      } else {
        return nullptr;
      }
    }
  };

  void toLower(kj::ArrayPtr<char> text) {
    for (char& c: text) {
      if ('A' <= c && c <= 'Z') {
        c = c - 'A' + 'a';
      }
    }
  }

  struct AcceptedConnection {
    kj::Own<kj::AsyncIoStream> connection;
    kj::String lastBuffer;
    char buf[1024];

    explicit AcceptedConnection(kj::Own<kj::AsyncIoStream>&& connectionParam)
        : connection(kj::mv(connectionParam)), lastBuffer(kj::str("")) { }

    kj::Promise<kj::String> readUntilHelper(kj::String&& delimiter, kj::Vector<kj::String>&& output) {
      return connection->tryRead(buf, 1, sizeof(buf) - 1).then([&, delimiter=kj::mv(delimiter), output=kj::mv(output)](size_t size) mutable -> kj::Promise<kj::String>  {
        if (size == 0) {
          return kj::strArray(output, "");
        }

        buf[size] = '\0';
        kj::StringPtr temp(buf, size);

        KJ_IF_MAYBE(pos, find(temp, delimiter)) {
          output.add(kj::heapString(temp.slice(0, *pos)));

          if (*pos + delimiter.size() < temp.size()) {
            lastBuffer = kj::heapString(temp.slice(*pos + delimiter.size()));
          }

          return kj::strArray(output, "");
        } else {
          output.add(kj::heapString(temp));
          return readUntilHelper(kj::mv(delimiter), kj::mv(output));
        }
      });
    }

    kj::Promise<kj::String> readUntil(kj::String&& delimiter) {
      kj::Vector<kj::String> output;
      output.add(kj::mv(lastBuffer));
      lastBuffer = kj::str("");
      return readUntilHelper(kj::mv(delimiter), kj::mv(output));
    }

    kj::Promise<void> start() {
      return connection->write(STRING_AND_SIZE("220 Sandstorm SMTP Bridge")).then(
          [this]() {
            return messageLoop();
      });
    }

    kj::Promise<void> forwardMessage(kj::StringPtr data) {
      // capnp::EzRpcClient client("unix:/tmp/sandstorm-api");
      // EmailSendPort::Client session = client.importCap<EmailSendPort>("HackSessionContext");
      // auto req = session.sendRequest();
      auto msg = parse_message(data);
      auto part = g_mime_message_get_mime_part(msg);

      auto subject =  g_mime_object_get_header((GMimeObject*)msg, "Subject"); // memleak

      KJ_SYSCALL(write(STDOUT_FILENO, subject, strlen(subject)));
      KJ_SYSCALL(write(STDOUT_FILENO, STRING_AND_SIZE("")));

      // g_free(msg);
      return kj::READY_NOW;
    }

    kj::Promise<void> handleCommand(kj::String&& line) {
      kj::ArrayPtr<const char> rawCommand;
      KJ_IF_MAYBE(pos, line.findFirst(' ')) {
        rawCommand = line.slice(0, *pos);
      } else {
        rawCommand = line;
      }

      auto command = kj::heapString(rawCommand);
      toLower(command);
      // TODO(soon): make comparisons case insensitive
      if (command == "helo") {
        // TODO(soon): make sure hostname is passed as an argument
        return connection->write(STRING_AND_SIZE("250 Sandstorm at your service"));
      } else if (command == "mail") {
        // TODO(soon): do something here?
        return connection->write(STRING_AND_SIZE("250 OK"));
      } else if (command == "data") {
        return connection->write(STRING_AND_SIZE("354 Start mail input; end with <CRLF>.<CRLF>")).then([&]() {
          return readUntil(kj::str(END_BODY));
        }).then([&](auto data) {
          return forwardMessage(data);
        }).then([&]() {
            connection->write(STRING_AND_SIZE("250 OK"));
        });
      } else if (command == "rcpt") {
        // TODO(soon): do something here?
        return connection->write(STRING_AND_SIZE("250 OK"));
      } else if (command == "noop") {
        return connection->write(STRING_AND_SIZE("250 OK"));
      } else if (command == "rset") {
        // TODO(soon): make sure it's ok to do nothing here
        return connection->write(STRING_AND_SIZE("250 OK"));
      } else if (command == "quit") {
        // TODO(soon): make this less hacky
        return connection->write(STRING_AND_SIZE("221 2.0.0 Goodbye!"));
      } else {
        return connection->write(STRING_AND_SIZE("502 5.5.2 Error: command not recognized"));
      }
    }
    kj::Promise<void> messageLoop() {
      return readUntil(kj::str(END_LINE)).then(
          [this](kj::String&& line) -> kj::Promise<void> {
        if (line.size() == 0) {
          return kj::READY_NOW;
        }
        return handleCommand(kj::mv(line)).then([&]() {
          return messageLoop();
        });
      },
          [this](kj::Exception&& err) {
            KJ_LOG(ERROR, err);
        return kj::READY_NOW;
        // Swallow exception and quit looping server
      });
    }
  };

  kj::Promise<void> runServer(kj::ConnectionReceiver& serverPort,
                               kj::TaskSet& taskSet) {
    return serverPort.accept().then([&](kj::Own<kj::AsyncIoStream>&& connection) {
      auto connectionState = kj::heap<AcceptedConnection>(kj::mv(connection));
      auto promise = connectionState->start();
      taskSet.add(promise.attach(kj::mv(connectionState)));
      return runServer(serverPort, taskSet);
    });
  }

  #undef STRING_AND_SIZE
  }  // namespace smtp
}  // namespace sandstorm
