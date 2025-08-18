#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>

#include "httplib.h"
#include "Client.h"
#include "Message.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shell32.lib")

using namespace std;

string username;
vector<Client> clients;
vector<Message> messages;

// ---------- helpers ----------
static bool read_text_file(const string& path, string& out) {
    ifstream f(path, ios::binary);
    if (!f) return false;
    out.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
    return true;
}
static void replace_all(string& s, const string& from, const string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
}
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

// ---- Presence via Client::isOnline ----
static string get_client_name(const Client& c) { return c.getName(); }

static Client& ensure_client(const string& uname) {
    auto it = find_if(clients.begin(), clients.end(),
        [&](const Client& c) { return get_client_name(c) == uname; });
    if (it == clients.end()) {
        clients.emplace_back(uname);
        clients.back().setIsOnline(false); // başlangıçta offline
        return clients.back();
    }
    return *it;
}

// clients -> JSON (kilit içeride)
static mutex g_m;
static condition_variable g_cv;
static string users_json_from_clients_locked() {
    string out = "[";
    bool first = true;
    for (auto& c : clients) {
        if (!first) out += ",";
        first = false;
        out += "{\"user\":\"" + json_escape(get_client_name(c)) +
            "\",\"online\":" + (c.getIsOnline() ? string("true") : string("false")) + "}";
    }
    out += "]";
    return out;
}

// Tek SSE kuyruğu: hem "msg" hem "users" eventlerini tutar
static vector<string> g_events; // her eleman: "event: X\ndata: {...}\n\n"

static void push_sse(const string& event_name, const string& json_payload) {
    lock_guard<mutex> lk(g_m);
    g_events.push_back("event: " + event_name + "\n" "data: " + json_payload + "\n\n");
    g_cv.notify_all();
}
// --------------------------------------

int main() {
    httplib::Server server;
    server.set_mount_point("/static", "wwwroot/assets");

    // GET / -> login
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        string html;
        if (!read_text_file("wwwroot/login.html", html)) {
            res.status = 500; res.set_content("login.html not found", "text/plain; charset=UTF-8"); return;
        }
        res.set_content(html, "text/html; charset=UTF-8");
        });

    // POST /login -> username ata, online yap, users yayınla, /home
    server.Post("/login", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("username")) { res.status = 400; res.set_content("Username is required", "text/plain"); return; }
        username = req.get_param_value("username");

        bool changed = false;
        {
            lock_guard<mutex> lk(g_m);
            Client& me = ensure_client(username);
            bool was = me.getIsOnline();
            me.setIsOnline(true);
            changed = !was;
        }
        if (changed) {
            // listeyi kilitleyerek üret, sonra yayınla
            string json;
            { lock_guard<mutex> lk(g_m); json = users_json_from_clients_locked(); }
            push_sse("users", json);
        }

        res.set_redirect("/home");
        });

    // GET /home -> home.html + {{username}}
    server.Get("/home", [](const httplib::Request&, httplib::Response& res) {
        if (username.empty()) { res.set_redirect("/"); return; }
        string html;
        if (!read_text_file("wwwroot/home.html", html)) {
            res.status = 500; res.set_content("home.html not found", "text/plain; charset=UTF-8"); return;
        }
        replace_all(html, "{{username}}", username);
        res.set_content(html, "text/html; charset=UTF-8");
        });

    // GET /users -> mevcut kullanıcılar (online/offline)
    server.Get("/users", [](const httplib::Request&, httplib::Response& res) {
        string json;
        { lock_guard<mutex> lk(g_m); json = users_json_from_clients_locked(); }
        res.set_content(json, "application/json; charset=UTF-8");
        });

    // SSE /events -> users ve msg yayınları (2 sn heartbeat)
    server.Get("/events", [](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");

        const string who = req.has_param("user") ? req.get_param_value("user") : "";

        // Bağlantı açılırken online yap ve listeyi yayınla (değiştiyse)
        if (!who.empty()) {
            bool changed = false;
            {
                lock_guard<mutex> lk(g_m);
                Client& c = ensure_client(who);
                bool was = c.getIsOnline();
                c.setIsOnline(true);
                changed = !was;
            }
            if (changed) {
                string json;
                { lock_guard<mutex> lk(g_m); json = users_json_from_clients_locked(); }
                push_sse("users", json);
            }
        }

        res.set_chunked_content_provider(
            "text/event-stream",
            // provider
            [last = size_t{ 0 }](size_t /*offset*/, httplib::DataSink& sink) mutable {
                using namespace std::chrono;
                vector<string> batch;
                {
                    unique_lock<mutex> lk(g_m);
                    if (last >= g_events.size()) g_cv.wait_for(lk, seconds(2)); // <- 2sn
                    while (last < g_events.size()) batch.push_back(g_events[last++]);
                }
                for (const auto& ev : batch) {
                    if (!sink.write(ev.data(), ev.size())) return false;
                }
                static const char ping[] = ": keep-alive\n\n";
                if (!sink.write(ping, sizeof(ping) - 1)) return false;
                return true;
            },
            // releaser: bağlantı kapanınca offline yap ve listeyi yayınla
            [who](bool /*success*/) {
                if (who.empty()) return;
                bool changed = false;
                {
                    lock_guard<mutex> lk(g_m);
                    Client& c = ensure_client(who);
                    bool was = c.getIsOnline();
                    c.setIsOnline(false);
                    changed = was;
                }
                if (changed) {
                    string json;
                    { lock_guard<mutex> lk(g_m); json = users_json_from_clients_locked(); }
                    push_sse("users", json);
                }
            }
        );
        });

    // POST /send -> mesajı yayınla (msg)
    server.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("user") || !req.has_param("text")) {
            res.status = 400; res.set_content("Missing user/text", "text/plain"); return;
        }
        const string user = req.get_param_value("user");
        const string text = req.get_param_value("text");
        const string json = string("{\"user\":\"") + json_escape(user) + "\",\"text\":\"" + json_escape(text) + "\"}";
        push_sse("msg", json);
        res.set_content("OK", "text/plain");
        });

    // POST /leave -> sekme kapanmadan anında offline bildirimi
    server.Post("/leave", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("user")) { res.status = 400; res.set_content("Missing user", "text/plain"); return; }
        const string who = req.get_param_value("user");

        bool changed = false;
        {
            lock_guard<mutex> lk(g_m);
            Client& c = ensure_client(who);
            bool was = c.getIsOnline();
            c.setIsOnline(false);
            changed = was;
        }
        if (changed) {
            string json;
            { lock_guard<mutex> lk(g_m); json = users_json_from_clients_locked(); }
            push_sse("users", json);
        }
        res.set_content("OK", "text/plain");
        });

    // Tarayıcıyı ana sayfaya aç & dinle
    ShellExecuteA(NULL, "open", "http://127.0.0.1:8080/home", NULL, NULL, SW_SHOWNORMAL);
    cout << "HTTP server: http://127.0.0.1:8080/ (Ctrl+C to stop)\n";
    server.listen("0.0.0.0", 8080);
    return 0;
}
