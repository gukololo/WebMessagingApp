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

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shell32.lib")

using namespace std;
using namespace chrono;

string username;
static vector<Client> clients;
static mutex g_m;
static condition_variable g_cv;

//helper method to read a text file into a string
static bool read_text_file(const string& path, string& out) {
    ifstream f(path, ios::binary);
    if (!f) return false;
    out.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
    return true;
}

//helper methods for string manipulation
static void replace_all(string& s, const string& from, const string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
}

//helper method to escape JSON strings
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

static string trim(const string& s) {
    size_t i = 0, j = s.size();
    while (i < j && isspace((unsigned char)s[i])) ++i;
    while (j > i && isspace((unsigned char)s[j - 1])) --j;
    return s.substr(i, j - i);
}
static string json_array_of_strings(const vector<string>& arr) {
    string out = "[";
    bool first = true;
    for (const auto& v : arr) {
        if (!first) out += ",";
        first = false;
        out += "\"" + json_escape(v) + "\"";
    }
    out += "]";
    return out;
}

/*
*method to check if the given name exists in the storage of clients.if not it creates a new client with that name
*@param name: the name of the client to check
* @return: a reference to the existing or newly created client
*/
static Client& ensure_client(const string& name) {
    
    for (Client& c : clients) {
        if(c.getName() == name) {
            return c;
		}
    }
    Client new_client(name);
    clients.push_back(new_client);
    return clients.back(); 

}

// clients -> JSON (kilit içeride)

static string users_json_from_clients_locked() {
    string out = "[";
    bool first = true;
    for (auto& c : clients) {
        if (!first) out += ",";
        first = false;
        out += "{\"user\":\"" + json_escape(c.getName( )) +
            "\",\"online\":" + (c.getIsOnline() ? string("true") : string("false")) + "}";
    }
    out += "]";
    return out;
}

// Tek SSE kuyruğu: hem "msg" hem "users" hem "sys"
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

    // SSE /events -> users, msg, sys yayınları (2 sn heartbeat)
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

    // POST /send -> komutları işle (/add, /remove) veya hedeflere mesaj gönder
    server.Post("/send", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("user") || !req.has_param("text")) {
            res.status = 400; res.set_content("Missing user/text", "text/plain"); return;
        }
        const string user = req.get_param_value("user");
        const string raw = req.get_param_value("text");
        const string text = trim(raw);

        // Komutlar: /add NAME  |  /remove NAME
        auto begins_with = [&](const string& pref) {
            return text.size() >= pref.size() && equal(pref.begin(), pref.end(), text.begin());
            };

        if (begins_with("/add ")) {
            string target = trim(text.substr(5));
            if (target.empty()) {
                string j = string("{\"text\":\"") + json_escape("Usage: /add NAME") +
                    "\",\"to\":[\"" + json_escape(user) + "\"]}";
                push_sse("sys", j);
                res.set_content("OK", "text/plain");
                return;
            }
            {
                lock_guard<mutex> lk(g_m);
                Client& me = ensure_client(user);
                me.addDestinationName(target);
            }
            string j = string("{\"text\":\"") + json_escape("Added " + target + " to your destinations") +
                "\",\"to\":[\"" + json_escape(user) + "\"]}";
            push_sse("sys", j);
            res.set_content("OK", "text/plain");
            return;
        }

        if (begins_with("/remove ")) {
            string target = trim(text.substr(8));
            if (target.empty()) {
                string j = string("{\"text\":\"") + json_escape("Usage: /remove NAME") +
                    "\",\"to\":[\"" + json_escape(user) + "\"]}";
                push_sse("sys", j);
                res.set_content("OK", "text/plain");
                return;
            }
            {
                lock_guard<mutex> lk(g_m);
                Client& me = ensure_client(user);
                me.removeDestinationName(target);
            }
            string j = string("{\"text\":\"") + json_escape("Removed " + target + " from your destinations") +
                "\",\"to\":[\"" + json_escape(user) + "\"]}";
            push_sse("sys", j);
            res.set_content("OK", "text/plain");
            return;
        }

        // Normal mesaj: sadece gönderenin hedef listesine gitsin (+ gönderen kendisi de görsün)
        vector<string> to_list;
        {
            lock_guard<mutex> lk(g_m);
            Client& me = ensure_client(user);
            to_list = me.getDestinationNames(); // kopya al
        }

        if (to_list.empty()) {
            // Hedef yoksa uyarı
            string j = string("{\"text\":\"") + json_escape("No destinations set. Use /add NAME") +
                "\",\"to\":[\"" + json_escape(user) + "\"]}";
            push_sse("sys", j);
            res.set_content("OK", "text/plain");
            return;
        }

        // JSON: {"user":"Alice","text":"...","to":["Bob","Charlie"]}
        string j = string("{\"user\":\"") + json_escape(user) + "\",\"text\":\"" + json_escape(text) +
            "\",\"to\":" + json_array_of_strings(to_list) + "}";
        push_sse("msg", j);
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
