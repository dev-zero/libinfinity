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

/**
 * SECTION:infd-directory
 * @short_description: Directory of published documents
 * @see_also: #InfcBrowser, #InfdStorage
 * @include: libinfinity/server/infd-directory.h
 * @stability: Unstable
 *
 * The #InfdDirectory manages a directory of documents. An item in the
 * directory is also called &quot;node&quot;. A node may either be a
 * subdirectory or a document (also called "note"). Notes may be of
 * different type - there may be plain text notes, rich text notes,
 * graphics notes, etc.
 *
 * #InfdStorage defines where the directory structure and the notes are read
 * from and how there are permanently stored.
 **/

#include <libinfinity/server/infd-directory.h>

#include <libinfinity/common/inf-session.h>
#include <libinfinity/common/inf-net-object.h>
#include <libinfinity/common/inf-error.h>
#include <libinfinity/common/inf-xml-util.h>
#include <libinfinity/inf-marshal.h>

#include <string.h>

/* TODO: Close and store sessions when there are no available users for
 * some time. */

typedef struct _InfdDirectoryNode InfdDirectoryNode;
struct _InfdDirectoryNode {
  InfdDirectoryNode* parent;
  InfdDirectoryNode* prev;
  InfdDirectoryNode* next;

  InfdStorageNodeType type;
  guint id;
  gchar* name;

  union {
    struct {
      /* Running session, or NULL */
      InfdSessionProxy* session;
      /* Session type */
      InfdNotePlugin* plugin;
    } note;

    struct {
      /* List of connections that have this folder open and have to be
       * notified if something happens with it. */
      GSList* connections;
      /* First child node */
      InfdDirectoryNode* child;
      /* Whether we requested the node already from the background storage.
       * This is required because the nodes field may be NULL due to an empty
       * subdirectory or due to an unexplored subdirectory. */
      gboolean explored;
    } subdir;
  } shared;
};

typedef struct _InfdDirectoryPrivate InfdDirectoryPrivate;
struct _InfdDirectoryPrivate {
  InfIo* io;
  InfdStorage* storage;
  GPtrArray* directory_methods;
  GPtrArray* session_methods;
  InfConnectionManager* connection_manager;
  InfConnectionManagerGroup* group; /* TODO: This should be a property */

  GHashTable* plugins; /* Registered plugins */
 /* TODO: ConnectionManagerGroup has already a connection list */
  GSList* connections;

  guint node_counter;
  GHashTable* nodes; /* Mapping from id to node */
  InfdDirectoryNode* root;
};

#if 0
typedef gboolean(*InfdDirectoryCreateStorageNodeFunc)(InfdDirectoryStorage*,
                                                      const gchar*,
                                                      GError**);
#endif

enum {
  PROP_0,

  PROP_IO,
  PROP_STORAGE,
  PROP_CONNECTION_MANAGER,
  PROP_METHOD_MANAGER
};

enum {
  NODE_ADDED,
  NODE_REMOVED,

  LAST_SIGNAL
};

#define INFD_DIRECTORY_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), INFD_TYPE_DIRECTORY, InfdDirectoryPrivate))

/* These make sure that the node iter points to is contained in directory */
#define infd_directory_return_if_iter_fail(directory, iter) \
  g_return_if_fail( \
    g_hash_table_lookup( \
      INFD_DIRECTORY_PRIVATE(INFD_DIRECTORY(directory))->nodes, \
      GUINT_TO_POINTER((iter)->node_id) \
    ) == (iter)->node \
  )

#define infd_directory_return_val_if_iter_fail(directory, iter, val) \
  g_return_val_if_fail( \
    g_hash_table_lookup( \
      INFD_DIRECTORY_PRIVATE(INFD_DIRECTORY(directory))->nodes, \
      GUINT_TO_POINTER((iter)->node_id) \
    ) == (iter)->node, \
    val \
  )

/* These make sure that node is a subdirectory node */
#define infd_directory_return_if_subdir_fail(node) \
  g_return_if_fail( \
    ((InfdDirectoryNode*)node)->type == INFD_STORAGE_NODE_SUBDIRECTORY \
  )

#define infd_directory_return_val_if_subdir_fail(node, val) \
  g_return_val_if_fail( \
    ((InfdDirectoryNode*)node)->type == INFD_STORAGE_NODE_SUBDIRECTORY, \
    val \
  )

static GObjectClass* parent_class;
static guint directory_signals[LAST_SIGNAL];

/*
 * Path handling.
 */

/* Returns the complete path to this node in the given GString */
static void
infd_directory_node_get_path_string(InfdDirectoryNode* node,
                                    GString* string)
{
  g_return_if_fail(node != NULL);
  g_return_if_fail(string != NULL);

  if(node->parent != NULL)
  {
    /* Each node except the root node has a name */
    g_assert(node->name != NULL);

    /* Make sure to not recurse if our parent is the root node because
     * this would add an additional slash */
    if(node->parent->parent != NULL)
      infd_directory_node_get_path_string(node->parent, string);
 
    g_string_append_c(string, '/');
    g_string_append(string, node->name);
  }
  else
  {
    /* This node has no parent, so it is the root node */
    g_assert(node->name == NULL);
    g_string_append_c(string, '/');
  }
}

static void
infd_directory_node_get_path(InfdDirectoryNode* node,
                             gchar** path,
                             gsize* len)
{
  GString* str;

  g_return_if_fail(node != NULL);
  g_return_if_fail(path != NULL);

  str = g_string_sized_new(128);

  infd_directory_node_get_path_string(node, str);
  *path = str->str;

  if(len != NULL)
    *len = str->len;

  g_string_free(str, FALSE);
}

static void
infd_directory_node_make_path(InfdDirectoryNode* node,
                              const gchar* name,
                              gchar** path,
                              gsize* len)
{
  GString* str;

  g_return_if_fail(node != NULL);
  g_return_if_fail(name != NULL);
  g_return_if_fail(path != NULL);

  str = g_string_sized_new(128);

  infd_directory_node_get_path_string(node, str);
  if(node->parent != NULL)
    g_string_append_c(str, '/');

  g_string_append(str, name);

  *path = str->str;
  if(len != NULL)
    *len = str->len;

  g_string_free(str, FALSE);
}

/*
 * Node construction and removal
 */

static void
infd_directory_node_link(InfdDirectoryNode* node,
                         InfdDirectoryNode* parent)
{
  g_return_if_fail(node != NULL);
  g_return_if_fail(parent != NULL);
  infd_directory_return_if_subdir_fail(parent);

  node->prev = NULL;
  if(parent->shared.subdir.child != NULL)
  {
    parent->shared.subdir.child->prev = node;
    node->next = parent->shared.subdir.child;
  }
  else
  {
    node->next = NULL;
  }

  parent->shared.subdir.child = node;
}

static void
infd_directory_node_unlink(InfdDirectoryNode* node)
{
  g_return_if_fail(node != NULL);
  g_return_if_fail(node->parent != NULL);

  if(node->prev != NULL)
  {
    node->prev->next = node->next;
  }
  else 
  {
    g_assert(node->parent->type == INFD_STORAGE_NODE_SUBDIRECTORY);
    node->parent->shared.subdir.child = node->next;
  }

  if(node->next != NULL)
    node->next->prev = node->prev;
}

/* This function takes ownership of name */
static InfdDirectoryNode*
infd_directory_node_new_common(InfdDirectory* directory,
                               InfdDirectoryNode* parent,
                               InfdStorageNodeType type,
                               gchar* name)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  node = g_slice_new(InfdDirectoryNode);
  node->parent = parent;
  node->type = type;
  node->id = priv->node_counter ++;
  node->name = name;

  if(parent != NULL)
  {
    infd_directory_node_link(node, parent);
  }
  else
  {
    node->prev = NULL;
    node->next = NULL;
  }

  g_hash_table_insert(priv->nodes, GUINT_TO_POINTER(node->id), node);
  return node;
}

static InfdDirectoryNode*
infd_directory_node_new_subdirectory(InfdDirectory* directory,
                                     InfdDirectoryNode* parent,
                                     gchar* name)
{
  InfdDirectoryNode* node;

  node = infd_directory_node_new_common(
    directory,
    parent,
    INFD_STORAGE_NODE_SUBDIRECTORY,
    name
  );

  node->shared.subdir.connections = NULL;
  node->shared.subdir.child = NULL;
  node->shared.subdir.explored = FALSE;

  return node;
}

static InfdDirectoryNode*
infd_directory_node_new_note(InfdDirectory* directory,
                             InfdDirectoryNode* parent,
                             gchar* name,
                             InfdNotePlugin* plugin)
{
  InfdDirectoryNode* node;

  node = infd_directory_node_new_common(
    directory,
    parent,
    INFD_STORAGE_NODE_NOTE,
    name
  );

  node->shared.note.session = NULL;
  node->shared.note.plugin = plugin;

  return node;
}

/* Notes are saved into the storage when save_notes is TRUE. */
static void
infd_directory_node_free(InfdDirectory* directory,
                         InfdDirectoryNode* node,
                         gboolean save_notes)
{
  InfdDirectoryPrivate* priv;
  gchar* path;
  gboolean removed;

  g_return_if_fail(INFD_IS_DIRECTORY(directory));
  g_return_if_fail(node != NULL);

  priv = INFD_DIRECTORY_PRIVATE(directory);

  switch(node->type)
  {
  case INFD_STORAGE_NODE_SUBDIRECTORY:
    g_slist_free(node->shared.subdir.connections);

    /* Free child nodes */
    if(node->shared.subdir.explored == TRUE)
    {
      while(node->shared.subdir.child != NULL)
      {
        infd_directory_node_free(
          directory,
          node->shared.subdir.child,
          save_notes
        );
      }
    }

    break;
  case INFD_STORAGE_NODE_NOTE:
    if(save_notes == TRUE && node->shared.note.session != NULL)
    {
      infd_directory_node_get_path(node->parent, &path, NULL);

      /* TODO: Error handling */
      node->shared.note.plugin->session_write(
        priv->storage,
        infd_session_proxy_get_session(node->shared.note.session),
        path,
        NULL
      );

      g_free(path);
    }

    if(node->shared.note.session != NULL)
      g_object_unref(G_OBJECT(node->shared.note.session));

    break;
  default:
    g_assert_not_reached();
    break;
  }

  if(node->parent != NULL)
    infd_directory_node_unlink(node);

  removed = g_hash_table_remove(priv->nodes, GUINT_TO_POINTER(node->id));
  g_assert(removed == TRUE);

  g_free(node->name);
  g_slice_free(InfdDirectoryNode, node);
}

static void
infd_directory_node_remove_connection(InfdDirectoryNode* node,
                                      InfXmlConnection* connection)
{
  InfdDirectoryNode* child;
  GSList* item;

  g_assert(node->type == INFD_STORAGE_NODE_SUBDIRECTORY);
  g_assert(node->shared.subdir.explored == TRUE);

  item = g_slist_find(node->shared.subdir.connections, connection);

  /* Note that if the connection is not in this node's connection list,
   * then it cannot be in a child's list either. */
  if(item != NULL)
  {
    node->shared.subdir.connections = g_slist_delete_link(
      node->shared.subdir.connections,
      item
    );

    if(node->shared.subdir.explored == TRUE)
    {
      for(child = node->shared.subdir.child;
          child != NULL;
          child = child->next)
      {
        if(child->type == INFD_STORAGE_NODE_SUBDIRECTORY &&
           child->shared.subdir.explored == TRUE)
        {
          infd_directory_node_remove_connection(child, connection);
        }
      }
    }
    else
    {
      g_assert(node->shared.subdir.connections == NULL);
    }
  }
}

/*
 * Node synchronization.
 */

static const InfConnectionManagerMethodDesc*
infd_directory_find_session_method_for_network(InfdDirectory* directory,
                                               const gchar* network)
{
  InfdDirectoryPrivate* priv;
  InfConnectionManagerMethodDesc* method;
  guint i;

  priv = INFD_DIRECTORY_PRIVATE(directory);
  for(i = 0; i < priv->session_methods->len; ++ i)
  {
    method = g_ptr_array_index(priv->session_methods, i);
    if(method == NULL) return NULL; /* array is null-terminated */

    if(strcmp(method->network, "network") == 0)
      return method;
  }

  return NULL;
}

/* Creates XML request to tell someone about a new node */
static xmlNodePtr
infd_directory_node_register_to_xml(InfdDirectoryNode* node)
{
  xmlNodePtr xml;
  gchar id_buf[16];
  gchar parent_buf[16];
  const gchar* typename;

  g_assert(node->parent != NULL);

  sprintf(id_buf, "%u", node->id);
  sprintf(parent_buf, "%u", node->parent->id);

  switch(node->type)
  {
  case INFD_STORAGE_NODE_SUBDIRECTORY:
    typename = "InfSubdirectory";
    break;
  case INFD_STORAGE_NODE_NOTE:
    typename = node->shared.note.plugin->note_type;
    break;
  default:
    g_assert_not_reached();
    break;
  }

  xml = xmlNewNode(NULL, (const xmlChar*)"add-node");

  xmlNewProp(xml, (const xmlChar*)"id", (const xmlChar*)id_buf);
  xmlNewProp(xml, (const xmlChar*)"parent", (const xmlChar*)parent_buf);
  xmlNewProp(xml, (const xmlChar*)"name", (const xmlChar*)node->name);
  xmlNewProp(xml, (const xmlChar*)"type", (const xmlChar*)typename);

  return xml;
}

/* Creates XML request to tell someone about a removed node */
static xmlNodePtr
infd_directory_node_unregister_to_xml(InfdDirectoryNode* node)
{
  xmlNodePtr xml;
  gchar id_buf[16];

  sprintf(id_buf, "%u", node->id);

  xml = xmlNewNode(NULL, (const xmlChar*)"remove-node");
  xmlNewProp(xml, (const xmlChar*)"id", (const xmlChar*)id_buf);

  return xml;
}

/* Sends a message to the given connections. We cannot always send to all
 * group members because some messages are only supposed to be sent to
 * clients that explored a certain subdirectory. */
static void
infd_directory_send(InfdDirectory* directory,
                    GSList* connections,
                    InfXmlConnection* exclude,
                    xmlNodePtr xml)
{
  InfdDirectoryPrivate* priv;
  GSList* item;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  if(connections == NULL ||
     (connections->data == exclude && connections->next == NULL))
  {
    xmlFreeNode(xml);
  }
  else
  {
    for(item = connections; item != NULL; item = g_slist_next(item))
    {
      if(item->data == exclude) continue;

      /* Do not copy this item if it is the last item to be sent because the
       * connection manager takes ownership */
      if(item->next != NULL &&
         (item->next->data != exclude || item->next->next != NULL))
      {
        inf_connection_manager_group_send_to_connection(
          priv->group,
          INF_XML_CONNECTION(item->data),
          xmlCopyNode(xml, 1)
        );
      }
      else
      {
        inf_connection_manager_group_send_to_connection(
          priv->group,
          INF_XML_CONNECTION(item->data),
          xml
        );
      }
    }
  }
}

/* Announces the presence of a new node. This is not done in
 * infd_directory_node_new because we do not want to do this for all
 * nodes we create (namely not for the root node). */
static void
infd_directory_node_register(InfdDirectory* directory,
                             InfdDirectoryNode* node,
                             InfXmlConnection* seq_conn,
                             guint seq)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryIter iter;
  xmlNodePtr xml;

#if 0
  g_return_if_fail(INFD_IS_DIRECTORY(directory));
  g_return_if_fail(node != NULL);
  g_return_if_fail(node->parent != NULL);
  infd_directory_return_if_subdir_fail(node->parent);
#endif

  priv = INFD_DIRECTORY_PRIVATE(directory);
  iter.node_id = node->id;
  iter.node = node;

  g_signal_emit(
    G_OBJECT(directory),
    directory_signals[NODE_ADDED],
    0,
    &iter
  );

  if(seq_conn != NULL)
  {
    xml = infd_directory_node_register_to_xml(node);
    inf_xml_util_set_attribute_uint(xml, "seq", seq);

    inf_connection_manager_group_send_to_connection(
      priv->group,
      seq_conn,
      xml
    );
  }

  if(node->parent->shared.subdir.connections != NULL)
  {
    xml = infd_directory_node_register_to_xml(node);

    infd_directory_send(
      directory,
      node->parent->shared.subdir.connections,
      seq_conn,
      xml
    );
  }
}

/* Announces that a node is removed. Again, this is not done in
 * infd_directory_node_free because we do not want to do this for
 * every subnode if a subdirectory is freed. */
static void
infd_directory_node_unregister(InfdDirectory* directory,
                               InfdDirectoryNode* node,
                               InfXmlConnection* seq_conn,
                               guint seq)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryIter iter;
  xmlNodePtr xml;

#if 0
  g_return_if_fail(INFD_IS_DIRECTORY(directory));
  g_return_if_fail(node != NULL);
  g_return_if_fail(node->parent != NULL);
  infd_directory_return_if_subdir_fail(node->parent);
#endif

  priv = INFD_DIRECTORY_PRIVATE(directory);
  iter.node_id = node->id;
  iter.node = node;

  g_signal_emit(
    G_OBJECT(directory),
    directory_signals[NODE_REMOVED],
    0,
    &iter
  );

  if(seq_conn != NULL)
  {
    xml = infd_directory_node_unregister_to_xml(node);
    inf_xml_util_set_attribute_uint(xml, "seq", seq);

    inf_connection_manager_group_send_to_connection(
      priv->group,
      seq_conn,
      xml
    );
  }

  if(node->parent->shared.subdir.connections != NULL)
  {
    xml = infd_directory_node_unregister_to_xml(node);

    infd_directory_send(
      directory,
      node->parent->shared.subdir.connections,
      seq_conn,
      xml
    );
  }
}

/*
 * Directory tree operations.
 */

static InfdDirectoryNode*
infd_directory_node_find_child_by_name(InfdDirectoryNode* parent,
                                       const gchar* name)
{
  InfdDirectoryNode* node;

  infd_directory_return_val_if_subdir_fail(parent, NULL);

  for(node = parent->shared.subdir.child; node != NULL; node = node->next)
  {
    /* TODO: Make this Unicode aware */
    if(g_ascii_strcasecmp(node->name, name) == 0)
      return node;
  }

  return NULL;
}

static gboolean
infd_directory_node_explore(InfdDirectory* directory,
                            InfdDirectoryNode* node,
                            GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdStorageNode* storage_node;
  InfdDirectoryNode* new_node;
  InfdNotePlugin* plugin;
  GError* local_error;
  GSList* list;
  GSList* item;
  gchar* path;
  gsize len;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  g_assert(priv->storage != NULL);
  g_assert(node->type == INFD_STORAGE_NODE_SUBDIRECTORY);
  g_assert(node->shared.subdir.explored == FALSE);

  infd_directory_node_get_path(node, &path, &len);

  local_error = NULL;

  list = infd_storage_read_subdirectory(priv->storage, path, &local_error);

  if(local_error != NULL)
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  for(item = list; item != NULL; item = g_slist_next(item))
  {
    storage_node = (InfdStorageNode*)item->data;

    /* TODO: Transfer ownership of storade_node->name to
     * infd_directory_new_*? */
    switch(storage_node->type)
    {
    case INFD_STORAGE_NODE_SUBDIRECTORY:
      new_node = infd_directory_node_new_subdirectory(
        directory,
        node,
        g_strdup(storage_node->name)
      );
      
      break;
    case INFD_STORAGE_NODE_NOTE:
      /* TODO: Currently we ignore notes of unknown type. Perhaps we should
       * report some error. */
      plugin = g_hash_table_lookup(priv->plugins, storage_node->identifier);
      if(plugin != NULL)
      {
        new_node = infd_directory_node_new_note(
          directory,
          node,
          g_strdup(storage_node->name),
          plugin
        );
      }

      break;
    default:
      g_assert_not_reached();
      break;
    }

    if(new_node != NULL)
    {
      /* Announce the new node. In most cases, this does nothing on the
       * network because there are no connections that have this node open
       * (otherwise, we would already have explored the node earlier).
       * However, if the background storage is replaced by a new one, the root
       * folder of the new storage will be explored immediately (see below in
       * infd_directory_set_storage()) and there might still be connections
       * interesting in root folder changes (because they opened the root
       * folder from the old storage). Also, local users might be interested
       * in the new node. */
      infd_directory_node_register(directory, new_node, NULL, 0);
    }
  }

  infd_storage_node_list_free(list);

  node->shared.subdir.explored = TRUE;
  return TRUE;
}

static InfdDirectoryNode*
infd_directory_node_add_subdirectory(InfdDirectory* directory,
                                     InfdDirectoryNode* parent,
                                     const gchar* name,
                                     InfXmlConnection* seq_conn,
                                     guint seq,
                                     GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;
  gboolean result;
  gchar* path;

  infd_directory_return_val_if_subdir_fail(parent, NULL);
  g_return_val_if_fail(parent->shared.subdir.explored == TRUE, NULL);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  g_assert(priv->storage != NULL);

  node = infd_directory_node_find_child_by_name(parent, name);
  if(node != NULL)
  {
    g_set_error(
      error,
      inf_directory_error_quark(),
      INF_DIRECTORY_ERROR_NODE_EXISTS,
      "%s",
      inf_directory_strerror(INF_DIRECTORY_ERROR_NODE_EXISTS)
    );

    return NULL;
  }
  else
  {
    infd_directory_node_make_path(parent, name, &path, NULL);

    result = infd_storage_create_subdirectory(priv->storage, path, error);

    g_free(path);
    if(result == FALSE) return NULL;

    node = infd_directory_node_new_subdirectory(
      directory,
      parent,
      g_strdup(name)
    );

    infd_directory_node_register(directory, node, seq_conn, seq);
    return node;
  }
}

static InfdDirectoryNode*
infd_directory_node_add_note(InfdDirectory* directory,
                             InfdDirectoryNode* parent,
                             const gchar* name,
                             InfdNotePlugin* plugin,
                             InfXmlConnection* seq_conn,
                             guint seq,
                             GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;
  InfSession* session;
  gchar* group_name;
  InfConnectionManagerGroup* group;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  infd_directory_return_val_if_subdir_fail(parent, NULL);
  g_return_val_if_fail(parent->shared.subdir.explored == TRUE, NULL);

  node = infd_directory_node_find_child_by_name(parent, name);
  if(node != NULL)
  {
    g_set_error(
      error,
      inf_directory_error_quark(),
      INF_DIRECTORY_ERROR_NODE_EXISTS,
      "%s",
      inf_directory_strerror(INF_DIRECTORY_ERROR_NODE_EXISTS)
    );

    return NULL;
  }
  else
  {
    node = infd_directory_node_new_note(
      directory,
      parent,
      g_strdup(name),
      plugin
    );

    group_name = g_strdup_printf("InfSession_%u", node->id);

    /* Open group */
    group = inf_connection_manager_open_group(
      priv->connection_manager,
      group_name,
      NULL, /* net_object */
      (InfConnectionManagerMethodDesc**)priv->session_methods->pdata
    );

    g_free(group_name);

    session = plugin->session_new(
      priv->io,
      priv->connection_manager,
      NULL,
      NULL
    );

    g_assert(session != NULL);

    node->shared.note.session = g_object_new(
      INFD_TYPE_SESSION_PROXY,
      "session", session,
      "subscription-group", group,
      NULL
    );

    inf_connection_manager_group_set_object(
      group,
      INF_NET_OBJECT(node->shared.note.session)
    );

    g_object_unref(G_OBJECT(session));
    inf_connection_manager_group_unref(group);

    g_assert(node->shared.note.session != NULL);

    /* TODO: Save initial node */

    infd_directory_node_register(directory, node, seq_conn, seq);
    return node;
  }
}

static gboolean
infd_directory_node_remove(InfdDirectory* directory,
                           InfdDirectoryNode* node,
                           InfXmlConnection* seq_conn,
                           guint seq,
                           GError** error)
{
  InfdDirectoryPrivate* priv;
  gboolean result;
  gchar* path;

  priv = INFD_DIRECTORY_PRIVATE(directory);
  g_assert(priv->storage != NULL);

  infd_directory_node_get_path(node, &path, NULL);
  result = infd_storage_remove_node(priv->storage, path, error);
  g_free(path);

  if(result == FALSE)
    return FALSE;

  infd_directory_node_unregister(directory, node, seq_conn, seq);
  infd_directory_node_free(directory, node, FALSE);
  return TRUE;
}

static InfdSessionProxy*
infd_directory_node_get_session(InfdDirectory* directory,
                                InfdDirectoryNode* node,
                                GError** error)
{
  InfdDirectoryPrivate* priv;
  InfConnectionManagerGroup* group;
  InfSession* session;
  gchar* path;
  gchar* group_name;

  g_return_val_if_fail(node->type == INFD_STORAGE_NODE_NOTE, NULL);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  g_assert(priv->storage != NULL);

  if(node->shared.note.session != NULL)
    return node->shared.note.session;

  group_name = g_strdup_printf("InfSession_%u", node->id);

  group = inf_connection_manager_open_group(
    priv->connection_manager,
    group_name,
    NULL,
    (InfConnectionManagerMethodDesc**)priv->session_methods->pdata
  );

  g_free(group_name);

  infd_directory_node_get_path(node, &path, NULL);
  session = node->shared.note.plugin->session_read(
    priv->storage,
    priv->io,
    priv->connection_manager,
    path,
    error
  );
  g_free(path);

  if(session == NULL)
    return NULL;

  node->shared.note.session = g_object_new(
    INFD_TYPE_SESSION_PROXY,
    "session", session,
    "subscription-group", group,
    NULL
  );

  inf_connection_manager_group_set_object(
    group,
    INF_NET_OBJECT(node->shared.note.session)
  );

  inf_connection_manager_group_unref(group);
  g_object_unref(session);

  return node->shared.note.session;
}

/*
 * Network command handling.
 */

static InfdDirectoryNode*
infd_directory_get_node_from_xml(InfdDirectory* directory,
                                 xmlNodePtr xml,
                                 const gchar* attrib,
                                 GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;
  guint node_id;
  gboolean has_node;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  has_node = inf_xml_util_get_attribute_uint_required(
    xml,
    attrib,
    &node_id,
    error
  );

  if(has_node == FALSE) return NULL;

  node = g_hash_table_lookup(priv->nodes, GUINT_TO_POINTER(node_id));
  if(node == NULL)
  {
    g_set_error(
      error,
      inf_directory_error_quark(),
      INF_DIRECTORY_ERROR_NO_SUCH_NODE,
      "%s",
      inf_directory_strerror(INF_DIRECTORY_ERROR_NO_SUCH_NODE)
    );

    return NULL;
  }

  return node;
}

static InfdDirectoryNode*
infd_directory_get_node_from_xml_typed(InfdDirectory* directory,
                                       xmlNodePtr xml,
                                       const gchar* attrib,
                                       InfdStorageNodeType type,
                                       GError** error)
{
  InfdDirectoryNode* node;
  node = infd_directory_get_node_from_xml(directory, xml, attrib, error);

  if(node != NULL && node->type != type)
  {
    switch(type)
    {
    case INFD_STORAGE_NODE_SUBDIRECTORY:
      g_set_error(
        error,
        inf_directory_error_quark(),
        INF_DIRECTORY_ERROR_NOT_A_SUBDIRECTORY,
        "%s",
        inf_directory_strerror(INF_DIRECTORY_ERROR_NOT_A_SUBDIRECTORY)
      );

      return NULL;
    case INFD_STORAGE_NODE_NOTE:
      g_set_error(
        error,
        inf_directory_error_quark(),
        INF_DIRECTORY_ERROR_NOT_A_NOTE,
        "%s",
        inf_directory_strerror(INF_DIRECTORY_ERROR_NOT_A_NOTE)
      );

      return NULL;
    default:
      g_assert_not_reached();
      return NULL;
    }
  }

  return node;
}

static gboolean
infd_directory_handle_explore_node(InfdDirectory* directory,
                                   InfXmlConnection* connection,
                                   const xmlNodePtr xml,
                                   GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;
  InfdDirectoryNode* child;
  xmlChar* seq_attr;
  xmlNodePtr reply_xml;
  guint total;
  gchar total_str[16];

  priv = INFD_DIRECTORY_PRIVATE(directory);

  node = infd_directory_get_node_from_xml_typed(
    directory,
    xml,
    "id",
    INFD_STORAGE_NODE_SUBDIRECTORY,
    error
  );

  if(node->shared.subdir.explored == FALSE)
    if(infd_directory_node_explore(directory, node, error) == FALSE)
      return FALSE;

  if(g_slist_find(node->shared.subdir.connections, connection) != NULL)
  {
    g_set_error(
      error,
      inf_directory_error_quark(),
      INF_DIRECTORY_ERROR_ALREADY_EXPLORED,
      "%s",
      inf_directory_strerror(INF_DIRECTORY_ERROR_ALREADY_EXPLORED)
    );

    return FALSE;
  }

  total = 0;
  for(child = node->shared.subdir.child; child != NULL; child = child->next)
    ++ total;
  sprintf(total_str, "%u", total);

  seq_attr = xmlGetProp(xml, (const xmlChar*)"seq");
  reply_xml = xmlNewNode(NULL, (const xmlChar*)"explore-begin");
  xmlNewProp(reply_xml, (const xmlChar*)"total", (const xmlChar*)total_str);
  if(seq_attr != NULL)
    xmlNewProp(reply_xml, (const xmlChar*)"seq", seq_attr);

  inf_connection_manager_group_send_to_connection(
    priv->group,
    connection,
    reply_xml
  );

  for(child = node->shared.subdir.child; child != NULL; child = child->next)
  {
    reply_xml = infd_directory_node_register_to_xml(child);
    if(seq_attr != NULL)
      xmlNewProp(reply_xml, (const xmlChar*)"seq", seq_attr);

    inf_connection_manager_group_send_to_connection(
      priv->group,
      connection,
      reply_xml
    );
  }

  reply_xml = xmlNewNode(NULL, (const xmlChar*)"explore-end");

  if(seq_attr != NULL)
  {
    xmlNewProp(reply_xml, (const xmlChar*)"seq", seq_attr);
    xmlFree(seq_attr);
  }

  inf_connection_manager_group_send_to_connection(
    priv->group,
    connection,
    reply_xml
  );

  /* Remember that this connection explored that node so that it gets
   * notified when changes occur. */
  node->shared.subdir.connections = g_slist_prepend(
    node->shared.subdir.connections,
    connection
  );

  return TRUE;
}

static gboolean
infd_directory_handle_add_node(InfdDirectory* directory,
                               InfXmlConnection* connection,
                               const xmlNodePtr xml,
                               GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* parent;
  InfdDirectoryNode* node;
  InfdNotePlugin* plugin;
  xmlChar* name;
  xmlChar* type;
  gboolean has_seq;
  guint seq;
  GError* local_error;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  parent = infd_directory_get_node_from_xml_typed(
    directory,
    xml,
    "parent",
    INFD_STORAGE_NODE_SUBDIRECTORY,
    error
  );

  if(parent == NULL)
    return FALSE;

  type = inf_xml_util_get_attribute_required(xml, "type", error);
  if(type == NULL) return FALSE;

  if(strcmp((const gchar*)type, "InfSubdirectory") == 0)
  {
    /* No plugin because we want to create a directory */
    plugin = NULL;
    xmlFree(type);
  }
  else
  {
    plugin = g_hash_table_lookup(priv->plugins, (const gchar*)type);
    xmlFree(type);

    if(plugin == NULL)
    {
      g_set_error(
        error,
        inf_directory_error_quark(),
        INF_DIRECTORY_ERROR_TYPE_UNKNOWN,
        "%s",
        inf_directory_strerror(INF_DIRECTORY_ERROR_TYPE_UNKNOWN)
      );

      return FALSE;
    }
  }

  local_error = NULL;
  has_seq = inf_xml_util_get_attribute_uint(xml, "seq", &seq, &local_error);
  if(local_error != NULL)
  {
    g_propagate_error(error, local_error);
    return FALSE;
  }

  name = inf_xml_util_get_attribute_required(xml, "name", error);
  if(name == NULL) return FALSE;

  /* Note that seq is only passed uninitialized here if it is not used
   * anyway because seq_conn is NULL. */
  if(plugin == NULL)
  {
    node = infd_directory_node_add_subdirectory(
      directory,
      parent,
      (const gchar*)name,
      (has_seq == TRUE) ? connection : NULL,
      seq,
      error
    );
  }
  else
  {
    node = infd_directory_node_add_note(
      directory,
      parent,
      (const gchar*)name,
      plugin,
      (has_seq == TRUE) ? connection : NULL,
      seq,
      error
    );
  }

  xmlFree(name);

  if(node == NULL)
    return FALSE;

  return TRUE;
}

static gboolean
infd_directory_handle_remove_node(InfdDirectory* directory,
                                  InfXmlConnection* connection,
                                  const xmlNodePtr xml,
                                  GError** error)
{
  InfdDirectoryNode* node;
  guint seq;
  gboolean result;
  gboolean has_seq;

  node = infd_directory_get_node_from_xml(directory, xml, "id", error);
  if(node == NULL) return FALSE;

  has_seq = inf_xml_util_get_attribute_uint_required(xml, "seq", &seq, error);

  /* Note that seq is only passed uninitialized here if it is not used
   * anyway because seq_conn is NULL. */
  result = infd_directory_node_remove(
    directory,
    node,
    (has_seq == TRUE) ? connection : NULL,
    seq,
    error
  );

  return result;
}

static gboolean
infd_directory_handle_subscribe_session(InfdDirectory* directory,
                                        InfXmlConnection* connection,
                                        const xmlNodePtr xml,
                                        GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;
  InfdSessionProxy* proxy;
  InfConnectionManagerGroup* group;
  gchar* network;
  const InfConnectionManagerMethodDesc* method;
  xmlChar* seq_attr;
  xmlNodePtr reply_xml;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  node = infd_directory_get_node_from_xml_typed(
    directory,
    xml,
    "id",
    INFD_STORAGE_NODE_NOTE,
    error
  );

  if(node == NULL)
    return FALSE;

  /* TODO: Bail if this connection is either currently being synchronized to
   * or is already subscribed */

  proxy = infd_directory_node_get_session(directory, node, error);
  if(proxy == NULL)
    return FALSE;

  g_object_get(G_OBJECT(proxy), "subscription-group", &group, NULL);
  g_object_get(G_OBJECT(connection), "network", &network, NULL);

  method = inf_connection_manager_group_get_method_for_network(group, network);
  if(method == NULL)
  {
    /* Session does not support connection's network. */

    g_set_error(
      error,
      inf_directory_error_quark(),
      INF_DIRECTORY_ERROR_NETWORK_UNSUPPORTED,
      "The session does not support network '%s'.",
      network
    );

    inf_connection_manager_group_unref(group);
    g_free(network);

    return FALSE;
  }

  g_free(network);

  /* Reply that subscription was successful (so far, synchronization may
   * still fail) and tell identifier. */
  reply_xml = xmlNewNode(NULL, (const xmlChar*)"subscribe-session");

  xmlNewProp(
    reply_xml,
    (const xmlChar*)"group",
    (const xmlChar*)inf_connection_manager_group_get_name(group)
  );
  
  xmlNewProp(
    reply_xml,
    (const xmlChar*)"method",
    (const xmlChar*)method->name
  );

  inf_connection_manager_group_unref(group);
  inf_xml_util_set_attribute_uint(reply_xml, "id", node->id);

  seq_attr = xmlGetProp(xml, (const xmlChar*)"seq");
  if(seq_attr != NULL)
  {
    xmlNewProp(reply_xml, (const xmlChar*)"seq", seq_attr);
    xmlFree(seq_attr);
  }

  inf_connection_manager_group_send_to_connection(
    priv->group,
    connection,
    reply_xml
  );

  infd_session_proxy_subscribe_to(proxy, connection);
  return TRUE;
}

/*
 * Signal handlers.
 */

/* Required by infd_directory_connection_notify_status_cb() */
static void
infd_directory_remove_connection(InfdDirectory* directory,
                                 InfXmlConnection* connection);

static void
infd_directory_connection_notify_status_cb(InfXmlConnection* connection,
                                           const gchar* property,
                                           gpointer user_data)
{
  InfdDirectory* directory;
  InfXmlConnectionStatus status;

  directory = INFD_DIRECTORY(user_data);

  g_object_get(G_OBJECT(connection), "status", &status, NULL);

  if(status == INF_XML_CONNECTION_CLOSING ||
     status == INF_XML_CONNECTION_CLOSED)
  {
    infd_directory_remove_connection(directory, connection);
  }
}

static void
infd_directory_remove_connection(InfdDirectory* directory,
                                 InfXmlConnection* connection)
{
  InfdDirectoryPrivate* priv;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  if(priv->root != NULL && priv->root->shared.subdir.explored)
  {
    infd_directory_node_remove_connection(priv->root, connection);
  }

  g_signal_handlers_disconnect_by_func(
    G_OBJECT(connection),
    G_CALLBACK(infd_directory_connection_notify_status_cb),
    directory
  );

  inf_connection_manager_group_remove_connection(priv->group, connection);
  priv->connections = g_slist_remove(priv->connections, connection);
  g_object_unref(G_OBJECT(connection));
}

/*
 * Property modification.
 */

static void
infd_directory_set_storage(InfdDirectory* directory,
                           InfdStorage* storage)
{
  InfdDirectoryPrivate* priv;

  InfdDirectoryNode* child;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  if(priv->storage != NULL)
  {
    /* priv->root may be NULL if this is called from dispose. */
    if(priv->root != NULL && priv->root->shared.subdir.explored == TRUE)
    {
      /* Clear directory tree. This will cause all sessions to be saved in
       * storage. Note that sessions are not closed and further
       * modifications to the sessions will not be stored. */
      while((child = priv->root->shared.subdir.child) != NULL)
      {
        infd_directory_node_unregister(directory, child, NULL, 0);
        infd_directory_node_free(directory, child, TRUE);
      }
    }

    g_object_unref(G_OBJECT(priv->storage));
  }

  priv->storage = storage;

  if(storage != NULL)
  {
    /* root folder was explored before storage change, so keep it
     * explored. */
    if(priv->root->shared.subdir.explored == TRUE)
    {
      /* Need to set explored flag to FALSE to meet preconditions of
       * infd_directory_node_explore(). */
      priv->root->shared.subdir.explored = FALSE;

      /* TODO: Error handling? */
      infd_directory_node_explore(directory, priv->root, NULL);
    }

    g_object_ref(G_OBJECT(storage));
  }
}

static void
infd_directory_set_connection_manager(InfdDirectory* directory,
                                      InfConnectionManager* manager)
{
  InfdDirectoryPrivate* priv;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  /* construct/only */
  g_assert(priv->connection_manager == NULL);
  priv->connection_manager = manager;
  g_object_ref(manager);
}

static void
infd_directory_set_method_manager(InfdDirectory* directory,
                                  InfMethodManager* manager)
{
  InfdDirectoryPrivate* priv;
  const InfConnectionManagerMethodDesc* desc;
  const InfConnectionManagerMethodDesc* existing_desc;
  
  GSList* list;

  priv = INFD_DIRECTORY_PRIVATE(directory);

  /* construct/only */
  g_assert(priv->directory_methods == NULL);
  g_assert(priv->session_methods == NULL);
  
  priv->directory_methods = g_ptr_array_new();
  priv->session_methods = g_ptr_array_new();

  list = inf_method_manager_list_all_methods(manager);
  for( ; list != NULL; list = g_slist_next(list))
  {
    desc = (const InfConnectionManagerMethodDesc*)list->data;
    if(strcmp(desc->name, "central") == 0)
      g_ptr_array_add(priv->directory_methods, (gpointer)desc);

    /* TODO: Use first method for session methods. We should do something more
     * intelligent here. */
    existing_desc = infd_directory_find_session_method_for_network(
      directory,
      desc->network
    );

    if(existing_desc == NULL)
      g_ptr_array_add(priv->session_methods, (gpointer)desc);
  }

  /* null-terminate */
  g_ptr_array_add(priv->directory_methods, NULL);
  g_ptr_array_add(priv->session_methods, NULL);
}

/*
 * GObject overrides.
 */

static void
infd_directory_init(GTypeInstance* instance,
                    gpointer g_class)
{
  InfdDirectory* directory;
  InfdDirectoryPrivate* priv;

  directory = INFD_DIRECTORY(instance);
  priv = INFD_DIRECTORY_PRIVATE(directory);

  priv->io = NULL;
  priv->storage = NULL;
  priv->connection_manager = NULL;
  priv->directory_methods = NULL;
  priv->session_methods = NULL;

  priv->plugins = g_hash_table_new(g_str_hash, g_str_equal);
  priv->connections = NULL;

  priv->node_counter = 0;
  priv->nodes = g_hash_table_new(NULL, NULL);

  /* The root node has no name. */
  priv->root = infd_directory_node_new_subdirectory(directory, NULL, NULL);
}

static GObject*
infd_directory_constructor(GType type,
                           guint n_construct_properties,
                           GObjectConstructParam* construct_properties)
{
  GObject* object;
  InfdDirectory* directory;
  InfdDirectoryPrivate* priv;

  object = G_OBJECT_CLASS(parent_class)->constructor(
    type,
    n_construct_properties,
    construct_properties
  );

  directory = INFD_DIRECTORY(object);
  priv = INFD_DIRECTORY_PRIVATE(directory);

  /* TODO: Use default connection manager in case none is set */
  g_assert(priv->connection_manager != NULL);
  
  if(priv->directory_methods == NULL)
  {
    infd_directory_set_method_manager(
      directory,
      inf_method_manager_get_default()
    );
  }

  priv->group = inf_connection_manager_open_group(
    priv->connection_manager,
    "InfDirectory",
    INF_NET_OBJECT(directory),
    (InfConnectionManagerMethodDesc**)priv->directory_methods->pdata
  );

  g_assert(priv->connections == NULL);
  return object;
}

static void
infd_directory_dispose(GObject* object)
{
  InfdDirectory* directory;
  InfdDirectoryPrivate* priv;

  directory = INFD_DIRECTORY(object);
  priv = INFD_DIRECTORY_PRIVATE(directory);

  /* This frees the complete directory tree and saves sessions into the
   * storage. */
  infd_directory_node_free(directory, priv->root, TRUE);
  priv->root = NULL;

  g_hash_table_destroy(priv->nodes);
  priv->nodes = NULL;

  while(priv->connections != NULL)
    infd_directory_remove_connection(directory, priv->connections->data);

  /* We have dropped all references to connections now, so these do not try
   * to tell anyone that the directory tree has gone or whatever. */

  inf_connection_manager_group_unref(priv->group);
  g_object_unref(G_OBJECT(priv->connection_manager));
  infd_directory_set_storage(directory, NULL);

  g_ptr_array_free(priv->session_methods, TRUE);
  g_ptr_array_free(priv->directory_methods, TRUE);

  g_hash_table_destroy(priv->plugins);
  priv->plugins = NULL;

  if(priv->io != NULL)
  {
    g_object_unref(G_OBJECT(priv->io));
    priv->io = NULL;
  }

  G_OBJECT_CLASS(parent_class)->dispose(object);
}

static void
infd_directory_set_property(GObject* object,
                            guint prop_id,
                            const GValue* value,
                            GParamSpec* pspec)
{
  InfdDirectory* directory;
  InfdDirectoryPrivate* priv;
  InfMethodManager* manager;

  directory = INFD_DIRECTORY(object);
  priv = INFD_DIRECTORY_PRIVATE(directory);

  switch(prop_id)
  {
  case PROP_IO:
    g_assert(priv->io == NULL); /* construct only */
    priv->io = INF_IO(g_value_dup_object(value));
    break;
  case PROP_STORAGE:
    infd_directory_set_storage(
      directory,
      INFD_STORAGE(g_value_get_object(value))
    );

    break;
  case PROP_CONNECTION_MANAGER:
    infd_directory_set_connection_manager(
      directory,
      INF_CONNECTION_MANAGER(g_value_get_object(value))
    );

    break;
  case PROP_METHOD_MANAGER:
    manager = INF_METHOD_MANAGER(g_value_get_object(value));
    if(manager != NULL) infd_directory_set_method_manager(directory, manager);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
infd_directory_get_property(GObject* object,
                            guint prop_id,
                            GValue* value,
                            GParamSpec* pspec)
{
  InfdDirectory* directory;
  InfdDirectoryPrivate* priv;

  directory = INFD_DIRECTORY(object);
  priv = INFD_DIRECTORY_PRIVATE(directory);

  switch(prop_id)
  {
  case PROP_IO:
    g_value_set_object(value, G_OBJECT(priv->io));
    break;
  case PROP_STORAGE:
    g_value_set_object(value, G_OBJECT(priv->storage));
    break;
  case PROP_CONNECTION_MANAGER:
    g_value_set_object(value, G_OBJECT(priv->connection_manager));
    break;
  case PROP_METHOD_MANAGER:
    /* write/only */
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

/*
 * InfNetObject implementation.
 */

static gboolean
infd_directory_net_object_received(InfNetObject* net_object,
                                   InfXmlConnection* connection,
                                   const xmlNodePtr node,
                                   GError** error)
{
  InfdDirectory* directory;
  InfdDirectoryPrivate* priv;
  GError* local_error;
  xmlNodePtr reply_xml;
  xmlChar* seq_attr;
  gchar code_str[16];

  directory = INFD_DIRECTORY(net_object);
  priv = INFD_DIRECTORY_PRIVATE(directory);
  local_error = NULL;

  if(strcmp((const gchar*)node->name, "explore-node") == 0)
  {
    infd_directory_handle_explore_node(
      INFD_DIRECTORY(net_object),
      connection,
      node,
      &local_error
    );
  }
  else if(strcmp((const gchar*)node->name, "add-node") == 0)
  {
    infd_directory_handle_add_node(
      INFD_DIRECTORY(net_object),
      connection,
      node,
      &local_error
    );
  }
  else if(strcmp((const gchar*)node->name, "remove-node") == 0)
  {
    infd_directory_handle_remove_node(
      INFD_DIRECTORY(net_object),
      connection,
      node,
      &local_error
    );
  }
  else if(strcmp((const gchar*)node->name, "subscribe-session") == 0)
  {
    infd_directory_handle_subscribe_session(
      INFD_DIRECTORY(net_object),
      connection,
      node,
      &local_error
    );
  }
  else
  {
    g_set_error(
      &local_error,
      inf_directory_error_quark(),
      INF_DIRECTORY_ERROR_UNEXPECTED_MESSAGE,
      "%s",
      inf_directory_strerror(INF_DIRECTORY_ERROR_UNEXPECTED_MESSAGE)
    );
  }

  if(local_error != NULL)
  {
    sprintf(code_str, "%u", local_error->code);

    /* TODO: If error is not from the InfDirectoryError error domain, the
     * client cannot reconstruct the error because he possibly does not know
     * the error domain (it might even come from a storage plugin). */

    /* An error happened, so tell the client that the request failed and
     * what has gone wrong. */
    reply_xml = xmlNewNode(NULL, (const xmlChar*)"request-failed");
    xmlNewProp(reply_xml, (const xmlChar*)"code", (const xmlChar*)code_str);

    xmlNewProp(
      reply_xml,
      (const xmlChar*)"domain",
      (const xmlChar*)g_quark_to_string(local_error->domain)
    );

    seq_attr = xmlGetProp(node, (const xmlChar*)"seq");
    if(seq_attr != NULL)
    {
      xmlNewProp(reply_xml, (const xmlChar*)"seq", seq_attr);
      xmlFree(seq_attr);
    }

    inf_connection_manager_group_send_to_connection(
      priv->group,
      connection,
      reply_xml
    );

    g_propagate_error(error, local_error);
  }

  /* Never forward directory messages */
  return FALSE;
}

/*
 * GType registration.
 */
static void
infd_directory_class_init(gpointer g_class,
                          gpointer class_data)
{
  GObjectClass* object_class;
  InfdDirectoryClass* directory_class;

  object_class = G_OBJECT_CLASS(g_class);
  directory_class = INFD_DIRECTORY_CLASS(g_class);

  parent_class = G_OBJECT_CLASS(g_type_class_peek_parent(g_class));
  g_type_class_add_private(g_class, sizeof(InfdDirectoryPrivate));

  object_class->constructor = infd_directory_constructor;
  object_class->dispose = infd_directory_dispose;
  object_class->set_property = infd_directory_set_property;
  object_class->get_property = infd_directory_get_property;

  directory_class->node_added = NULL;
  directory_class->node_removed = NULL;

  g_object_class_install_property(
    object_class,
    PROP_IO,
    g_param_spec_object(
      "io",
      "IO",
      "IO object to watch sockets and schedule timeouts",
      INF_TYPE_IO,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_STORAGE,
    g_param_spec_object(
      "storage",
      "Storage backend",
      "The storage backend to use",
      INFD_TYPE_STORAGE,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_CONNECTION_MANAGER,
    g_param_spec_object(
      "connection-manager",
      "Connection manager",
      "The connection manager for the directory",
      INF_TYPE_CONNECTION_MANAGER,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_METHOD_MANAGER,
    g_param_spec_object(
      "method-manager",
      "Method manager",
      "The method manager to get communication methods from",
      INF_TYPE_METHOD_MANAGER,
      G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  /**
   * InfdDirectory::node-added:
   * @directory: The #InfdDirectory emitting the signal.
   * @iter: A #InfdDirectoryIter pointing to the added node.
   *
   * This signal is emitted when a new node has been created. It can either
   * be created by API calls such as infd_directory_add_note() and
   * infd_directory_add_subdirectory() or by a client making a corresponding
   * request.
   **/
  directory_signals[NODE_ADDED] = g_signal_new(
    "node-added",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfdDirectoryClass, node_added),
    NULL, NULL,
    inf_marshal_VOID__BOXED,
    G_TYPE_NONE,
    1,
    INFD_TYPE_DIRECTORY_ITER | G_SIGNAL_TYPE_STATIC_SCOPE
  );

  /**
   * InfdDirectory::node-removed:
   * @directory: The #InfdDirectory emitting the signal.
   * @iter: A #InfdDirectoryIter pointing to the removed node.
   *
   * This signal is emitted when a node has been removed. If a subdirectory
   * node is removed, all contained nodes are removed as well. Node removal
   * can either happen through a call to infd_directory_remove_node(), or by
   * a client making a corresponding request.
   **/
  directory_signals[NODE_REMOVED] = g_signal_new(
    "node-removed",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfdDirectoryClass, node_removed),
    NULL, NULL,
    inf_marshal_VOID__BOXED,
    G_TYPE_NONE,
    1,
    INFD_TYPE_DIRECTORY_ITER | G_SIGNAL_TYPE_STATIC_SCOPE
  );
}

static void
infd_directory_net_object_init(gpointer g_iface,
                               gpointer iface_data)
{
  InfNetObjectIface* iface;
  iface = (InfNetObjectIface*)g_iface;

  iface->received = infd_directory_net_object_received;
}

GType
infd_directory_iter_get_type(void)
{
  static GType directory_iter_type = 0;

  if(!directory_iter_type)
  {
    directory_iter_type = g_boxed_type_register_static(
      "InfdDirectoryIter",
      (GBoxedCopyFunc)infd_directory_iter_copy,
      (GBoxedFreeFunc)infd_directory_iter_free
    );
  }

  return directory_iter_type;
}

GType
infd_directory_get_type(void)
{
  static GType directory_type = 0;

  if(!directory_type)
  {
    static const GTypeInfo directory_type_info = {
      sizeof(InfdDirectoryClass),  /* class_size */
      NULL,                        /* base_init */
      NULL,                        /* base_finalize */
      infd_directory_class_init,   /* class_init */
      NULL,                        /* class_finalize */
      NULL,                        /* class_data */
      sizeof(InfdDirectory),       /* instance_size */
      0,                           /* n_preallocs */
      infd_directory_init,         /* instance_init */
      NULL                         /* value_table */
    };

    static const GInterfaceInfo net_object_info = {
      infd_directory_net_object_init,
      NULL,
      NULL
    };

    directory_type = g_type_register_static(
      G_TYPE_OBJECT,
      "InfdDirectory",
      &directory_type_info,
      0
    );

    g_type_add_interface_static(
      directory_type,
      INF_TYPE_NET_OBJECT,
      &net_object_info
    );
  }

  return directory_type;
}

/*
 * Public API.
 */

/**
 * infd_directory_iter_copy:
 * @iter: A #InfdDirectoryIter.
 *
 * Makes a dynamically-allocated copy of @iter. This should not be used by
 * applications because you can copy the structs by value.
 *
 * Return Value: A newly-allocated copy of @iter.
 **/
InfdDirectoryIter*
infd_directory_iter_copy(InfdDirectoryIter* iter)
{
  InfdDirectoryIter* new_iter;

  g_return_val_if_fail(iter != NULL, NULL);

  new_iter = g_slice_new(InfdDirectoryIter);
  *new_iter = *iter;

  return new_iter;
}

/**
 * infd_directory_iter_free:
 * @iter: A #InfdDirectoryIter.
 *
 * Frees a #InfdDirectoryIter allocated with infd_directory_iter_copy().
 **/
void
infd_directory_iter_free(InfdDirectoryIter* iter)
{
  g_return_if_fail(iter != NULL);

  g_slice_free(InfdDirectoryIter, iter);
}

/**
 * infd_directory_new:
 * @io: IO object to watch connections and schedule timeouts.
 * @storage: Storage backend that is used to read/write notes from
 * permanent memory into #InfBuffer objects.
 * @connection_manager: A #InfConnectionManager to register added
 * connections to and which forwards incoming data to the directory
 * or running sessions.
 * @method_manager: A #InfMethodManager to load communication methods from,
 * or %NULL to use the default one.
 *
 * Creates a new #InfdDirectory.
 *
 * Return Value: A new #InfdDirectory.
 **/
InfdDirectory*
infd_directory_new(InfIo* io,
                   InfdStorage* storage,
                   InfConnectionManager* connection_manager,
                   InfMethodManager* method_manager)
{
  GObject* object;

  g_return_val_if_fail(INF_IS_IO(io), NULL);
  g_return_val_if_fail(INFD_IS_STORAGE(storage), NULL);
  g_return_val_if_fail(INF_IS_CONNECTION_MANAGER(connection_manager), NULL);
  g_return_val_if_fail(method_manager == NULL ||
                       INF_IS_METHOD_MANAGER(method_manager), NULL);

  object = g_object_new(
    INFD_TYPE_DIRECTORY,
    "io", io,
    "storage", storage,
    "connection-manager", connection_manager,
    "method-manager", method_manager,
    NULL
  );

  return INFD_DIRECTORY(object);
}

/**
 * infd_directory_get_io:
 * @directory: A #InfdDirectory.
 *
 * Returns the IO object in use by the directory.
 *
 * Return Value: A #InfIo.
 **/
InfIo*
infd_directory_get_io(InfdDirectory* directory)
{
  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), NULL);
  return INFD_DIRECTORY_PRIVATE(directory)->io;
}

/**
 * infd_directory_get_storage:
 * @directory: A #InfdDirectory:
 *
 * Returns the storage backend in use by the directory.
 *
 * Return Value: An #InfdDirectoryStorage.
 **/
InfdStorage*
infd_directory_get_storage(InfdDirectory* directory)
{
  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), NULL);
  return INFD_DIRECTORY_PRIVATE(directory)->storage;
}

/**
 * infd_directory_get_connection_manager:
 * @directory: A #InfdDirectory.
 *
 * Returns the connection manager of the directory.
 *
 * Return Value: An #InfConnectionManager.
 **/
InfConnectionManager*
infd_directory_get_connection_manager(InfdDirectory* directory)
{
  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), NULL);
  return INFD_DIRECTORY_PRIVATE(directory)->connection_manager;
}

/**
 * infd_directory_add_plugin:
 * @directory: A #InfdDirectory.
 * @plugin: A #InfdNotePlugin.
 *
 * Adds @plugin to @directory. This allows the directory to create sessions
 * of the plugin's type. Only one plugin of each type can be added to the
 * directory. The plugin's storage_type must match the storage of @directory.
 *
 * Return Value: Whether the plugin was added successfully.
 **/
gboolean
infd_directory_add_plugin(InfdDirectory* directory,
                          const InfdNotePlugin* plugin)
{
  InfdDirectoryPrivate* priv;
  const gchar* storage_type;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(plugin != NULL, FALSE);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  storage_type = g_type_name(G_TYPE_FROM_INSTANCE(priv->storage));
  g_return_val_if_fail(
    strcmp(plugin->storage_type, storage_type) == 0,
    FALSE
  );

  if(g_hash_table_lookup(priv->plugins, plugin->note_type) != NULL)
    return FALSE;

  g_hash_table_insert(
    priv->plugins,
    (gpointer)plugin->note_type,
    (gpointer)plugin
  );

  return TRUE;
}

/**
 * infd_directory_lookup_plugin:
 * @directory: A #InfdDirectory.
 * @note_type: A note type for which to lookup the plugin.
 *
 * Returns the #InfdNotePlugin that handles the given note type, or %NULL
 * in case no corresponding plugin was added.
 *
 * Return Value: A #InfdNotePlugin, or %NULL.
 **/
const InfdNotePlugin*
infd_directory_lookup_plugin(InfdDirectory* directory,
                             const gchar* note_type)
{
  InfdDirectoryPrivate* priv;
  
  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), NULL);
  g_return_val_if_fail(note_type != NULL, NULL);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  return (const InfdNotePlugin*)g_hash_table_lookup(priv->plugins, note_type);
}

/**
 * infd_directory_add_connection:
 * @directory: A #InfdDirectory.
 * @connection: A #InfConnection.
 *
 * Adds @connection to the connections of @directory. The directory will then
 * receive requests from @connection. If the directory's method manager does
 * not contain a "central" method for connection's network, then the
 * connection will not be added and the function returns %FALSE.
 *
 * Returns: Whether the connection was added to the directory.
 **/
gboolean
infd_directory_add_connection(InfdDirectory* directory,
                              InfXmlConnection* connection)
{
  InfdDirectoryPrivate* priv;
  gboolean result;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(INF_IS_XML_CONNECTION(connection), FALSE);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  g_return_val_if_fail(priv->connection_manager != NULL, FALSE);

  result = inf_connection_manager_group_add_connection(
    priv->group,
    connection
  );

  if(result == FALSE)
    return FALSE;

  g_signal_connect(
    G_OBJECT(connection),
    "notify::status",
    G_CALLBACK(infd_directory_connection_notify_status_cb),
    directory
  );

  priv->connections = g_slist_prepend(priv->connections, connection);
  g_object_ref(G_OBJECT(connection));

  return TRUE;
}

/**
 * infd_directory_iter_get_root:
 * @directory: A #InfdDirectory
 * @iter An uninitalized #InfdDirectoryIter.
 *
 * Sets @iter to point to the root node of the directory.
 **/
void
infd_directory_iter_get_root(InfdDirectory* directory,
                             InfdDirectoryIter* iter)
{
  InfdDirectoryPrivate* priv;

  g_return_if_fail(INFD_IS_DIRECTORY(directory));
  g_return_if_fail(iter != NULL);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  g_assert(priv->root != NULL);

  iter->node_id = priv->root->id;
  iter->node = priv->root;
}

/**
 * infd_directory_iter_get_next:
 * @directory: A #InfdDirectory.
 * @iter: A #InfdDirectoryIter pointing to some node in @directory.
 *
 * Sets @iter to point to the next node within the same subdirectory. If there
 * is no next node, @iter is left untouched and the function returns %FALSE.
 *
 * Return Value: %TRUE, if @iter was set. 
 **/
gboolean
infd_directory_iter_get_next(InfdDirectory* directory,
                             InfdDirectoryIter* iter)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(iter != NULL, FALSE);
  infd_directory_return_val_if_iter_fail(directory, iter, FALSE);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  node = (InfdDirectoryNode*)iter->node;

  if(node->next != NULL)
  {
    iter->node_id = node->next->id;
    iter->node = node->next;

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

/**
 * infd_directory_iter_get_prev:
 * @directory: A #InfdDirectory.
 * @iter: A #InfdDirectoryIter pointing to some node in @directory.
 *
 * Sets @iter to point to the previous node within the same subdirectory. If
 * there is no such node, @iter is left untouched and the function returns
 * %FALSE.
 *
 * Return Value: %TRUE, if @iter was set. 
 **/
gboolean
infd_directory_iter_get_prev(InfdDirectory* directory,
                             InfdDirectoryIter* iter)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(iter != NULL, FALSE);
  infd_directory_return_val_if_iter_fail(directory, iter, FALSE);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  node = (InfdDirectoryNode*)iter->node;

  if(node->prev != NULL)
  {
    iter->node_id = node->prev->id;
    iter->node = node->prev;

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

/**
 * infd_directory_iter_get_parent:
 * @directory: A #InfdDirectory.
 * @iter: A #InfdDirectoryIter pointing to some node in @directory.
 *
 * Sets @iter to point to the parent node of @iter. This node is guaranteed
 * to be a subdirectory node. If there is no such node (i.e. @iter points
 * to the root node), @iter is left untouched and the function returns %FALSE.
 *
 * Return Value: %TRUE, if @iter was set. 
 **/
gboolean
infd_directory_iter_get_parent(InfdDirectory* directory,
                               InfdDirectoryIter* iter)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(iter != NULL, FALSE);
  infd_directory_return_val_if_iter_fail(directory, iter, FALSE);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  node = (InfdDirectoryNode*)iter->node;

  if(node->parent != NULL)
  {
    iter->node_id = node->parent->id;
    iter->node = node->parent;

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

/**
 * infd_directory_iter_get_child:
 * @directory: A #InfdDirectory.
 * @iter: A #InfdDirectoryIter pointing to a subdirectory node in @directory.
 * @error: Location to store error information.
 *
 * Sets @iter to point to first child node of @iter. This requires that @iter
 * points to a subdirectory node. If the subdirectory @iter points to has
 * no children, the function returns %FALSE and @iter is left untouched.
 *
 * The function might fail if this node's children have not yet been read
 * from the background storage and an error occurs while reading them. In
 * this case, %FALSE is returned and @error is set.
 *
 * Return Value: %TRUE, if @iter was set. 
 **/
gboolean
infd_directory_iter_get_child(InfdDirectory* directory,
                              InfdDirectoryIter* iter,
                              GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(iter != NULL, FALSE);
  infd_directory_return_val_if_iter_fail(directory, iter, FALSE);
  infd_directory_return_val_if_subdir_fail(iter->node, FALSE);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  node = (InfdDirectoryNode*)iter->node;

  if(node->shared.subdir.explored == FALSE)
  {
    if(infd_directory_node_explore(directory, node, error) == FALSE)
      return FALSE;

    g_assert(node->shared.subdir.explored == TRUE);
  }

  if(node->shared.subdir.child != NULL)
  {
    iter->node_id = node->shared.subdir.child->id;
    iter->node = node->shared.subdir.child;

    return TRUE;
  }
  else
  {
    return FALSE;
  }
}

/**
 * infd_directory_add_subdirectory:
 * @directory: A #InfdDirectory.
 * @parent: A #InfdDirectoryIter pointing to a subdirectory node
 * in @directory.
 * @name: The name of the new node.
 * @iter: An uninitalized #InfdDirectoryIter.
 * @error: Location to store error information.
 *
 * Adds a subdirectory to the directory tree. The new subdirectory will be
 * a child the subdirectory @parent points to. @iter is modified to point to
 * the new subdirectory. If creation fails, the function returns FALSE and
 * @error is set.
 *
 * Return Value: %TRUE if the subdirectory was created successfully.
 **/
gboolean
infd_directory_add_subdirectory(InfdDirectory* directory,
                                InfdDirectoryIter* parent,
                                const gchar* name,
                                InfdDirectoryIter* iter,
                                GError** error)
{
  InfdDirectoryPrivate* priv;
  InfdDirectoryNode* node;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(parent != NULL, FALSE);
  g_return_val_if_fail(name != NULL, FALSE);
  infd_directory_return_val_if_iter_fail(directory, parent, FALSE);
  infd_directory_return_val_if_subdir_fail(parent->node, FALSE);

  priv = INFD_DIRECTORY_PRIVATE(directory);
  g_return_val_if_fail(priv->storage != NULL, FALSE);

  if( ((InfdDirectoryNode*)parent->node)->shared.subdir.explored == FALSE)
    if(infd_directory_node_explore(directory, parent->node, error) == FALSE)
      return FALSE;

  node = infd_directory_node_add_subdirectory(
    directory,
    parent->node,
    name,
    NULL,
    0,
    error
  );

  if(node == NULL)
    return FALSE;

  if(iter != NULL)
  {
    iter->node_id = node->id;
    iter->node = node;
  }

  return TRUE;
}

/**
 * infd_directory_add_note:
 * @directory: A #InfdDirectory.
 * @parent: A #InfdDirectoryIter pointing to a subdirectory node
 * in @directory.
 * @name: The name of the new node.
 * @plugin: The plugin to use for the node. Must have been added with
 * infd_directory_add_plugin().
 * @iter: An uninitialized #InfdDirectoryIter.
 * @error: Location to store error information.
 *
 * Creates a new text note in @directory. It will be child of the subdirectory
 * node @parent points to. @iter is set to point to the new node. If an
 * error occurs, the function returns %FALSE and @error is set.
 *
 * Return Value: %TRUE on success.
 **/
gboolean
infd_directory_add_note(InfdDirectory* directory,
                        InfdDirectoryIter* parent,
                        const gchar* name,
                        InfdNotePlugin* plugin,
                        InfdDirectoryIter* iter,
                        GError** error)
{
  InfdDirectoryNode* node;
  
  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(parent != NULL, FALSE);
  g_return_val_if_fail(name != NULL, FALSE);
  g_return_val_if_fail(plugin != NULL, FALSE);
  infd_directory_return_val_if_iter_fail(directory, parent, FALSE);
  infd_directory_return_val_if_subdir_fail(parent->node, FALSE);

  if( ((InfdDirectoryNode*)parent->node)->shared.subdir.explored == FALSE)
    if(infd_directory_node_explore(directory, parent->node, error) == FALSE)
      return FALSE;

  node = infd_directory_node_add_note(
    directory,
    parent->node,
    name,
    plugin,
    NULL,
    0,
    error
  );

  if(node == NULL)
    return FALSE;

  if(iter != NULL)
  {
    iter->node = node;
    iter->node_id = node->id;
  }

  return TRUE;
}

/**
 * infd_directory_remove_node:
 * @directory: A #InfdDirectory
 * @iter: A #InfdDirectoryIter pointing to some node in @directory.
 * @error: Location to store error information.
 *
 * Removes the node @iter points to. If it is a subdirectory node, every
 * node it contains will also be removed. If the function fails, %FALSE is
 * returned and @error is set.
 *
 * Return Value: %TRUE on success.
 **/
gboolean
infd_directory_remove_node(InfdDirectory* directory,
                           InfdDirectoryIter* iter,
                           GError** error)
{
  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), FALSE);
  g_return_val_if_fail(iter != NULL, FALSE);
  infd_directory_return_val_if_iter_fail(directory, iter, FALSE);

  return infd_directory_node_remove(directory, iter->node, NULL, 0, error);
}

/**
 * infd_directory_iter_get_node_type:
 * @directory: A #InfdDirectory.
 * @iter: A #InfdDirectoryIter pointing to some node in @directory.
 *
 * Returns the type of the node @iter points to.
 *
 * Returns: A #InfdDirectoryStorageNodeType.
 **/
InfdStorageNodeType
infd_directory_iter_get_node_type(InfdDirectory* directory,
                                  InfdDirectoryIter* iter)
{
  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), INFD_STORAGE_NODE_NOTE);

  infd_directory_return_val_if_iter_fail(
    directory,
    iter,
    INFD_STORAGE_NODE_NOTE
  );

  return ((InfdDirectoryNode*)iter->node)->type;
}

/**
 * infd_directory_iter_get_plugin:
 * @directory: A #InfdDirectory.
 * @iter: a #InfdDirectoryIter pointing to a note in @directory.
 *
 * Returns the plugin that is used to create a session for the note @iter
 * points to.
 *
 * Return Value: The plugin for the note @iter points to.
 **/
InfdNotePlugin*
infd_directory_iter_get_plugin(InfdDirectory* directory,
                               InfdDirectoryIter* iter)
{
  InfdDirectoryNode* node;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), NULL);
  infd_directory_return_val_if_iter_fail(directory, iter, NULL);

  node = (InfdDirectoryNode*)iter->node;
  g_return_val_if_fail(node->type != INFD_STORAGE_NODE_NOTE, NULL);

  return node->shared.note.plugin;
}

/**
 * infd_directory_iter_get_session:
 * @directory: A #InfdDirectory.
 * @iter: A #InfdDirectoryIter pointing to a note in @directory.
 * @error: Location to store error information.
 *
 * Returns the running session in which the note @iter points to is currently
 * edited. If the session does not exist, it is created. However, this might
 * fail if the loading from the background storage fails. In this case, %NULL
 * is returned and @error is set.
 *
 * Return Value: A #InfdSessionProxy for the note @iter points to.
 **/
InfdSessionProxy*
infd_directory_iter_get_session(InfdDirectory* directory,
                                InfdDirectoryIter* iter,
                                GError** error)
{
  InfdDirectoryNode* node;

  g_return_val_if_fail(INFD_IS_DIRECTORY(directory), NULL);
  infd_directory_return_val_if_iter_fail(directory, iter, NULL);

  node = (InfdDirectoryNode*)iter->node;
  g_return_val_if_fail(node->type != INFD_STORAGE_NODE_NOTE, NULL);

  return infd_directory_node_get_session(directory, node, error);
}

/* vim:set et sw=2 ts=2: */