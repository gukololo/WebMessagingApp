#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <iostream>
#include <fstream>
#include <string>

#include "httplib.h"
#include "Client.h"
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Shell32.lib")

using namespace std;
string username;
static bool read_text_file(const string& path, string& out) {
    ifstream f(path, ios::binary);
    if (!f) return false;
    out.assign(istreambuf_iterator<char>(f), istreambuf_iterator<char>());
    return true;
}

// Basit replace-all (placeholder doldurmak için)
static void replace_all(string& s, const string& from, const string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    } 
}

int main() {
    static httplib::Server server;
    server.set_mount_point("/static", "wwwroot/assets");
   /* server.set_file_extension_and_mimetype_mapping("jpg", "image/jpeg");
    server.set_file_extension_and_mimetype_mapping("png", "image/png");
    server.set_file_extension_and_mimetype_mapping("css", "text/css");
    server.set_file_extension_and_mimetype_mapping("js", "application/javascript");*/

    // GET / → login.html'i dosyadan yükle ve gönder
    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        string html;
        if (!read_text_file("wwwroot/login.html", html)) {
            res.status = 500;
            res.set_content("login.html not found", "text/plain; charset=UTF-8");
            return;
        }
        res.set_content(html, "text/html; charset=UTF-8");
        });

    // POST /login → kullanıcı adını al, kaydet, /success'e yönlendir
    server.Post("/login", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("username")) {
            res.status = 400;
            res.set_content("Username is required", "text/plain; charset=UTF-8");
            return;
        }
        username = req.get_param_value("username");


        res.set_redirect("/success");
        });

    // GET /success success.html'i yükle, {{username}} yer tutucusunu doldur, gönder
    server.Get("/success", [](const httplib::Request&, httplib::Response& res) {
        if (username.empty()) { res.set_redirect("/"); return; }

        string html;
        if (!read_text_file("wwwroot/success.html", html)) {
            res.status = 500;
            res.set_content("success.html not found", "text/plain; charset=UTF-8");
            return;
        }
        replace_all(html, "{{username}}", username);
        res.set_content(html, "text/html; charset=UTF-8");
        });

    // Tarayıcıyı aç & sunucuyu başlat
    ShellExecuteA(NULL, "open", "http://127.0.0.1:8080", NULL, NULL, SW_SHOWNORMAL);
    cout << "HTTP server: http://127.0.0.1:8080/ (Ctrl+C to stop)\n";
    server.listen("127.0.0.1", 8080);
    
    return 0;
}
