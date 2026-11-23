#include "OverlayApp.hpp"
#include <csignal>

namespace {
void HandleTerminationSignal(int /*sig*/) {
    OverlayApp::RequestStop();
}
} // namespace

int main() {
    std::signal(SIGINT, HandleTerminationSignal);
    std::signal(SIGTERM, HandleTerminationSignal);

    OverlayApp app;
    if (!app.Initialize()) {
        return -1;
    }
    return app.Run();
}
