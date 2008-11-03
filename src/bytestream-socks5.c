/*
 * bytestream-socks5.c - Source for GabbleBytestreamSocks5
 * Copyright (C) 2007-2008 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "bytestream-socks5.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <loudmouth/loudmouth.h>
#include <telepathy-glib/interfaces.h>

#define DEBUG_FLAG GABBLE_DEBUG_BYTESTREAM

#include "base64.h"
#include "bytestream-factory.h"
#include "bytestream-iface.h"
#include "connection.h"
#include "debug.h"
#include "disco.h"
#include "gabble-signals-marshal.h"
#include "namespaces.h"
#include "util.h"

static void
bytestream_iface_init (gpointer g_iface, gpointer iface_data);

G_DEFINE_TYPE_WITH_CODE (GabbleBytestreamSocks5, gabble_bytestream_socks5,
    G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (GABBLE_TYPE_BYTESTREAM_IFACE,
      bytestream_iface_init));

/* signals */
enum
{
  DATA_RECEIVED,
  STATE_CHANGED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {0};

/* properties */
enum
{
  PROP_CONNECTION = 1,
  PROP_PEER_HANDLE,
  PROP_PEER_HANDLE_TYPE,
  PROP_STREAM_ID,
  PROP_STREAM_INIT_ID,
  PROP_PEER_JID,
  PROP_PEER_RESOURCE,
  PROP_STATE,
  PROP_PROTOCOL,
  LAST_PROPERTY
};

enum _Socks5State
{
  SOCKS5_STATE_INVALID,
  SOCKS5_STATE_AUTH_REQUEST_SENT,
  SOCKS5_STATE_CONNECT_REQUESTED,
  SOCKS5_STATE_CONNECTED,
  SOCKS5_STATE_AWAITING_AUTH_REQUEST,
  SOCKS5_STATE_AWAITING_COMMAND,
};

typedef enum _Socks5State Socks5State;

/* SOCKS5 commands */
#define SOCKS5_VERSION     0x05
#define SOCKS5_CMD_CONNECT 0x01 
#define SOCKS5_RESERVED    0x00
#define SOCKS5_ATYP_DOMAIN 0x03
#define SOCKS5_STATUS_OK   0x00
#define SOCKS5_AUTH_NONE   0x00

struct _Streamhost
{
  gchar *jid;
  gchar *host;
  guint port;
};
typedef struct _Streamhost Streamhost;

struct _GabbleBytestreamSocks5Private
{
  GabbleConnection *conn;
  TpHandle peer_handle;
  gchar *stream_id;
  gchar *stream_init_id;
  gchar *peer_resource;
  GabbleBytestreamState bytestream_state;
  gchar *peer_jid;

  GSList *streamhosts;
  LmMessage *msg_for_acknowledge_connection;

  GIOChannel *io_channel;
  Socks5State socks5_state;

  gint read_watch;
  GString *read_buffer;

  gint write_watch;
  GString *write_buffer;
  gsize write_position;

  gint error_watch;

  gboolean dispose_has_run;
};

#define GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE(obj) ((obj)->priv)

static void socks5_connect_next (GabbleBytestreamSocks5 *self);

static void gabble_bytestream_socks5_close (GabbleBytestreamIface *iface,
    GError *error);

static void
gabble_bytestream_socks5_init (GabbleBytestreamSocks5 *self)
{
  GabbleBytestreamSocks5Private *priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GABBLE_TYPE_BYTESTREAM_SOCKS5, GabbleBytestreamSocks5Private);

  self->priv = priv;
}

static void
gabble_bytestream_socks5_dispose (GObject *object)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (object);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  TpHandleRepoIface *contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  if (priv->dispose_has_run)
    return;

  priv->dispose_has_run = TRUE;

  tp_handle_unref (contact_repo, priv->peer_handle);

  if (priv->bytestream_state != GABBLE_BYTESTREAM_STATE_CLOSED)
    {
      gabble_bytestream_iface_close (GABBLE_BYTESTREAM_IFACE (self), NULL);
    }

  G_OBJECT_CLASS (gabble_bytestream_socks5_parent_class)->dispose (object);
}

static void
gabble_bytestream_socks5_finalize (GObject *object)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (object);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  g_free (priv->stream_id);
  g_free (priv->stream_init_id);
  g_free (priv->peer_resource);
  g_free (priv->peer_jid);

  G_OBJECT_CLASS (gabble_bytestream_socks5_parent_class)->finalize (object);
}

static void
gabble_bytestream_socks5_get_property (GObject *object,
                                       guint property_id,
                                       GValue *value,
                                       GParamSpec *pspec)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (object);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        g_value_set_object (value, priv->conn);
        break;
      case PROP_PEER_HANDLE:
        g_value_set_uint (value, priv->peer_handle);
        break;
      case PROP_PEER_HANDLE_TYPE:
        g_value_set_uint (value, TP_HANDLE_TYPE_CONTACT);
        break;
      case PROP_STREAM_ID:
        g_value_set_string (value, priv->stream_id);
        break;
      case PROP_STREAM_INIT_ID:
        g_value_set_string (value, priv->stream_init_id);
        break;
      case PROP_PEER_RESOURCE:
        g_value_set_string (value, priv->peer_resource);
        break;
      case PROP_PEER_JID:
        g_value_set_string (value, priv->peer_jid);
        break;
      case PROP_STATE:
        g_value_set_uint (value, priv->bytestream_state);
        break;
      case PROP_PROTOCOL:
        g_value_set_string (value, NS_BYTESTREAMS);
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static void
gabble_bytestream_socks5_set_property (GObject *object,
                                       guint property_id,
                                       const GValue *value,
                                       GParamSpec *pspec)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (object);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  switch (property_id)
    {
      case PROP_CONNECTION:
        priv->conn = g_value_get_object (value);
        break;
      case PROP_PEER_HANDLE:
        priv->peer_handle = g_value_get_uint (value);
        break;
      case PROP_STREAM_ID:
        g_free (priv->stream_id);
        priv->stream_id = g_value_dup_string (value);
        break;
      case PROP_STREAM_INIT_ID:
        g_free (priv->stream_init_id);
        priv->stream_init_id = g_value_dup_string (value);
        break;
      case PROP_PEER_RESOURCE:
        g_free (priv->peer_resource);
        priv->peer_resource = g_value_dup_string (value);
        break;
      case PROP_STATE:
        if (priv->bytestream_state != g_value_get_uint (value))
            {
              priv->bytestream_state = g_value_get_uint (value);
              g_signal_emit (object, signals[STATE_CHANGED], 0, priv->bytestream_state);
            }
        break;
      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
        break;
    }
}

static GObject *
gabble_bytestream_socks5_constructor (GType type,
                                      guint n_props,
                                      GObjectConstructParam *props)
{
  GObject *obj;
  GabbleBytestreamSocks5Private *priv;
  TpHandleRepoIface *contact_repo;
  const gchar *jid;

  obj = G_OBJECT_CLASS (gabble_bytestream_socks5_parent_class)->
           constructor (type, n_props, props);

  priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (GABBLE_BYTESTREAM_SOCKS5 (obj));

  g_assert (priv->conn != NULL);
  g_assert (priv->peer_handle != 0);
  g_assert (priv->stream_id != NULL);

  contact_repo = tp_base_connection_get_handles (
      (TpBaseConnection *) priv->conn, TP_HANDLE_TYPE_CONTACT);

  tp_handle_ref (contact_repo, priv->peer_handle);

  jid = tp_handle_inspect (contact_repo, priv->peer_handle);

  if (priv->peer_resource != NULL)
    priv->peer_jid = g_strdup_printf ("%s/%s", jid, priv->peer_resource);
  else
    priv->peer_jid = g_strdup (jid);

  return obj;
}

static void
gabble_bytestream_socks5_class_init (
    GabbleBytestreamSocks5Class *gabble_bytestream_socks5_class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (gabble_bytestream_socks5_class);
  GParamSpec *param_spec;

  g_type_class_add_private (gabble_bytestream_socks5_class,
      sizeof (GabbleBytestreamSocks5Private));

  object_class->dispose = gabble_bytestream_socks5_dispose;
  object_class->finalize = gabble_bytestream_socks5_finalize;

  object_class->get_property = gabble_bytestream_socks5_get_property;
  object_class->set_property = gabble_bytestream_socks5_set_property;
  object_class->constructor = gabble_bytestream_socks5_constructor;

   g_object_class_override_property (object_class, PROP_CONNECTION,
      "connection");
   g_object_class_override_property (object_class, PROP_PEER_HANDLE,
       "peer-handle");
   g_object_class_override_property (object_class, PROP_PEER_HANDLE_TYPE,
       "peer-handle-type");
   g_object_class_override_property (object_class, PROP_STREAM_ID,
       "stream-id");
   g_object_class_override_property (object_class, PROP_PEER_JID,
       "peer-jid");
   g_object_class_override_property (object_class, PROP_STATE,
       "state");
   g_object_class_override_property (object_class, PROP_PROTOCOL,
       "protocol");

  param_spec = g_param_spec_string (
      "peer-resource",
      "Peer resource",
      "the resource used by the remote peer during the SI, if any",
      NULL,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_PEER_RESOURCE,
      param_spec);

  param_spec = g_param_spec_string (
      "stream-init-id",
      "stream init ID",
      "the iq ID of the SI request, if any",
      NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_STREAM_INIT_ID,
      param_spec);

  signals[DATA_RECEIVED] =
    g_signal_new ("data-received",
                  G_OBJECT_CLASS_TYPE (gabble_bytestream_socks5_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  g_cclosure_marshal_VOID__UINT_POINTER,
                  G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_POINTER);

  signals[STATE_CHANGED] =
    g_signal_new ("state-changed",
                  G_OBJECT_CLASS_TYPE (gabble_bytestream_socks5_class),
                  G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                  0,
                  NULL, NULL,
                  gabble_marshal_VOID__UINT,
                  G_TYPE_NONE, 1, G_TYPE_UINT);
}

static gboolean 
socks5_channel_writable_cb (GIOChannel *source, 
                            GIOCondition condition,
                            gpointer data) 
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (data);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  gsize remaining_length = priv->write_buffer->len - priv->write_position;
  GIOStatus status;
  gsize bytes_written;

  g_assert (remaining_length > 0);

  status = g_io_channel_write_chars (priv->io_channel,
      &priv->write_buffer->str [priv->write_position], remaining_length,
      &bytes_written, NULL);

  remaining_length -= bytes_written;
  if (remaining_length == 0)
    {
      g_string_truncate (priv->write_buffer, 0);
      priv->write_position = 0;
      priv->write_watch = 0;
      return FALSE;
    }

  priv->write_position += bytes_written;

  if (status != G_IO_STATUS_NORMAL)
    {
      /* FIXME handle errors. */
      return FALSE;
    }

  return TRUE;
}

static void
socks5_schedule_write (GabbleBytestreamSocks5 *self,
                       const gchar *msg,
                       gsize len)
{
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  g_string_append_len (priv->write_buffer, msg, len);

  if (!priv->write_watch)
    priv->write_watch = g_io_add_watch (priv->io_channel, G_IO_OUT,
        socks5_channel_writable_cb, self);
}

static gsize
socks5_handle_received_data (GabbleBytestreamSocks5 *self,
                             GString *string)
{
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  gchar msg[47] = {'\0'};

  switch (priv->socks5_state)
    {
      case SOCKS5_STATE_AUTH_REQUEST_SENT:
        if (string->len < 2)
          return 0;

        /* FIXME: do proper checking */
        g_assert (string->str[0] == SOCKS5_VERSION &&
            string->str[1] == SOCKS5_STATUS_OK);

        msg[0] = SOCKS5_VERSION;
        msg[1] = SOCKS5_CMD_CONNECT;
        msg[2] = SOCKS5_RESERVED;
        msg[3] = SOCKS5_ATYP_DOMAIN;
        /* Lenght of a hex SHA1 */
        msg[4] = 40;
        /* FIXME: send the domainname */
        /* Port: 0 */
        msg[45] = 0x00;
        msg[46] = 0x00;

        socks5_schedule_write (self, msg, 47);

        priv->socks5_state = SOCKS5_STATE_CONNECT_REQUESTED;

        return 2;

      case SOCKS5_STATE_CONNECT_REQUESTED:
        if (string->len < 2)
          return 0;

        /* FIXME: handle errors */
        g_assert (string->str[0] == SOCKS5_VERSION &&
            string->str[1] == SOCKS5_STATUS_OK);

        priv->socks5_state = SOCKS5_STATE_CONNECTED;

        _gabble_connection_acknowledge_set_iq (priv->conn, priv->msg_for_acknowledge_connection);

        return 2;

      case SOCKS5_STATE_AWAITING_AUTH_REQUEST:
        if (string->len < 3)
          return 0;

        /* FIXME: handle errors */
        g_assert (string->str[0] == SOCKS5_VERSION &&
            string->str[1] == 1 &&
            string->str[2] == SOCKS5_AUTH_NONE);

        msg[0] = SOCKS5_VERSION;
        msg[1] = SOCKS5_AUTH_NONE;

        socks5_schedule_write (self, msg, 2);

        priv->socks5_state = SOCKS5_STATE_AWAITING_COMMAND;

        return 3;

      case SOCKS5_STATE_AWAITING_COMMAND:
        if (string->len < 47)
          return 0;

        /* FIXME: check the values */

        msg[0] = SOCKS5_VERSION;
        msg[1] = SOCKS5_STATUS_OK; // XXX rinominare OK?

        socks5_schedule_write (self, msg, 2);

        priv->socks5_state = SOCKS5_STATE_CONNECTED;

        return 47;

      case SOCKS5_STATE_CONNECTED:
        g_signal_emit (G_OBJECT (self), signals[DATA_RECEIVED], 0, priv->peer_handle, string);

        return string->len;

      default:
        /* FIXME: handle errors */
        return string->len;
    }
}

static gboolean 
socks5_channel_readable_cb (GIOChannel *source, 
                            GIOCondition condition,
                            gpointer data) 
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (data);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  gsize available_length =
    priv->read_buffer->allocated_len - priv->read_buffer->len - 1;
  GIOStatus status;
  gsize bytes_read;
  gsize used_bytes;

  if (available_length == 0)
    {
      g_string_set_size (priv->read_buffer, priv->read_buffer->len * 2);
      available_length = priv->read_buffer->allocated_len - priv->read_buffer->len - 1;
    }

  status = g_io_channel_read_chars (source,
      &priv->read_buffer->str [priv->read_buffer->len], available_length,
      &bytes_read, NULL);

  priv->read_buffer->len += bytes_read;
  priv->read_buffer->str[priv->read_buffer->len] = '\0';

  used_bytes = socks5_handle_received_data (self, priv->read_buffer);
  g_string_erase (priv->read_buffer, 0, used_bytes);

  return TRUE;
}

static gboolean 
socks5_channel_error_cb (GIOChannel *source, 
                         GIOCondition condition,
                         gpointer data) 
{
  /* FIXME: handle errors */
  g_critical ("I/O error\n");

  return FALSE;
}

static gboolean
socks5_connect (gpointer data)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (data);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  Streamhost* streamhost;
  struct addrinfo req = {0};
  struct addrinfo *address_list;
  struct addrinfo *streamhost_address;
  gint fd;
  gint socket_flags;
  gint res;
  gchar msg[3];

  if (priv->streamhosts)
    {
      streamhost = priv->streamhosts->data;
    }
  else
    {
      DEBUG ("No more streamhosts to streamhost, closing");
      gabble_bytestream_socks5_close (GABBLE_BYTESTREAM_IFACE (self), NULL);

      return FALSE;
    }

  DEBUG ("Trying streamhost %s on port %d", streamhost->host,
      streamhost->port);

  req.ai_family = AF_UNSPEC;
  req.ai_socktype = SOCK_STREAM;
  req.ai_protocol = IPPROTO_TCP;

  if (getaddrinfo (streamhost->host, NULL, &req, &address_list) != 0)
    {
      DEBUG ("getaddrinfo on %s failed", streamhost->host);
      socks5_connect_next (self);

      return FALSE;
    }

  fd = -1;
  streamhost_address = address_list;

  while (fd < 0 && streamhost_address) 
    {
      ((struct sockaddr_in *) streamhost_address->ai_addr)->sin_port =
        htons (streamhost->port);

      fd = socket (streamhost_address->ai_family,
          streamhost_address->ai_socktype, streamhost_address->ai_protocol);

      if (fd >= 0)
        break;

      streamhost_address = streamhost_address->ai_next;
    }


  if (fd < 0)
    {
      gabble_bytestream_socks5_close (GABBLE_BYTESTREAM_IFACE (self), NULL);
      freeaddrinfo (address_list);

      return FALSE;
    }

  /* Set non-blocking */
  socket_flags = fcntl (fd, F_GETFL, 0);
  fcntl (fd, F_SETFL, socket_flags | O_NONBLOCK);
  
  res = connect (fd, (struct sockaddr*)streamhost_address->ai_addr, streamhost_address->ai_addrlen);

  freeaddrinfo (address_list);

  if (res < 0 && errno != EINPROGRESS)
    {
      close (fd);
      socks5_connect_next (self);

      return FALSE;
    }

  priv->io_channel = g_io_channel_unix_new (fd);

  g_io_channel_set_encoding (priv->io_channel, NULL, NULL);
  g_io_channel_set_buffered (priv->io_channel, FALSE);
  g_io_channel_set_close_on_unref (priv->io_channel, TRUE);

  priv->read_watch = g_io_add_watch(priv->io_channel, G_IO_IN,
      socks5_channel_readable_cb, self);
  priv->error_watch = g_io_add_watch(priv->io_channel, G_IO_HUP | G_IO_ERR,
      socks5_channel_error_cb, self);

  g_assert (priv->write_buffer == NULL);
  priv->write_buffer = g_string_new ("");

  g_assert (priv->read_buffer == NULL);
  priv->read_buffer = g_string_sized_new (4096);

  msg[0] = SOCKS5_VERSION;
  /* Number of auth methods we are offering */
  msg[1] = 1;
  msg[2] = SOCKS5_AUTH_NONE;

  socks5_schedule_write (self, msg, 3);

  priv->socks5_state = SOCKS5_STATE_AUTH_REQUEST_SENT;

  return FALSE;
}

static void
socks5_connect_next (GabbleBytestreamSocks5 *self)
{
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  g_assert (priv->streamhosts != NULL);

  /* FIXME: free the streamhost */
  priv->streamhosts = g_slist_delete_link (priv->streamhosts, priv->streamhosts);

  socks5_connect (self);
}

/**
 * gabble_bytestream_socks5_add_streamhost
 *
 * Adds the streamhost as a candidate for connection.
 */
void
gabble_bytestream_socks5_add_streamhost (GabbleBytestreamSocks5 *self,
                                         LmMessageNode *streamhost_node)
{
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  const gchar *zeroconf;
  const gchar *jid;
  const gchar *host;
  const gchar *port;
  guint numeric_port;
  Streamhost *streamhost;

  g_return_if_fail (strcmp (streamhost_node->name, "streamhost") == 0);

  zeroconf = lm_message_node_get_attribute (streamhost_node, "zeroconf");
  if (zeroconf != NULL)
    {
      DEBUG ("zeroconf streamhosts are not supported");
      return;
    }

  jid = lm_message_node_get_attribute (streamhost_node, "jid");
  if (jid == NULL)
    {
      DEBUG ("streamhost doesn't contain a JID");
      return;
    }

  host = lm_message_node_get_attribute (streamhost_node, "host");
  if (host == NULL)
    {
      DEBUG ("streamhost doesn't contain a host");
      return;
    }

  port = lm_message_node_get_attribute (streamhost_node, "port");
  if (port == NULL)
    {
      DEBUG ("streamhost doesn't contain a port");
      return;
    }

  numeric_port = strtoul (port, NULL, 10);
  if (numeric_port <= 0)
    {
      DEBUG ("streamhost contain an invalid port: %s", port);
      return;
    }

  DEBUG ("streamhost with jid %s, host %s and port %d added", jid, host,
      numeric_port);

  streamhost = g_new0 (Streamhost, 1);
  streamhost->jid = g_strdup (jid);
  streamhost->host = g_strdup (host);
  streamhost->port = numeric_port;

  priv->streamhosts = g_slist_append (priv->streamhosts, streamhost);
}

/**
 * gabble_bytestream_socks5_connect_to_streamhost
 *
 * Try to connect to a streamhost.
 */
void
gabble_bytestream_socks5_connect_to_streamhost (GabbleBytestreamSocks5 *self,
                                                LmMessage *msg)

{
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  priv->msg_for_acknowledge_connection = lm_message_ref (msg);

  g_idle_add(socks5_connect, self);
}

/*
 * gabble_bytestream_socks5_send
 *
 * Implements gabble_bytestream_iface_send on GabbleBytestreamIface
 */
static gboolean
gabble_bytestream_socks5_send (GabbleBytestreamIface *iface,
                               guint len,
                               const gchar *str)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (iface);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  if (priv->bytestream_state != GABBLE_BYTESTREAM_STATE_OPEN)
    {
      DEBUG ("can't send data through a not open bytestream (state: %d)",
          priv->bytestream_state);
      return FALSE;
    }

  socks5_schedule_write (self, str, len);

  return TRUE;
}

/*
 * gabble_bytestream_socks5_accept
 *
 * Implements gabble_bytestream_iface_accept on GabbleBytestreamIface
 */
static void
gabble_bytestream_socks5_accept (GabbleBytestreamIface *iface,
                                 GabbleBytestreamAugmentSiAcceptReply func,
                                 gpointer user_data)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (iface);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  LmMessage *msg;
  LmMessageNode *si;

  if (priv->bytestream_state != GABBLE_BYTESTREAM_STATE_LOCAL_PENDING)
    {
      /* The stream was previoulsy or automatically accepted */
      return;
    }

  msg = gabble_bytestream_factory_make_accept_iq (priv->peer_jid,
      priv->stream_init_id, NS_BYTESTREAMS);
  si = lm_message_node_get_child_with_namespace (msg->node, "si", NS_SI);
  g_assert (si != NULL);

  if (func != NULL)
    {
      /* let the caller add his profile specific data */
      func (si, user_data);
    }

  if (_gabble_connection_send (priv->conn, msg, NULL))
    {
      DEBUG ("stream %s with %s is now accepted", priv->stream_id,
          priv->peer_jid);
      g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_ACCEPTED, NULL);
    }

  lm_message_unref (msg);
}

static void
gabble_bytestream_socks5_decline (GabbleBytestreamSocks5 *self,
                               GError *error)
{
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  LmMessage *msg;

  g_return_if_fail (priv->bytestream_state == GABBLE_BYTESTREAM_STATE_LOCAL_PENDING);

  msg = lm_message_build (priv->peer_jid, LM_MESSAGE_TYPE_IQ,
      '@', "type", "error",
      '@', "id", priv->stream_init_id,
      NULL);

  if (error != NULL && error->domain == GABBLE_XMPP_ERROR)
    {
      gabble_xmpp_error_to_node (error->code, msg->node, error->message);
    }
  else
    {
      gabble_xmpp_error_to_node (XMPP_ERROR_FORBIDDEN, msg->node,
          "Offer Declined");
    }

  _gabble_connection_send (priv->conn, msg, NULL);

  lm_message_unref (msg);

  g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSED, NULL);
}

/*
 * gabble_bytestream_socks5_close
 *
 * Implements gabble_bytestream_iface_close on GabbleBytestreamIface
 */
static void
gabble_bytestream_socks5_close (GabbleBytestreamIface *iface,
                                GError *error)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (iface);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);

  if (priv->bytestream_state == GABBLE_BYTESTREAM_STATE_CLOSED)
     /* bytestream already closed, do nothing */
     return;

  if (priv->bytestream_state == GABBLE_BYTESTREAM_STATE_LOCAL_PENDING)
    {
      /* Stream was created using SI so we decline the request */
      gabble_bytestream_socks5_decline (self, error);
    }
  else
    {
      LmMessage *msg;

      DEBUG ("send Socks5 close stanza");

      if (priv->io_channel)
        {
          if (priv->read_watch != 0)
            g_source_remove (priv->read_watch);
          if (priv->write_watch != 0)
            g_source_remove (priv->write_watch);
          if (priv->error_watch != 0)
            g_source_remove (priv->error_watch);
          g_io_channel_unref (priv->io_channel);
          priv->io_channel = NULL;
        }

      msg = lm_message_build (priv->peer_jid, LM_MESSAGE_TYPE_IQ,
          '@', "type", "set",
          '(', "close", "",
            '@', "xmlns", NS_BYTESTREAMS,
            '@', "sid", priv->stream_id,
          ')', NULL);

      /* We don't really care about the answer as the bytestream
       * is closed anyway. */
      _gabble_connection_send_with_reply (priv->conn, msg,
          NULL, NULL, NULL, NULL);

      lm_message_unref (msg);

      g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSED, NULL);
    }
}

static LmHandlerResult
socks5_init_reply_cb (GabbleConnection *conn,
                   LmMessage *sent_msg,
                   LmMessage *reply_msg,
                   GObject *obj,
                   gpointer user_data)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (obj);

  if (lm_message_get_sub_type (reply_msg) == LM_MESSAGE_SUB_TYPE_RESULT)
    {
      /* yeah, stream initiated */
      DEBUG ("Socks5 stream initiated");
      g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_OPEN, NULL);
    }
  else
    {
      DEBUG ("error during Socks5 initiation");
      g_object_set (self, "state", GABBLE_BYTESTREAM_STATE_CLOSED, NULL);
    }

  return LM_HANDLER_RESULT_REMOVE_MESSAGE;
}

static gboolean
socks5_listen_cb (GIOChannel *source,
                  GIOCondition condition,
                  gpointer data)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (data);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  gint fd;
  struct sockaddr_in addr;
  guint addr_len = sizeof (addr);
  int flags;

  fd = accept (g_io_channel_unix_get_fd (source), (struct sockaddr *) &addr,
      &addr_len);

  /* Set non-blocking */
  flags = fcntl (fd, F_GETFL, 0);
  fcntl (fd, F_SETFL, flags | O_NONBLOCK);

  priv->io_channel = g_io_channel_unix_new (fd);

  g_io_channel_set_encoding (priv->io_channel, NULL, NULL);
  g_io_channel_set_buffered (priv->io_channel, FALSE);
  g_io_channel_set_close_on_unref (priv->io_channel, TRUE);

  priv->read_watch = g_io_add_watch(priv->io_channel, G_IO_IN,
      socks5_channel_readable_cb, self);
  priv->error_watch = g_io_add_watch(priv->io_channel, G_IO_HUP | G_IO_ERR,
      socks5_channel_error_cb, self);

  g_assert (priv->write_buffer == NULL);
  priv->write_buffer = g_string_new ("");

  g_assert (priv->read_buffer == NULL);
  priv->read_buffer = g_string_sized_new (4096);

  priv->socks5_state = SOCKS5_STATE_AWAITING_AUTH_REQUEST;

  return FALSE;
}

/*
 * gabble_bytestream_socks5_initiate
 *
 * Implements gabble_bytestream_iface_initiate on GabbleBytestreamIface
 */
static gboolean
gabble_bytestream_socks5_initiate (GabbleBytestreamIface *iface)
{
  GabbleBytestreamSocks5 *self = GABBLE_BYTESTREAM_SOCKS5 (iface);
  GabbleBytestreamSocks5Private *priv = GABBLE_BYTESTREAM_SOCKS5_GET_PRIVATE (self);
  struct sockaddr_in addr;
  gint fd;
  GIOChannel *channel;
  LmMessage *msg;

  if (priv->bytestream_state != GABBLE_BYTESTREAM_STATE_INITIATING)
    {
      DEBUG ("bytestream is not is the initiating state (state %d)",
          priv->bytestream_state);
      return FALSE;
    }

  fd = socket (AF_INET, SOCK_STREAM, 0);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons (5087);

  /* FIXME: check the return values */
  bind (fd, (struct sockaddr *)&addr, sizeof (addr));
  listen (fd, 5);
  channel = g_io_channel_unix_new (fd);

  g_io_channel_set_close_on_unref (channel, TRUE);

  /* FIXME handle errors */
  priv->read_watch = g_io_add_watch (channel, G_IO_IN, socks5_listen_cb, self);

  /* FIXME: send a real address and port. */
  msg = lm_message_build (priv->peer_jid, LM_MESSAGE_TYPE_IQ,
      '@', "type", "set",
      '(', "query", "",
        '@', "xmlns", NS_BYTESTREAMS,
        '@', "sid", priv->stream_id,
        '@', "mode", "tcp",
        '(', "streamhost", "",
          '@', "jid", priv->peer_jid,
          '@', "host", "127.0.0.1",
          '@', "port", "5087",
        ')',
      ')', NULL);

  if (!_gabble_connection_send_with_reply (priv->conn, msg,
      socks5_init_reply_cb, G_OBJECT (self), NULL, NULL))
    {
      DEBUG ("Error when sending Socks5 init stanza");

      lm_message_unref (msg);
      return FALSE;
    }

  lm_message_unref (msg);

  return TRUE;
}

static void
bytestream_iface_init (gpointer g_iface,
                       gpointer iface_data)
{
  GabbleBytestreamIfaceClass *klass = (GabbleBytestreamIfaceClass *) g_iface;

  klass->initiate = gabble_bytestream_socks5_initiate;
  klass->send = gabble_bytestream_socks5_send;
  klass->close = gabble_bytestream_socks5_close;
  klass->accept = gabble_bytestream_socks5_accept;
}
