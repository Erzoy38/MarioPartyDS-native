// lobby-client: goes through the lobby of MarioPartyDS-server from the command line, without a game.
// For tests of the server and by hand.
//
// Usage: lobby-client --lobby ADDRESS[:PORT] --name NAME --character NAME
//                     (--create CODEFILE | --join CODE|@CODEFILE)
//                     [--start-players N] [--known-servers FILE] [--lobby-exit 1]
// --create makes a room and writes its code to CODEFILE ("-": nowhere); the host starts once N
// players (default 4) are in and ready. --join @FILE waits for FILE and joins with its code; guests
// are ready at once. After the start the program prints the start and ends (--lobby-exit is
// accepted for the same command line as the game's test program).
// Exit code 0 after the start, 2 for bad options, 3 if refused or failed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "lobby.h"

#include <winsock2.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

struct Options {
    std::string server;
    std::string name = "Player";
    std::string create;  // code file, when this one makes the room
    std::string join;    // CODE or @FILE
    int character = 0;
    int start_players = 4;
    std::string known_servers;
};

int character_by_name(const char* name) {
    for (int i = 0; i < lobby::kCharacters; ++i) {
        if (!_stricmp(name, lobby::character_name(i))) return i;
    }
    std::fprintf(stderr, "unknown character %s\n", name);
    std::exit(2);
}

int run(const Options& options) {
    std::string address = options.server;
    std::uint16_t port = lobby::kDefaultPort;
    const size_t colon = address.rfind(':');
    if (colon != std::string::npos) {
        port = std::uint16_t(std::atoi(address.c_str() + colon + 1));
        address.resize(colon);
    }
    std::string code = options.join;
    if (!code.empty() && code[0] == '@') {
        const std::string path = code.substr(1);
        for (int waited = 0; !std::filesystem::exists(path); waited += 50) {
            if (waited > 120000) {
                std::printf("lobby: no code file %s\n", path.c_str());
                return 3;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::ifstream file(path);
        std::getline(file, code);
    }
    LobbyClient client;
    client.set_known_servers(options.known_servers);
    client.connect(address, port, options.name);
    bool asked = false, ready_sent = false, start_sent = false, code_written = false;
    std::uint64_t seen_updates = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (;;) {
        client.poll();
        const LobbyClient::Phase phase = client.phase();
        if (phase == LobbyClient::Phase::Failed) return 3;
        if (phase == LobbyClient::Phase::Menu) {
            if (asked && client.last_error() != 0) {
                std::printf("lobby: refused: %s (error %d)\n", client.message().c_str(), client.last_error());
                return 3;
            }
            if (!asked) {
                if (!options.create.empty()) client.create(options.character);
                else client.join(code, options.character);
                asked = true;
            }
        }
        if (phase == LobbyClient::Phase::InRoom) {
            if (client.room_updates() != seen_updates) {
                seen_updates = client.room_updates();
                std::printf("lobby: room %s, own slot %d:", client.code().c_str(), client.own_slot());
                int present = 0, ready = 0;
                for (const LobbySeat& seat : client.seats()) {
                    if (!seat.present) continue;
                    ++present;
                    ready += seat.ready;
                    std::printf(" [%s %s%s %dms]", seat.name.c_str(), lobby::character_name(seat.character),
                                seat.ready ? " ready" : "", seat.ping_ms);
                }
                std::printf("\n");
                if (!options.create.empty() && present >= options.start_players && ready == present && !start_sent) {
                    client.start();
                    start_sent = true;
                }
            }
            if (!options.create.empty() && !code_written) {
                code_written = true;
                if (options.create != "-") {
                    const std::string temporary = options.create + ".new";
                    { std::ofstream(temporary) << client.code() << "\n"; }
                    std::filesystem::rename(temporary, options.create);
                }
            }
            if (options.create.empty() && !ready_sent) {
                client.set_ready(true);
                ready_sent = true;
            }
            if (start_sent && client.last_error() != 0) {
                std::printf("lobby: start refused: %s\n", client.message().c_str());
                return 3;
            }
        }
        if (phase == LobbyClient::Phase::Started) break;
        if (std::chrono::steady_clock::now() - begin > std::chrono::seconds(180)) {
            std::printf("lobby: no start within 180 s\n");
            return 3;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LobbyStart start = client.take_start();
    std::printf("lobby: player %d of %d, %s, input delay %d\n", start.slot + 1, start.players,
                lobby::character_name(start.characters[size_t(start.slot)]), start.input_delay);
    closesocket(static_cast<SOCKET>(start.socket));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--lobby")) options.server = argv[i + 1];
        else if (!std::strcmp(argv[i], "--name")) options.name = argv[i + 1];
        else if (!std::strcmp(argv[i], "--create")) options.create = argv[i + 1];
        else if (!std::strcmp(argv[i], "--join")) options.join = argv[i + 1];
        else if (!std::strcmp(argv[i], "--character")) options.character = character_by_name(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--start-players")) options.start_players = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--known-servers")) options.known_servers = argv[i + 1];
        else if (!std::strcmp(argv[i], "--lobby-exit")) continue;
        else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (options.server.empty() || (options.create.empty() == options.join.empty())) {
        std::fprintf(stderr, "usage: lobby-client --lobby ADDRESS[:PORT] --name NAME --character NAME "
                             "(--create CODEFILE | --join CODE|@CODEFILE) [--start-players N] "
                             "[--known-servers FILE]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    return run(options);
}
