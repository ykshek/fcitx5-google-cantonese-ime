#ifdef HAVE_FCITX5

#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed

using namespace fcitx;

void GoogleIMEEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) {
    // No-op prototype: accept the event but do not consume it. This keeps the
    // engine concrete and loadable by fcitx5. Replace with full logic later.
    (void)entry;
    (void)keyEvent;
}

// Register addon factory using the macro that takes a single identifier
FCITX_ADDON_FACTORY(GoogleIMEFactory)

#endif // HAVE_FCITX5
