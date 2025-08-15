#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>

#include "httplib.h"
#include "Client.h"
#include "Message.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shell32.lib")

using namespace std;

string username;
vector<Client> clients;
vector<Message> messages;

//read file from the path and return its content as a string
static bool read_text_file(const string& path, string& out) {
    ifstream f(path, ios::binary);
    if (!f) return false;
    out.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
    return true;
}
// replace all occurrences of 'from' with 'to' in string 's'
static void replace_all(string& s, const string& from, const string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
}
// JSON kaçışı (çok basit)
static string json_escape(const string& in) {
    string o; o.reserve(in.size());
    for (unsigned char c : in) {
        switch (c) {
        case '\"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) { char buf[7]; snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; }
            else o += c;
        }
    }
    return o;
}

// --- SSE broadcast durumu ---
static mutex g_m;
static condition_variable g_cv;
static vector<string> g_backlog; // JSON mesajları ({"user":"..","text":".."})

int main() {
    static httplib::Server server;

    server.set_mount_point("/static", "wwwroot/assets");

    // Login page
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        string html;
        if (!read_text_file("wwwroot/login.html", html)) {
            res.status = 500; res.set_content("login.html not found", "text/plain; charset=UTF-8"); return;
        }
        res.set_content(html, "text/html; charset=UTF-8");
        });

    // Login post -> /home
    server.Post("/login", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("username")) { res.status = 400; res.set_content("Username is required", "text/plain"); return; }
        username = req.get_param_value("username");
        // örnek: Client newClient(username); clients.push_back(newClient);
        res.set_redirect("/home");
        });

    // HOME page
    server.Get("/home", [](const httplib::Request&, httplib::Response& res) {
        if (username.empty()) { res.set_redirect("/"); return; }
        string html;
        if (!read_text_file("wwwroot/home.html", html)) {
            res.status = 500; res.set_content("home.html not found", "text/plain; charset=UTF-8"); return;
        }
        replace_all(html, "{{username}}", username);
        res.set_content(html, "text/html; charset=UTF-8");
        });

    // SSE: /events — tüm yeni mesajları yayınlar
    server.Get("/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");

        res.set_chunked_content_provider(
            "text/event-stream",
            // ContentProviderWithoutLength
            [last = size_t{ 0 }](size_t /*offset*/, httplib::DataSink& sink) mutable {
                using namespace chrono;

                // 1) Yeni mesajları bekle/kopyala (kilit kısa süre tutulur)
                vector<string> batch;
                {
                    unique_lock<mutex> lk(g_m);
                    if (last >= g_backlog.size()) {
                        g_cv.wait_for(lk, seconds(15)); // heartbeat süresi
                    }
                    while (last < g_backlog.size()) {
                        batch.push_back(g_backlog[last++]);
                    }
                }

                // 2) Kopyaladıklarımızı gönder
                for (const auto& j : batch) {
                    const string ev = string("event: msg\n") +
                        "data: " + j + "\n\n";
                    if (!sink.write(ev.data(), ev.size())) return false;
                }

                // 3) Heartbeat (SSE yorum satırı)
                static const char ping[] = ": keep-alive\n\n";
                if (!sink.write(ping, sizeof(ping) - 1)) return false;

                return true; // bağlantı açık kalsın, provider tekrar çağrılsın
            },
            // ContentProviderResourceReleaser
            nullptr // <-- burada
        );
        });



    // Mesaj gönder: /send (POST user, text) -> yayın (SSE)
    server.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("user") || !req.has_param("text")) {
            res.status = 400; res.set_content("Missing user/text", "text/plain"); return;
        }
        const string user = req.get_param_value("user");
        const string text = req.get_param_value("text");

        const string json = string("{\"user\":\"") + json_escape(user) + "\",\"text\":\"" + json_escape(text) + "\"}";

        {
            lock_guard<mutex> lk(g_m);
            g_backlog.push_back(json);
        }
        g_cv.notify_all();
        res.set_content("OK", "text/plain");
        });

    // Tarayıcıyı aç & başlat
    ShellExecuteA(NULL, "open", "http://127.0.0.1:8080/home", NULL, NULL, SW_SHOWNORMAL);
    cout << "HTTP server: http://127.0.0.1:8080/ (Ctrl+C to stop)\n";
    server.listen("0.0.0.0", 8080);
    return 0;
}
