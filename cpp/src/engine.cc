#ifdef HAVE_FCITX5

#include "engine.h"
#include "daemon_client.h" // call query_daemon() from daemon_client.cc when needed

using namespace fcitx;

void GoogleIMEEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) {
    // Minimal runtime diagnostics and prototype candidate fetch.
    // Keep the method non-intrusive: log the call and query the local daemon
    // for quick feedback. This is a synchronous prototype and will be
    // replaced by an asynchronous implementation later.
    (void)entry;
    (void)keyEvent;

    try {
        std::cerr << "GoogleIMEEngine: keyEvent invoked\n";
        // Query the local daemon with a short test string. In practice this
        // should use the current composition buffer; we're using a short
        // placeholder so the call is cheap and visible in logs for testing.
        auto candidates = query_daemon("test", "zh-t-i0-pinyin", 5);
        std::cerr << "GoogleIMEEngine: daemon returned " << candidates.size() << " candidates\n";
        for (size_t i = 0; i < candidates.size(); ++i) {
            std::cerr << "  cand[" << i << "]=" << candidates[i] << "\n";
        }
    } catch (const std::exception &e) {
        std::cerr << "GoogleIMEEngine: exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "GoogleIMEEngine: unknown exception from daemon query\n";
    }
}

// Register addon factory using the macro that takes a single identifier
FCITX_ADDON_FACTORY(GoogleIMEFactory)

#endif // HAVE_FCITX5
