/* checkpoints_nofs.c - full-CLI checkpoint backend when persistence is cut.
 *
 * The portable memory snapshot codec remains in core.  These stubs preserve
 * the host-file API/ABI but never inspect or modify a caller-supplied path.
 * Explicit file operations fail closed; REPL/gateway use volatile sessions.
 */
#include "motiris.h"

#include <stddef.h>

#define UNAVAILABLE "snapshot persistence unavailable"

int motiris_session_save(const char *path, MotirisAgent *agent,
                         const char *session_id, const char **error) {
  (void)path;
  (void)agent;
  (void)session_id;
  if (error) *error = UNAVAILABLE;
  return -1;
}

int motiris_session_checkpoint(const char *sub, const char *id,
                               MotirisAgent *agent, const char *session_id,
                               const char **error) {
  (void)sub;
  (void)id;
  (void)agent;
  (void)session_id;
  if (error) *error = UNAVAILABLE;
  return -1;
}

int motiris_session_load(MotirisAgent *agent, const char *path,
                         const char *expected_session_id,
                         size_t *loaded_messages, const char **error) {
  (void)agent;
  (void)path;
  (void)expected_session_id;
  if (loaded_messages) *loaded_messages = 0;
  if (error) *error = UNAVAILABLE;
  return -1;
}
