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
        std::cerr << "GoogleIMEEngine: keyEvent invoked (scheduling async query)\n";
        // Launch an async worker to query the daemon so we don't block input.
        // This prototype uses a detached thread; later this should integrate
        // the fcitx5 EventLoop or a managed worker to safely post results
        // back to the main thread.
        std::thread worker([](){
            try {
                auto candidates = query_daemon("test", "zh-t-i0-pinyin", 8);
                std::cerr << "GoogleIMEEngine(async): daemon returned " << candidates.size() << " candidates\n";
                for (size_t i = 0; i < candidates.size(); ++i) {
                    std::cerr << "  async cand[" << i << "]=" << candidates[i] << "\n";
                }
            } catch (const std::exception &e) {
                std::cerr << "GoogleIMEEngine(async): exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "GoogleIMEEngine(async): unknown exception\n";
            }
        });
        worker.detach();
    } catch (const std::exception &e) {
        std::cerr << "GoogleIMEEngine: exception: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "GoogleIMEEngine: unknown exception scheduling async\n";
    }
}

// Register addon factory using the macro that takes a single identifier
FCITX_ADDON_FACTORY(GoogleIMEFactory)

#endif // HAVE_FCITX5
