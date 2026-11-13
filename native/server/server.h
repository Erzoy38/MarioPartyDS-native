// MarioPartyDS-server as a function: the program's main on Windows, a thread inside the app on
// Android and iOS (src/launcher.cpp, "Server hier starten").
#pragma once

#include <atomic>

// Runs the server with the program's options (see server.cpp) until `stop` becomes true; null runs
// it for good. Returns 0 after a stop, 1 if it could not start (key, address or port).
int server_main(int argc, char** argv, const std::atomic<bool>* stop);
