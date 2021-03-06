/* libinfinity - a GObject-based infinote implementation
 * Copyright (C) 2007-2014 Armin Burgmeier <armin@arbur.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <libinfinity/client/infc-browser.h>
#include <libinfinity/communication/inf-communication-manager.h>
#include <libinfinity/common/inf-session-proxy.h>
#include <libinfinity/common/inf-chat-buffer.h>
#include <libinfinity/common/inf-chat-session.h>
#include <libinfinity/common/inf-xmpp-connection.h>
#include <libinfinity/common/inf-tcp-connection.h>
#include <libinfinity/common/inf-request-result.h>
#include <libinfinity/common/inf-ip-address.h>
#include <libinfinity/common/inf-standalone-io.h>
#include <libinfinity/common/inf-io.h>
#include <libinfinity/common/inf-protocol.h>

#include <string.h>

typedef struct _InfTestChat InfTestChat;
struct _InfTestChat {
  InfStandaloneIo* io;
  InfXmppConnection* conn;
  InfBrowser* browser;
#ifndef G_OS_WIN32
  int input_fd;
#endif

  InfChatBuffer* buffer;
  InfUser* self;
};

static void
inf_test_chat_input_cb(InfNativeSocket* fd,
                       InfIoEvent io,
                       gpointer user_data)
{
  InfTestChat* test;
  char buffer[1024];

  test = (InfTestChat*)user_data;

  if(io & INF_IO_ERROR)
  {
  }

  if(io & INF_IO_INCOMING)
  {
    if(fgets(buffer, sizeof(buffer), stdin) == NULL)
    {
      inf_standalone_io_loop_quit(test->io);
    }
    else if(strlen(buffer) != sizeof(buffer) ||
            buffer[sizeof(buffer)-2] == '\n')
    {
      buffer[strlen(buffer)-1] = '\0';

      if(test->buffer != NULL && test->self != NULL)
      {
        inf_chat_buffer_add_message(
          test->buffer,
          test->self,
          buffer,
          strlen(buffer),
          time(NULL),
          0
        );
      }
    }
  }
}

static void
inf_chat_test_buffer_receive_message_cb(InfChatSession* session,
                                        InfChatBufferMessage* message,
                                        gpointer user_data)
{
  switch(message->type)
  {
  case INF_CHAT_BUFFER_MESSAGE_NORMAL:
    printf("<%s> %s\n", inf_user_get_name(message->user), message->text);
    break;
  case INF_CHAT_BUFFER_MESSAGE_EMOTE:
    printf(" * %s %s\n", inf_user_get_name(message->user), message->text);
    break;
  case INF_CHAT_BUFFER_MESSAGE_USERJOIN:
    printf(" --> %s has joined\n", inf_user_get_name(message->user));
    break;
  case INF_CHAT_BUFFER_MESSAGE_USERPART:
    printf(" <-- %s has left\n", inf_user_get_name(message->user));
    break;
  }
}

static void
inf_test_chat_userjoin_finished_cb(InfRequest* request,
                                   const InfRequestResult* result,
                                   const GError* error,
                                   gpointer user_data)
{
  InfTestChat* test;
  InfUser* user;

  test = (InfTestChat*)user_data;

  if(error == NULL)
  {
    printf("User join complete. Start chatting!\n");

#ifndef G_OS_WIN32
    inf_io_add_watch(
      INF_IO(test->io),
      &test->input_fd,
      INF_IO_INCOMING | INF_IO_ERROR,
      inf_test_chat_input_cb,
      test,
      NULL
    );
#endif

    inf_request_result_get_join_user(result, NULL, &user);
    test->self = user;
  }
  else
  {
    fprintf(stderr, "User join failed: %s\n", error->message);
    fprintf(stderr, "Chat will be read-only\n");
  }
}

static void
inf_chat_test_session_synchronization_complete_cb(InfSession* session,
                                                  InfXmlConnection* connection,
                                                  gpointer user_data)
{
  InfTestChat* test;
  InfcSessionProxy* proxy;
  InfRequest* request;
  GParameter params[1] = { { "name", { 0 } } };

  printf("Synchronization complete, joining user...\n");

  test = (InfTestChat*)user_data;
  proxy = infc_browser_get_chat_session(INFC_BROWSER(test->browser));

  g_value_init(&params[0].value, G_TYPE_STRING);
  g_value_set_string(&params[0].value, g_get_user_name());

  inf_session_proxy_join_user(
    INF_SESSION_PROXY(proxy),
    G_N_ELEMENTS(params),
    params,
    inf_test_chat_userjoin_finished_cb,
    test
  );

  g_value_unset(&params[0].value);
}

static void
inf_chat_test_session_synchronization_failed_cb(InfSession* session,
                                                InfXmlConnection* connection,
                                                const GError* error,
                                                gpointer user_data)
{
  InfTestChat* test;
  test = (InfTestChat*)user_data;

  fprintf(stderr, "Synchronization failed: %s\n", error->message);
  inf_standalone_io_loop_quit(test->io);
}

static void
inf_chat_test_session_close_cb(InfSession* session,
                               gpointer user_data)
{
  InfTestChat* test;
  test = (InfTestChat*)user_data;

  printf("The server closed the chat session\n");
  if(inf_standalone_io_loop_running(test->io))
    inf_standalone_io_loop_quit(test->io);
}

static void
inf_chat_test_subscribe_finished_cb(InfRequest* request,
                                    const InfRequestResult* result,
                                    const GError* error,
                                    gpointer user_data)
{
  InfTestChat* test;
  InfcSessionProxy* proxy;
  InfSession* session;
  test = (InfTestChat*)user_data;

  if(error == NULL)
  {
    printf("Subscription successful, waiting for synchronization...\n");

    proxy = infc_browser_get_chat_session(INFC_BROWSER(test->browser));
    g_object_get(G_OBJECT(proxy), "session", &session, NULL);

    test->buffer = INF_CHAT_BUFFER(inf_session_get_buffer(session));

    /* TODO: Show backlog after during/after synchronization */

    g_signal_connect_after(
      G_OBJECT(session),
      "receive-message",
      G_CALLBACK(inf_chat_test_buffer_receive_message_cb),
      test
    );

    g_signal_connect_after(
      G_OBJECT(session),
      "synchronization-complete",
      G_CALLBACK(inf_chat_test_session_synchronization_complete_cb),
      test
    );

    g_signal_connect_after(
      G_OBJECT(session),
      "synchronization-failed",
      G_CALLBACK(inf_chat_test_session_synchronization_failed_cb),
      test
    );

    /* This can happen when the server disables the chat without being
     * shutdown. */
    g_signal_connect_after(
      G_OBJECT(session),
      "close",
      G_CALLBACK(inf_chat_test_session_close_cb),
      test
    );

    g_object_unref(session);
  }
  else
  {
    fprintf(stderr, "Subscription failed: %s\n", error->message);
    inf_standalone_io_loop_quit(test->io);
  }
}

static void
inf_test_chat_notify_status_cb(GObject* object,
                               GParamSpec* pspec,
                               gpointer user_data)
{
  InfTestChat* test;
  InfBrowserStatus status;

  test = (InfTestChat*)user_data;
  g_object_get(G_OBJECT(object), "status", &status, NULL);

  if(status == INF_BROWSER_OPEN)
  {
    printf("Connection established, subscribing to chat...\n");

    /* Subscribe to chat */
    infc_browser_subscribe_chat(
      INFC_BROWSER(test->browser),
      inf_chat_test_subscribe_finished_cb,
      test
    );
  }

  if(status == INF_BROWSER_CLOSED)
  {
    printf("Connection closed\n");
    if(inf_standalone_io_loop_running(test->io))
      inf_standalone_io_loop_quit(test->io);
  }
}

static void
inf_test_chat_error_cb(InfXmppConnection* xmpp,
                          GError* error,
                          gpointer user_data)
{
  /* status notify will close conn: */
  fprintf(stderr, "Connection error: %s\n", error->message);
}

int
main(int argc, char* argv[])
{
  InfTestChat test;
  InfIpAddress* address;
  InfCommunicationManager* manager;
  InfTcpConnection* tcp_conn;
  GError* error;

  gnutls_global_init();
  g_type_init();

  test.io = inf_standalone_io_new();
#ifndef G_OS_WIN32
  test.input_fd = STDIN_FILENO;
#endif
  test.buffer = NULL;

  address = inf_ip_address_new_loopback4();

  error = NULL;
  tcp_conn =
    inf_tcp_connection_new_and_open(INF_IO(test.io), address, inf_protocol_get_default_port(), &error);

  inf_ip_address_free(address);

  if(tcp_conn == NULL)
  {
    fprintf(stderr, "Could not open TCP connection: %s\n", error->message);
    g_error_free(error);
  }
  else
  {
    test.conn = inf_xmpp_connection_new(
      tcp_conn,
      INF_XMPP_CONNECTION_CLIENT,
      NULL,
      "localhost",
      INF_XMPP_CONNECTION_SECURITY_BOTH_PREFER_TLS,
      NULL,
      NULL,
      NULL
    );

    g_object_unref(G_OBJECT(tcp_conn));

    manager = inf_communication_manager_new();
    test.browser = INF_BROWSER(
      infc_browser_new(
        INF_IO(test.io),
        manager,
        INF_XML_CONNECTION(test.conn)
      )
    );

    g_signal_connect_after(
      G_OBJECT(test.browser),
      "notify::status",
      G_CALLBACK(inf_test_chat_notify_status_cb),
      &test
    );

    g_signal_connect(
      G_OBJECT(test.browser),
      "error",
      G_CALLBACK(inf_test_chat_error_cb),
      &test
    );

    inf_standalone_io_loop(test.io);
    g_object_unref(G_OBJECT(manager));
    g_object_unref(G_OBJECT(test.browser));

    /* TODO: Wait until the XMPP connection is in status closed */
    g_object_unref(G_OBJECT(test.conn));
  }

  g_object_unref(G_OBJECT(test.io));
  return 0;
}

/* vim:set et sw=2 ts=2: */
