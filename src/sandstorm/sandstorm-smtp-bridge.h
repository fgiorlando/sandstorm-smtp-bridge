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
    GMimeMessage *message;
    GMimeParser *parser;
    GMimeStream *stream;

    /* create a stream to read from the file descriptor */
    stream = g_mime_stream_mem_new_with_buffer (str.cStr(), str.size());

    /* create a new parser object to parse the stream */
    parser = g_mime_parser_new_with_stream (stream);

    /* unref the stream (parser owns a ref, so this object does not actually get free'd until we destroy the parser) */
    g_object_unref (stream);

    /* parse the message from the stream */
    message = g_mime_parser_construct_message (parser);

    /* free the parser */
    g_object_unref (parser);

    return message;
  }

  kj::Vector<kj::ArrayPtr<const char>> split(kj::ArrayPtr<const char> input, char delim) {
    kj::Vector<kj::ArrayPtr<const char>> result;

    size_t start = 0;
    for (size_t i: kj::indices(input)) {
      if (input[i] == delim) {
        result.add(input.slice(start, i));
        start = i + 1;
      }
    }
    result.add(input.slice(start, input.size()));
    return result;
  }

  kj::Maybe<kj::ArrayPtr<const char>> splitFirst(kj::ArrayPtr<const char>& input, char delim) {
    for (size_t i: kj::indices(input)) {
      if (input[i] == delim) {
        auto result = input.slice(0, i);
        input = input.slice(i + 1, input.size());
        return result;
      }
    }
    return nullptr;
  }

  kj::ArrayPtr<const char> trim(kj::ArrayPtr<const char> input) {
    while (input.size() > 0 && input[0] == ' ') {
      input = input.slice(1, input.size());
    }
    while (input.size() > 0 && input[input.size() - 1] == ' ') {
      input = input.slice(0, input.size() - 1);
    }
    return input;
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
    EmailSendPort::Client& emailCap;
    kj::String lastBuffer;
    char buf[1024];

    explicit AcceptedConnection(kj::Own<kj::AsyncIoStream>&& connectionParam, EmailSendPort::Client& emailCap)
        : connection(kj::mv(connectionParam)), emailCap(emailCap), lastBuffer(kj::str("")) { }

    kj::Promise<kj::String> readUntilHelper(kj::String&& delimiter, kj::Vector<kj::String>&& output) {
      return connection->tryRead(buf, 1, sizeof(buf) - 1).then([&, delimiter=kj::mv(delimiter), output=kj::mv(output)](size_t size) mutable -> kj::Promise<kj::String>  {
        if (size == 0) {
          return kj::strArray(output, "");
        }

        buf[size] = '\0';
        kj::StringPtr line(buf, size);
        // TODO(someday): don't search all lines, just look at the minimum amount needed
        kj::String all = kj::str(kj::strArray(output, ""), line);

        KJ_IF_MAYBE(pos, find(all, delimiter)) {
          lastBuffer = kj::heapString(all.slice(*pos + delimiter.size()));

          return kj::heapString(all.slice(0, *pos + delimiter.size()));
        } else {
          output.add(kj::heapString(line));
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

    #define SET_HEADER(name, gmimeName) \
      header = g_mime_object_get_header((GMimeObject*)msg, #gmimeName); \
      if (header) { \
        decoded = g_mime_utils_header_decode_text(header); \
        HEADER_OBJECT.set##name(decoded); \
        g_free(decoded); \
      }

    #define SET_HEADER_LIST(name, gmimeName) \
      header = g_mime_object_get_header((GMimeObject*)msg, #gmimeName); \
      if (header) { \
        decoded = g_mime_utils_header_decode_text(header); \
        HEADER_OBJECT.init##name(1); \
        HEADER_OBJECT.get##name().set(0, decoded); \
        g_free(decoded); \
      }


    void setEmailHeader(sandstorm::EmailAddress::Builder&& address, char * val) {
      kj::String orig = kj::str(val);
      KJ_IF_MAYBE(pos, orig.findFirst('<')) {
        address.setName(kj::str(orig.slice(0, *pos)));
        address.setAddress(kj::str(orig.slice(*pos + 1, orig.size() - 1)));
      } else {
        address.setAddress(val);
      }
    }

    #define SET_EMAIL_HEADER(name, gmimeName) \
      header = g_mime_object_get_header((GMimeObject*)msg, #gmimeName); \
      if (header) { \
        decoded = g_mime_utils_header_decode_text(header); \
        setEmailHeader(HEADER_OBJECT.get##name(), decoded); \
        g_free(decoded); \
      }

    #define SET_EMAIL_HEADER_LIST(name, gmimeName) \
      header = g_mime_object_get_header((GMimeObject*)msg, #gmimeName); \
      if (header) { \
        decoded = g_mime_utils_header_decode_text(header); \
        HEADER_OBJECT.init##name(1); \
        setEmailHeader(HEADER_OBJECT.get##name()[0], decoded); \
        g_free(decoded); \
      }

    kj::String partToString(GMimeObject * part) {
      auto content = g_mime_part_get_content_object((GMimePart *)part);
      KJ_ASSERT(content != NULL, "Content of message unexpectedly null");
      auto stream = g_mime_data_wrapper_get_stream(content);
      auto length = g_mime_stream_length(stream);
      KJ_ASSERT(length >= 0, "Content's stream has unknown length", length);
      kj::String ret = kj::heapString(length);
      g_mime_stream_read(stream, ret.begin(), length);

      return ret;
    }

    kj::String partToString(GMimeObject * part, GMimeContentEncoding encoding) {
      if (encoding == GMIME_CONTENT_ENCODING_DEFAULT) {
        return partToString(part);
      }

      auto content = g_mime_part_get_content_object((GMimePart *)part);
      KJ_ASSERT(content != NULL, "Content of message unexpectedly null");
      auto stream = g_mime_data_wrapper_get_stream(content);
      auto filteredStream = g_mime_stream_filter_new(stream);
      auto filter = g_mime_filter_basic_new(encoding, FALSE);
      g_mime_stream_filter_add((GMimeStreamFilter *)filteredStream, filter);
      auto length = g_mime_stream_length(filteredStream);
      KJ_ASSERT(length >= 0, "Content's stream has unknown length", length);
      kj::String ret = kj::heapString(length);
      auto numBytes = g_mime_stream_read(filteredStream, ret.begin(), length);

      g_object_unref (filteredStream);
      g_object_unref (filter);
      return kj::heapString(ret.slice(0, numBytes));
    }

    void addAttachment(EmailMessage::Builder& email, GMimeObject * part, int attachmentCount) {
      auto msg = part;
      const char * header;
      char * decoded;

      auto attachment = email.getAttachments()[attachmentCount];
      header = g_mime_object_get_header((GMimeObject*)msg, "Content-Transfer-Encoding");
      GMimeContentEncoding encoding = GMIME_CONTENT_ENCODING_DEFAULT;
      if (header != NULL) {
        encoding = g_mime_content_encoding_from_string(header);
      }
      auto str = partToString(part, encoding);
      kj::ArrayPtr<const capnp::byte> data(reinterpret_cast<const capnp::byte *>(str.begin()), str.size());

      attachment.setContent(data);
      #define HEADER_OBJECT attachment
      SET_HEADER(ContentType, Content-Type)
      SET_HEADER(ContentDisposition, Content-Disposition)
      SET_HEADER(ContentId, Content-Id)
      #undef HEADER_OBJECT
    }

    void setBody(EmailMessage::Builder& email, GMimeObject * part, bool isTopLevel, int attachmentCount=0) {
      auto type = g_mime_object_get_content_type(part);
      if (!type) {
        if (isTopLevel) {
          email.setText(partToString(part));
        } else {
          KJ_FAIL_ASSERT("Unhandled mime part with unkown type");
        }
      } else {
        if (g_mime_object_get_disposition(part) != NULL) {
            addAttachment(email, part, attachmentCount);
        } else if (g_mime_content_type_is_type(type, "text", "*")) {
          if (g_mime_content_type_is_type(type, "text", "html")) {
            email.setHtml(partToString(part));
          } else {
            email.setText(partToString(part));
          }
        } else if (isTopLevel && GMIME_IS_MULTIPART(part)) {
          GMimeMultipart * multipart = (GMimeMultipart *)part;
          int numParts = g_mime_multipart_get_count(multipart);
          int numAttachments = 0;
          for(int i = 0; i < numParts; ++i) {
            auto subPart = g_mime_multipart_get_part(multipart, i);
            KJ_ASSERT(subPart != NULL, "Unexpected bad part in multipart message");
            if (g_mime_object_get_disposition(subPart) != NULL) {
              ++numAttachments;
            }
          }
          if (numAttachments > 0) {
            email.initAttachments(numAttachments);
          }
          int attachmentCount = 0;
          for(int i = 0; i < numParts; ++i) {
            auto subPart = g_mime_multipart_get_part(multipart, i);
            setBody(email, subPart, false, attachmentCount);
            if (g_mime_object_get_disposition(subPart) != NULL) {
              ++attachmentCount;
            }
          }
        } else {
          KJ_FAIL_ASSERT("Unhandled mime part", g_mime_content_type_to_string(type));
        }
      }
    }
    kj::Promise<void> forwardMessage(kj::StringPtr data) {
      auto req = emailCap.sendRequest();
      auto msg = parse_message(data);
      auto email = req.getEmail();
      auto part = g_mime_message_get_mime_part(msg);
      const char * header;
      char * decoded;

      KJ_REQUIRE(GMIME_IS_OBJECT(msg), "Message was unable to parsed as a valid MIME object");
      #define HEADER_OBJECT email
      SET_EMAIL_HEADER_LIST(To, To);
      SET_EMAIL_HEADER(From, From);
      SET_EMAIL_HEADER(ReplyTo, Reply-To);
      SET_EMAIL_HEADER_LIST(Cc, Cc);
      SET_EMAIL_HEADER_LIST(Bcc, Bcc);
      SET_HEADER(Subject, Subject);

      SET_HEADER(MessageId, Message-Id);
      SET_HEADER_LIST(References, References);
      SET_HEADER_LIST(InReplyTo, In-Reply-To);
      #undef HEADER_OBJECT


      if (!part) {
        // Fail?
        KJ_SYSCALL(write(STDERR_FILENO, STRING_AND_SIZE("sandstorm-smtp-bridge: Mime part of email wan't found?")));
        auto objStr = g_mime_object_to_string((GMimeObject*)msg);
        email.setText(objStr);
        g_free(objStr);
      } else {
        setBody(email, part, true);
      }

      g_object_unref(msg);

      return req.send().then([](auto results) {});
    }

    kj::Promise<bool> handleCommand(kj::String&& line) {
      kj::ArrayPtr<const char> rawCommand = line.slice(0, line.size() - 2); // Chop off line ending
      KJ_IF_MAYBE(pos, line.findFirst(' ')) {
        rawCommand = rawCommand.slice(0, *pos);
      }

      auto command = kj::heapString(rawCommand);
      toLower(command);
      if (command == "helo") {
        // TODO(someday): make sure hostname is passed as an argument
        return connection->write(STRING_AND_SIZE("250 Sandstorm at your service")).then([]() {
          return true;
        });
      } else if (command == "mail") {
        // TODO(someday): do something here?
        return connection->write(STRING_AND_SIZE("250 OK")).then([]() {
          return true;
        });
      } else if (command == "data") {
        return connection->write(STRING_AND_SIZE("354 Start mail input; end with <CRLF>.<CRLF>")).then([&]() {
          return readUntil(kj::str(END_BODY));
        }).then([&](auto data) {
          return forwardMessage(data);
        }).then([&]() {
            connection->write(STRING_AND_SIZE("250 OK"));
        }).then([]() {
          return true;
        });
      } else if (command == "rcpt") {
        // TODO(someday): do something here?
        return connection->write(STRING_AND_SIZE("250 OK")).then([]() {
          return true;
        });
      } else if (command == "noop") {
        return connection->write(STRING_AND_SIZE("250 OK")).then([]() {
          return true;
        });
      } else if (command == "rset") {
        return connection->write(STRING_AND_SIZE("250 OK")).then([]() {
          return true;
        });
      } else if (command == "quit") {
        return connection->write(STRING_AND_SIZE("221 2.0.0 Goodbye!")).then([]() {
          return false;
        });
      } else {
        return connection->write(STRING_AND_SIZE("502 5.5.2 Error: command not recognized")).then([]() {
          return true;
        });
      }
    }

    kj::Promise<void> messageLoop() {
      return readUntil(kj::str(END_LINE)).then(
          [this](kj::String&& line) -> kj::Promise<void> {
        if (line.size() == 0) {
          return kj::READY_NOW;
        }
        return handleCommand(kj::mv(line)).then([&](bool continueLoop) -> kj::Promise<void> {
          if (continueLoop) {
            return messageLoop();
          } else {
            return kj::READY_NOW;
          }
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
                               kj::TaskSet& taskSet, EmailSendPort::Client& emailCap) {
    return serverPort.accept().then([&](kj::Own<kj::AsyncIoStream>&& connection) {
      auto connectionState = kj::heap<AcceptedConnection>(kj::mv(connection), emailCap);
      auto promise = connectionState->start();
      taskSet.add(promise.attach(kj::mv(connectionState)));
      return runServer(serverPort, taskSet, emailCap);
    });
  }

  #undef STRING_AND_SIZE
  }  // namespace smtp
}  // namespace sandstorm
