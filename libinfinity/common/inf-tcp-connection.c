/* infinote - Collaborative notetaking application
 * Copyright (C) 2007 Armin Burgmeier
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
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <libinfinity/common/inf-tcp-connection.h>
#include <libinfinity/common/inf-ip-address.h>
#include <libinfinity/common/inf-io.h>
#include <libinfinity/inf-marshal.h>

#include <libinfinity/inf-config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <errno.h>
#include <string.h>

/* TODO: Win32 support */

typedef struct _InfTcpConnectionPrivate InfTcpConnectionPrivate;
struct _InfTcpConnectionPrivate {
  InfIo* io;
  InfIoEvent events;

  InfTcpConnectionStatus status;
  InfNativeSocket socket;

  InfIpAddress* remote_address;
  guint remote_port;
  unsigned int device_index;

  guint8* queue;
  gsize front_pos;
  gsize back_pos;
  gsize alloc;
};

enum {
  PROP_0,

  PROP_IO,

  PROP_STATUS,

  PROP_REMOTE_ADDRESS,
  PROP_REMOTE_PORT,
  PROP_LOCAL_ADDRESS,
  PROP_LOCAL_PORT,

  PROP_DEVICE_INDEX,
  PROP_DEVICE_NAME
};

enum {
  SENT,
  RECEIVED,
  ERROR,

  LAST_SIGNAL
};

#define INF_TCP_CONNECTION_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), INF_TYPE_TCP_CONNECTION, InfTcpConnectionPrivate))

static GObjectClass* parent_class;
static guint tcp_connection_signals[LAST_SIGNAL];
static GQuark inf_tcp_connection_error_quark;

static void
inf_tcp_connection_addr_info(InfNativeSocket socket,
                             gboolean local,
                             InfIpAddress** address,
                             guint* port)
{
  union {
    struct sockaddr in_generic;
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
  } native_addr;
  socklen_t len;

  len = sizeof(native_addr);

  if(local == TRUE)
    getsockname(socket, &native_addr.in_generic, &len);
  else
    getpeername(socket, &native_addr.in_generic, &len);

  switch(native_addr.in_generic.sa_family)
  {
  case AF_INET:
    if(address != NULL)
      *address = inf_ip_address_new_raw4(native_addr.in.sin_addr.s_addr);
    if(port != NULL)
      *port = ntohs(native_addr.in.sin_port);
    break;
  case AF_INET6:
    if(address != NULL)
      *address = inf_ip_address_new_raw6(native_addr.in6.sin6_addr.s6_addr);
    if(port != NULL)
      *port = ntohs(native_addr.in6.sin6_port);
    break;
  default:
    g_assert_not_reached();
    break;
  }
}

static void
inf_tcp_connection_make_system_error(int code,
                                     GError** error)
{
  /* TODO: This must be WSAGetLastError on Win32 */

  g_set_error(
    error,
    inf_tcp_connection_error_quark,
    code,
    "%s",
    strerror(code)
  );
}

static void
inf_tcp_connection_system_error(InfTcpConnection* connection,
                                int code)
{
  GError* error;
  error = NULL;

  inf_tcp_connection_make_system_error(code, &error);

  g_signal_emit(
    G_OBJECT(connection),
    tcp_connection_signals[ERROR],
    0,
    error
  );

  g_error_free(error);
}

/* Required by inf_tcp_connection_connected */
static void
inf_tcp_connection_io(InfNativeSocket* socket,
                      InfIoEvent events,
                      gpointer user_data);

static void
inf_tcp_connection_connected(InfTcpConnection* connection)
{
  InfTcpConnectionPrivate* priv;
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  priv->status = INF_TCP_CONNECTION_CONNECTED;
  priv->front_pos = 0;
  priv->back_pos = 0;

  priv->events = INF_IO_INCOMING | INF_IO_ERROR;

  inf_io_watch(
    priv->io,
    &priv->socket,
    priv->events,
    inf_tcp_connection_io,
    connection
  );

  g_object_freeze_notify(G_OBJECT(connection));
  g_object_notify(G_OBJECT(connection), "status");
  g_object_notify(G_OBJECT(connection), "local-address");
  g_object_notify(G_OBJECT(connection), "local-port");
  g_object_thaw_notify(G_OBJECT(connection));
}

static void
inf_tcp_connection_io_incoming(InfTcpConnection* connection)
{
  InfTcpConnectionPrivate* priv;
  gchar buf[2048];
  int errcode;
  ssize_t result;
  
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  g_assert(priv->status == INF_TCP_CONNECTION_CONNECTED);

  do
  {
    result = recv(
      priv->socket,
      buf,
      2048,
      MSG_NOSIGNAL
    );

    errcode = errno;

    if(result < 0 && errcode != EINTR && errcode != EAGAIN)
    {
      inf_tcp_connection_system_error(connection, errcode);
    }
    else if(result == 0)
    {
      inf_tcp_connection_close(connection);
    }
    else if(result > 0)
    {
      g_signal_emit(
        G_OBJECT(connection),
        tcp_connection_signals[RECEIVED],
        0,
        buf,
        (guint)result
      );
    }
  } while( ((result > 0) || (result < 0 && errcode == EINTR)) &&
           (priv->socket != -1));
}

static void
inf_tcp_connection_io_outgoing(InfTcpConnection* connection)
{
  InfTcpConnectionPrivate* priv;
  socklen_t len;
  int errcode;
  ssize_t result;

  priv = INF_TCP_CONNECTION_PRIVATE(connection);
  switch(priv->status)
  {
  case INF_TCP_CONNECTION_CONNECTING:
    len = sizeof(int);
    getsockopt(priv->socket, SOL_SOCKET, SO_ERROR, &errcode, &len);

    if(errcode == 0)
    {
      inf_tcp_connection_connected(connection);
    }
    else
    {
      inf_tcp_connection_system_error(connection, errcode);
    }

    break;
  case INF_TCP_CONNECTION_CONNECTED:
    g_assert(priv->back_pos < priv->front_pos);

    do
    {
      result = send(
        priv->socket,
        priv->queue + priv->back_pos,
        priv->front_pos - priv->back_pos,
        MSG_NOSIGNAL
      );

      /* Preserve errno so that it is not modified by future calls */
      errcode = errno;

      if(result < 0 && errcode != EINTR && errcode != EAGAIN)
      {
        inf_tcp_connection_system_error(connection, errcode);
      }
      else if(result == 0)
      {
        inf_tcp_connection_close(connection);
      }
      else if(result > 0)
      {
        g_signal_emit(
          G_OBJECT(connection),
          tcp_connection_signals[SENT],
          0,
          priv->queue + priv->back_pos,
          (guint)result
        );

        priv->back_pos += result;
        if(priv->back_pos == priv->front_pos)
        {
          /* Sent everything */
          priv->front_pos = 0;
          priv->back_pos = 0;

          priv->events &= ~INF_IO_OUTGOING;

          inf_io_watch(
            priv->io,
            &priv->socket,
            priv->events,
            inf_tcp_connection_io,
            connection
          );
        }
      }
    } while( (priv->events & INF_IO_OUTGOING) &&
             (result > 0 || (result < 0 && errcode == EINTR)) &&
             (priv->socket != -1));

    break;
  case INF_TCP_CONNECTION_CLOSED:
  default:
    g_assert_not_reached();
    break;
  }
}

static void
inf_tcp_connection_io(InfNativeSocket* socket,
                      InfIoEvent events,
                      gpointer user_data)
{
  InfTcpConnection* connection;
  InfTcpConnectionPrivate* priv;
  socklen_t len;
  int errcode;

  connection = INF_TCP_CONNECTION(user_data);
  priv = INF_TCP_CONNECTION_PRIVATE(connection);
  g_object_ref(G_OBJECT(connection));

  if(events & INF_IO_ERROR)
  {
    len = sizeof(int);
    getsockopt(priv->socket, SOL_SOCKET, SO_ERROR, &errcode, &len);
    inf_tcp_connection_system_error(connection, errcode);
  }
  else
  {
    if(events & INF_IO_INCOMING)
    {
      inf_tcp_connection_io_incoming(connection);
    }

    /* It may happen that the above closes the connection and we received
     * events for both INCOMING & OUTGOING here. */
    if((priv->status != INF_TCP_CONNECTION_CLOSED) &&
       (events & INF_IO_OUTGOING))
    {
      inf_tcp_connection_io_outgoing(connection);
    }
  }

  g_object_unref(G_OBJECT(connection));
}

static void
inf_tcp_connection_init(GTypeInstance* instance,
                        gpointer g_class)
{
  InfTcpConnection* connection;
  InfTcpConnectionPrivate* priv;

  connection = INF_TCP_CONNECTION(instance);
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  priv->io = NULL;
  priv->events = 0;
  priv->status = INF_TCP_CONNECTION_CLOSED;
  priv->socket = -1;

  priv->remote_address = NULL;
  priv->remote_port = 0;
  priv->device_index = 0;

  priv->queue = g_malloc(1024);
  priv->front_pos = 0;
  priv->back_pos = 0;
  priv->alloc = 1024;
}

static void
inf_tcp_connection_dispose(GObject* object)
{
  InfTcpConnection* connection;
  InfTcpConnectionPrivate* priv;

  connection = INF_TCP_CONNECTION(object);
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  if(priv->status != INF_TCP_CONNECTION_CLOSED)
    inf_tcp_connection_close(connection);

  if(priv->io != NULL)
  {
    g_object_unref(G_OBJECT(priv->io));
    priv->io = NULL;
  }

  G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void
inf_tcp_connection_finalize(GObject* object)
{
  InfTcpConnection* connection;
  InfTcpConnectionPrivate* priv;

  connection = INF_TCP_CONNECTION(object);
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  if(priv->remote_address != NULL)
    inf_ip_address_free(priv->remote_address);

  g_free(priv->queue);

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void
inf_tcp_connection_set_property(GObject* object,
                                guint prop_id,
                                const GValue* value,
                                GParamSpec* pspec)
{
  InfTcpConnection* connection;
  InfTcpConnectionPrivate* priv;
  const gchar* device_string;
  unsigned int new_index;

  connection = INF_TCP_CONNECTION(object);
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  switch(prop_id)
  {
  case PROP_IO:
    g_assert(priv->status == INF_TCP_CONNECTION_CLOSED);
    if(priv->io != NULL) g_object_unref(G_OBJECT(priv->io));
    priv->io = INF_IO(g_value_dup_object(value));
    break;
  case PROP_REMOTE_ADDRESS:
    g_assert(priv->status == INF_TCP_CONNECTION_CLOSED);
    if(priv->remote_address != NULL)
      inf_ip_address_free(priv->remote_address);
    priv->remote_address = (InfIpAddress*)g_value_dup_boxed(value);
    break;
  case PROP_REMOTE_PORT:
    g_assert(priv->status == INF_TCP_CONNECTION_CLOSED);
    priv->remote_port = g_value_get_uint(value);
    break;
  case PROP_DEVICE_INDEX:
    g_assert(priv->status == INF_TCP_CONNECTION_CLOSED);
    /* TODO: Verify that such a device exists */
    priv->device_index = g_value_get_uint(value);
    g_object_notify(G_OBJECT(object), "device-name");
    break;
  case PROP_DEVICE_NAME:
    g_assert(priv->status == INF_TCP_CONNECTION_CLOSED);
    device_string = g_value_get_string(value);
    if(device_string == NULL) priv->device_index = 0;

    new_index = if_nametoindex(device_string);
    if(new_index == 0)
    {
      g_warning("Interface `%s' does not exist", device_string);
    }
    else
    {
      priv->device_index = new_index;
      g_object_notify(G_OBJECT(object), "device-index");
    }

    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
inf_tcp_connection_get_property(GObject* object,
                                guint prop_id,
                                GValue* value,
                                GParamSpec* pspec)
{
  InfTcpConnection* connection;
  InfTcpConnectionPrivate* priv;
  InfIpAddress* address;
  guint port;
  char device_name[IF_NAMESIZE];

  connection = INF_TCP_CONNECTION(object);
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  switch(prop_id)
  {
  case PROP_IO:
    g_value_set_object(value, G_OBJECT(priv->io));
    break;
  case PROP_STATUS:
    g_value_set_enum(value, priv->status);
    break;
  case PROP_REMOTE_ADDRESS:
    g_value_set_static_boxed(value, priv->remote_address);
    break;
  case PROP_REMOTE_PORT:
    g_value_set_uint(value, priv->remote_port);
    break;
  case PROP_LOCAL_ADDRESS:
    g_assert(priv->status == INF_TCP_CONNECTION_CONNECTED);
    inf_tcp_connection_addr_info(priv->socket, TRUE, &address, NULL);
    g_value_take_boxed(value, address);
    break;
  case PROP_LOCAL_PORT:
    g_assert(priv->status == INF_TCP_CONNECTION_CONNECTED);
    inf_tcp_connection_addr_info(priv->socket, TRUE, NULL, &port);
    g_value_set_uint(value, port);
    break;
  case PROP_DEVICE_INDEX:
    g_value_set_uint(value, priv->device_index);
    break;
  case PROP_DEVICE_NAME:
    if(priv->device_index == 0)
    {
      g_value_set_string(value, NULL);
    }
    else
    {
      if(if_indextoname(priv->device_index, device_name) == NULL)
      {
        g_warning(
          "Failed to get name for device %u: %s",
          priv->device_index,
          strerror(errno)
        );
        
        g_value_set_string(value, NULL);
      }
      else
      {
        g_value_set_string(value, device_name);
      }
    }
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
inf_tcp_connection_error(InfTcpConnection* connection,
                         GError* error)
{
  InfTcpConnectionPrivate* priv;
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  /* Normally, it would be enough to check one both conditions, but socket
   * may be already set with status still being CLOSED during
   * inf_tcp_connection_open(). */
  if(priv->events != 0)
  {
    priv->events = 0;

    inf_io_watch(
      priv->io,
      &priv->socket,
      priv->events,
      inf_tcp_connection_io,
      connection
    );
  }

  if(priv->socket != -1)
  {
    close(priv->socket);
    priv->socket = -1;
  }

  if(priv->status != INF_TCP_CONNECTION_CLOSED)
  {
    priv->status = INF_TCP_CONNECTION_CLOSED;
    g_object_notify(G_OBJECT(connection), "status");
  }
}

static void
inf_tcp_connection_class_init(gpointer g_class,
                              gpointer class_data)
{
  GObjectClass* object_class;
  InfTcpConnectionClass* tcp_connection_class;

  object_class = G_OBJECT_CLASS(g_class);
  tcp_connection_class = INF_TCP_CONNECTION_CLASS(g_class);

  parent_class = G_OBJECT_CLASS(g_type_class_peek_parent(g_class));
  g_type_class_add_private(g_class, sizeof(InfTcpConnectionPrivate));

  object_class->dispose = inf_tcp_connection_dispose;
  object_class->finalize = inf_tcp_connection_finalize;
  object_class->set_property = inf_tcp_connection_set_property;
  object_class->get_property = inf_tcp_connection_get_property;

  tcp_connection_class->sent = NULL;
  tcp_connection_class->received = NULL;
  tcp_connection_class->error = inf_tcp_connection_error;

  inf_tcp_connection_error_quark = g_quark_from_static_string(
    "INF_TCP_CONNECTION_ERROR"
  );

  g_object_class_install_property(
    object_class,
    PROP_IO,
    g_param_spec_object(
      "io",
      "IO",
      "I/O handler",
      INF_TYPE_IO,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_STATUS,
    g_param_spec_enum(
      "status",
      "Status",
      "Status of the TCP connection",
      INF_TYPE_TCP_CONNECTION_STATUS,
      INF_TCP_CONNECTION_CLOSED,
      G_PARAM_READABLE
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_REMOTE_ADDRESS,
    g_param_spec_boxed(
      "remote-address",
      "Remote address",
      "Address to connect to",
      INF_TYPE_IP_ADDRESS,
      G_PARAM_READWRITE
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_REMOTE_PORT,
    g_param_spec_uint(
      "remote-port",
      "Remote port",
      "Port to connect to",
      0,
      65535,
      0,
      G_PARAM_READWRITE
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_LOCAL_ADDRESS,
    g_param_spec_boxed(
      "local-address",
      "Local address",
      "The local address of the connection",
      INF_TYPE_IP_ADDRESS,
      G_PARAM_READABLE
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_LOCAL_PORT,
    g_param_spec_uint(
      "local-port",
      "Local port",
      "The local port of the connection",
      0,
      65535,
      0,
      G_PARAM_READABLE
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_DEVICE_INDEX,
    g_param_spec_uint(
      "device-index",
      "Device index",
      "The index of the device to use for the connection",
      0,
      G_MAXUINT,
      0,
      G_PARAM_READWRITE
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_DEVICE_NAME,
    g_param_spec_string(
      "device-name",
      "Device name",
      "The name of the device to use for the connection, such as `eth0'",
      NULL,
      G_PARAM_READWRITE
    )
  );

  /**
   * InfTcpConnection::sent:
   * @connection: The #InfTcpConnection through which the data has been sent
   * @data: A #gpointer refering to the data that has been sent
   * @length: A #guint holding the number of bytes that has been sent
   */
  tcp_connection_signals[SENT] = g_signal_new(
    "sent",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfTcpConnectionClass, sent),
    NULL, NULL,
    inf_marshal_VOID__POINTER_UINT,
    G_TYPE_NONE,
    2,
    G_TYPE_POINTER,
    G_TYPE_UINT
  );

  /**
   * InfTcpConnection::received:
   * @connection: The #InfTcpConnection through which the data has been received
   * @data: A #gpointer refering to the data that has been received
   * @length: A #guint holding the number of bytes that has been received
   */
  tcp_connection_signals[RECEIVED] = g_signal_new(
    "received",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfTcpConnectionClass, received),
    NULL, NULL,
    inf_marshal_VOID__POINTER_UINT,
    G_TYPE_NONE,
    2,
    G_TYPE_POINTER,
    G_TYPE_UINT
  );

  /**
   * InfTcpConnection::error:
   * @connection: The erroneous #InfTcpConnection
   * @error: A pointer to a #GError object with details on the error
   */
  tcp_connection_signals[ERROR] = g_signal_new(
    "error",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfTcpConnectionClass, error),
    NULL, NULL,
    inf_marshal_VOID__POINTER,
    G_TYPE_NONE,
    1,
    G_TYPE_POINTER /* actually a GError* */
  );
}

GType
inf_tcp_connection_status_get_type(void)
{
  static GType tcp_connection_status_type = 0;

  if(!tcp_connection_status_type)
  {
    static const GEnumValue tcp_connection_status_values[] = {
      {
        INF_TCP_CONNECTION_CONNECTING,
        "INF_TCP_CONNECTION_CONNECTING",
        "connecting"
      }, {
        INF_TCP_CONNECTION_CONNECTED,
        "INF_TCP_CONNECTION_CONNECTED",
        "connected"
      }, {
        INF_TCP_CONNECTION_CLOSED,
        "INF_TCP_CONNECTION_CLOSED",
        "closed"
      }, {
        0,
        NULL,
        NULL
      }
    };

    tcp_connection_status_type = g_enum_register_static(
      "InfTcpConnectionStatus",
      tcp_connection_status_values
    );
  }

  return tcp_connection_status_type;
}

GType
inf_tcp_connection_get_type(void)
{
  static GType tcp_connection_type = 0;

  if(!tcp_connection_type)
  {
    static const GTypeInfo tcp_connection_type_info = {
      sizeof(InfTcpConnectionClass),  /* class_size */
      NULL,                           /* base_init */
      NULL,                           /* base_finalize */
      inf_tcp_connection_class_init,  /* class_init */
      NULL,                           /* class_finalize */
      NULL,                           /* class_data */
      sizeof(InfTcpConnection),       /* instance_size */
      0,                              /* n_preallocs */
      inf_tcp_connection_init,        /* instance_init */
      NULL                            /* value_table */
    };

    tcp_connection_type = g_type_register_static(
      G_TYPE_OBJECT,
      "InfTcpConnection",
      &tcp_connection_type_info,
      0
    );
  }

  return tcp_connection_type;
}

/**
 * inf_tcp_connection_open:
 * @connection: A #InfTcpConnection.
 * #error: Location to store error information.
 *
 * Attempts to open @connection. Make sure to have set the "remote-address"
 * and "remote-port" property before calling this function. If an error
 * occurs, the function returns FALSE and @error is set. Note however that
 * the connection might not be fully open when the function returns
 * (check the "status" property if you need to know). If an asynchronous
 * error occurs while the connection is being opened, the "error" signal
 * is emitted.
 **/
gboolean
inf_tcp_connection_open(InfTcpConnection* connection,
                        GError** error)
{
  InfTcpConnectionPrivate* priv;
/*  char device_name[IF_NAMESIZE];*/
  int result;

  union {
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
  } native_address;

  struct sockaddr* addr;
  socklen_t addrlen;

  g_return_val_if_fail(INF_IS_TCP_CONNECTION(connection), FALSE);
  priv = INF_TCP_CONNECTION_PRIVATE(connection);

  g_return_val_if_fail(priv->io != NULL, FALSE);
  g_return_val_if_fail(priv->status == INF_TCP_CONNECTION_CLOSED, FALSE);
  g_return_val_if_fail(priv->remote_address != NULL, FALSE);
  g_return_val_if_fail(priv->remote_port != 0, FALSE);

  switch(inf_ip_address_get_family(priv->remote_address))
  {
  case INF_IP_ADDRESS_IPV4:
    priv->socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    addr = (struct sockaddr*)&native_address.in;
    addrlen = sizeof(struct sockaddr_in);

    memcpy(
      &native_address.in.sin_addr,
      inf_ip_address_get_raw(priv->remote_address),
      sizeof(struct in_addr)
    );

    native_address.in.sin_family = AF_INET;
    native_address.in.sin_port = htons(priv->remote_port);

    break;
  case INF_IP_ADDRESS_IPV6:
    priv->socket = socket(PF_INET6, SOCK_STREAM, IPPROTO_TCP);
    addr = (struct sockaddr*)&native_address.in6;
    addrlen = sizeof(struct sockaddr_in6);

    memcpy(
      &native_address.in6.sin6_addr,
      inf_ip_address_get_raw(priv->remote_address),
      sizeof(struct in6_addr)
    );

    native_address.in6.sin6_family = AF_INET6;
    native_address.in6.sin6_port = htons(priv->remote_port);
    native_address.in6.sin6_flowinfo = 0;
    native_address.in6.sin6_scope_id = priv->device_index;

    break;
  default:
    g_assert_not_reached();
    break;
  }

  if(priv->socket == -1)
  {
    inf_tcp_connection_make_system_error(errno, error);
    return FALSE;
  }

  /* Note: The following requires root permissions. We rather set the
   * sin6_scope_id (see above) assuming a link-local address. */
#if 0
  /* Bind to interface */
  if(priv->device_index != 0)
  {
    if(if_indextoname(priv->device_index, device_name) == NULL)
    {
      inf_tcp_connection_make_system_error(errno, error);
      close(priv->socket);
      priv->socket = -1;
    }

    result = setsockopt(
      priv->socket,
      SOL_SOCKET,
      SO_BINDTODEVICE,
      device_name,
      strlen(device_name) + 1
    );

    if(result == -1)
    {
      printf("Error here `%s' for device %s\n", strerror(errno), device_name);
      inf_tcp_connection_make_system_error(errno, error);

      close(priv->socket);
      priv->socket = -1;
      return FALSE;
    }
  }
#endif

  /* Set socket non-blocking */
  result = fcntl(priv->socket, F_GETFL);
  if(result == -1)
  {
    inf_tcp_connection_make_system_error(errno, error);

    close(priv->socket);
    priv->socket = -1;
    return FALSE;
  }

  if(fcntl(priv->socket, F_SETFL, result | O_NONBLOCK) == -1)
  {
    inf_tcp_connection_make_system_error(errno, error);

    close(priv->socket);
    priv->socket = -1;
    return FALSE;
  }

  /* Connect */
  do
  {
    result = connect(priv->socket, addr, addrlen);
    if(result == -1 && errno != EINTR && errno != EINPROGRESS)
    {
      inf_tcp_connection_make_system_error(errno, error);

      close(priv->socket);
      priv->socket = -1;

      return FALSE;
    }
  } while(result == -1 && errno != EINPROGRESS);

  if(result == 0)
  {
    /* Connection fully established */
    inf_tcp_connection_connected(connection);
  }
  else
  {
    /* Connection establishment in progress */
    priv->events = INF_IO_OUTGOING | INF_IO_ERROR;

    inf_io_watch(
      priv->io,
      &priv->socket,
      priv->events,
      inf_tcp_connection_io,
      connection
    );

    priv->status = INF_TCP_CONNECTION_CONNECTING;
    g_object_notify(G_OBJECT(connection), "status");
  }

  return TRUE;
}

/**
 * inf_tcp_connection_close:
 * @connection: A #InfTcpConnection.
 *
 * Closes a TCP connection that is either open or currently connecting.
 **/
void
inf_tcp_connection_close(InfTcpConnection* connection)
{
  InfTcpConnectionPrivate* priv;

  g_return_if_fail(INF_IS_TCP_CONNECTION(connection));
  
  priv = INF_TCP_CONNECTION_PRIVATE(connection);
  g_return_if_fail(priv->status != INF_TCP_CONNECTION_CLOSED);

  priv->events = 0;

  inf_io_watch(
    priv->io,
    &priv->socket,
    priv->events,
    inf_tcp_connection_io,
    connection
  );

  close(priv->socket);
  priv->socket = -1;

  priv->status = INF_TCP_CONNECTION_CLOSED;
  g_object_notify(G_OBJECT(connection), "status");
}

/**
 * inf_tcp_connection_send:
 * @connection: A #InfTcpConnection with status %INF_TCP_CONNECTION_CONNECTED.
 * @data: The data to send.
 * @len: Number of bytes to send.
 *
 * Sends data through the TCP connection. The data is not sent immediately,
 * but enqueued to a buffer and will be sent as soon as kernel space
 * becomes available. The "sent" signal will be emitted when data has
 * really been sent.
 **/
void
inf_tcp_connection_send(InfTcpConnection* connection,
                        gconstpointer data,
                        guint len)
{
  InfTcpConnectionPrivate* priv;

  g_return_if_fail(INF_IS_TCP_CONNECTION(connection));
  g_return_if_fail(len == 0 || data != NULL);

  priv = INF_TCP_CONNECTION_PRIVATE(connection);
  g_return_if_fail(priv->status == INF_TCP_CONNECTION_CONNECTED);

  /* Move queue data back onto the beginning of the queue, if not already */
  if(priv->alloc - priv->front_pos < len && priv->back_pos > 0)
  {
    memmove(
      priv->queue,
      priv->queue + priv->back_pos,
      priv->front_pos - priv->back_pos
    );

    priv->front_pos -= priv->back_pos;
    priv->back_pos = 0;
  }

  /* Allocate more memory if there is still not enough space */
  if(priv->alloc - priv->front_pos < len)
  {
    /* Make sure we allocate enough */
    priv->alloc = priv->front_pos + len;

    /* Always allocate a multiple of 1024 */
    if(priv->alloc % 1024 != 0)
      priv->alloc = priv->alloc + (1024 - priv->alloc % 1024);

    priv->queue = g_realloc(priv->queue, priv->alloc);
  }

  memcpy(priv->queue + priv->front_pos, data, len);
  priv->front_pos += len;

  if(~priv->events & INF_IO_OUTGOING)
  {
    priv->events |= INF_IO_OUTGOING;

    inf_io_watch(
      priv->io,
      &priv->socket,
      priv->events,
      inf_tcp_connection_io,
      connection
    );
  }
}

/**
 * inf_tcp_connection_get_remote_address:
 * @connection: A #InfTcpConnection.
 *
 * Returns the IP address of the remote site.
 *
 * Return Value: A #InfIpAddress owned by @connection. You do not need to
 * free it, but need to make your own copy if you want to keep it longer than
 * @connection's lifetime.
 **/
InfIpAddress*
inf_tcp_connection_get_remote_address(InfTcpConnection* connection)
{
  g_return_val_if_fail(INF_IS_TCP_CONNECTION(connection), NULL);
  return INF_TCP_CONNECTION_PRIVATE(connection)->remote_address;
}

/**
 * inf_tcp_connection_get_remote_port:
 * @connection: A #InfTcpConnection.
 *
 * Returns the port of the remote site to which @connection is (or was)
 * connected or connecting.
 *
 * Return Value: The port of the remote site.
 **/
guint
inf_tcp_connection_get_remote_port(InfTcpConnection* connection)
{
  g_return_val_if_fail(INF_IS_TCP_CONNECTION(connection), 0);
  return INF_TCP_CONNECTION_PRIVATE(connection)->remote_port;
}

/* Creates a new TCP connection from an accepted socket. This is only used
 * by InfdTcpServer and should not be considered regular API. Do not call
 * this function. Language bindings should not wrap it. */
InfTcpConnection*
_inf_tcp_connection_accepted(InfIo* io,
                             InfNativeSocket socket,
                             GError** error)
{
  InfTcpConnection* connection;
  InfTcpConnectionPrivate* priv;
  int result;

  InfIpAddress* address;
  guint port;

  g_return_val_if_fail(INF_IS_IO(io), NULL);
  g_return_val_if_fail(socket != -1, NULL);

  result = fcntl(socket, F_GETFL);
  if(result == -1)
  {
    inf_tcp_connection_make_system_error(errno, error);
    return NULL;
  }

  if(fcntl(socket, F_SETFL, result | O_NONBLOCK) == -1)
  {
    inf_tcp_connection_make_system_error(errno, error);
    return NULL;
  }

  inf_tcp_connection_addr_info(socket, FALSE, &address, &port);
  g_return_val_if_fail(address != NULL, NULL);
  g_return_val_if_fail(port != 0, NULL);

  connection = INF_TCP_CONNECTION(
    g_object_new(
      INF_TYPE_TCP_CONNECTION,
      "io", io,
      "remote-address", address,
      "remote-port", port,
      NULL
    )
  );

  inf_ip_address_free(address);

  priv = INF_TCP_CONNECTION_PRIVATE(connection);
  priv->socket = socket;

  inf_tcp_connection_connected(connection);
  return connection;
}

/* vim:set et sw=2 ts=2: */