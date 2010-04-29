
#ifndef __DEBUG_H__
#define __DEBUG_H__

#include "config.h"

#include <glib.h>

#ifdef ENABLE_DEBUG

G_BEGIN_DECLS

typedef enum
{
  GABBLE_DEBUG_PRESENCE      = 1 << 0,
  GABBLE_DEBUG_GROUPS        = 1 << 1,
  GABBLE_DEBUG_ROSTER        = 1 << 2,
  GABBLE_DEBUG_DISCO         = 1 << 3,
  GABBLE_DEBUG_PROPERTIES    = 1 << 4,
  GABBLE_DEBUG_ROOMLIST      = 1 << 5,
  GABBLE_DEBUG_MEDIA         = 1 << 6,
  GABBLE_DEBUG_MUC           = 1 << 7,
  GABBLE_DEBUG_CONNECTION    = 1 << 8,
  GABBLE_DEBUG_IM            = 1 << 9,
  GABBLE_DEBUG_TUBES         = 1 << 10,
  GABBLE_DEBUG_VCARD         = 1 << 11,
  GABBLE_DEBUG_PIPELINE      = 1 << 12,
  GABBLE_DEBUG_JID           = 1 << 13,
  GABBLE_DEBUG_OLPC          = 1 << 14,
  GABBLE_DEBUG_BYTESTREAM    = 1 << 16,
  GABBLE_DEBUG_LOCATION      = 1 << 17,
  GABBLE_DEBUG_FT            = 1 << 18,
  GABBLE_DEBUG_SEARCH        = 1 << 19,
  GABBLE_DEBUG_BASE_CHANNEL  = 1 << 20,
  GABBLE_DEBUG_PLUGINS       = 1 << 21,
  GABBLE_DEBUG_MAIL_NOTIF    = 1 << 22
} GabbleDebugFlags;

void gabble_debug_set_flags_from_env (void);
void gabble_debug_set_flags (GabbleDebugFlags flags);
gboolean gabble_debug_flag_is_set (GabbleDebugFlags flag);
void gabble_debug_free (void);
void gabble_debug (GLogLevelFlags level, GabbleDebugFlags flag,
    const gchar *format, ...) G_GNUC_PRINTF (3, 4);

G_END_DECLS

#ifdef DEBUG_FLAG

#define ERROR(format, ...) \
  gabble_debug (G_LOG_LEVEL_ERROR, DEBUG_FLAG, "%s: " format, \
      G_STRFUNC, ##__VA_ARGS__)
#define CRITICAL(format, ...) \
  gabble_debug (G_LOG_LEVEL_CRITICAL, DEBUG_FLAG, "%s: " format, \
      G_STRFUNC, ##__VA_ARGS__)
#define WARNING(format, ...) \
  gabble_debug (G_LOG_LEVEL_WARNING, DEBUG_FLAG, "%s: " format, \
      G_STRFUNC, ##__VA_ARGS__)
#define MESSAGE(format, ...) \
  gabble_debug (G_LOG_LEVEL_MESSAGE, DEBUG_FLAG, "%s: " format, \
      G_STRFUNC, ##__VA_ARGS__)
#define INFO(format, ...) \
  gabble_debug (G_LOG_LEVEL_INFO, DEBUG_FLAG, "%s: " format, \
      G_STRFUNC, ##__VA_ARGS__)
#define DEBUG(format, ...) \
  gabble_debug (G_LOG_LEVEL_DEBUG, DEBUG_FLAG, "%s: " format, \
      G_STRFUNC, ##__VA_ARGS__)

#define DEBUGGING gabble_debug_flag_is_set (DEBUG_FLAG)

#define STANZA_DEBUG(st, s) \
  NODE_DEBUG (wocky_stanza_get_top_node (st), s)

#define NODE_DEBUG(n, s) \
G_STMT_START { \
  gchar *debug_tmp = lm_message_node_to_string (n); \
  gabble_debug (G_LOG_LEVEL_DEBUG, DEBUG_FLAG, "%s: %s:\n%s", G_STRFUNC, s, debug_tmp); \
  g_free (debug_tmp); \
} G_STMT_END

#endif /* DEBUG_FLAG */

#else /* ENABLE_DEBUG */

#ifdef DEBUG_FLAG

#define DEBUG(format, ...) G_STMT_START { } G_STMT_END

#define DEBUGGING 0

#define STANZA_DEBUG(st, s) G_STMT_START { } G_STMT_END

#define NODE_DEBUG(n, s) G_STMT_START { } G_STMT_END

#endif /* DEBUG_FLAG */

#define gabble_debug_free() G_STMT_START { } G_STMT_END

#endif /* ENABLE_DEBUG */

#endif /* __DEBUG_H__ */

