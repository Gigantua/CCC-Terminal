#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>
#include "base64.h"

class ClipboardTransfer {
public:
    bool pasteEvents = false;
    std::function<void(const std::string&)> reply, store;
    std::function<void()> ask;

    void osc52(const std::string& text) {
        size_t at = text.find(';');
        std::string data = at == std::string::npos ? text : text.substr(at + 1);
        if (data != "?") { if (store) store(base64::decode(data)); return; }
        pending = Pending::Osc52;
        if (ask) ask();
    }

    void osc5522(const std::string& text) {
        size_t semi = text.find(';');
        std::map<std::string, std::string> meta = keys(text.substr(0, semi));
        std::string payload = semi == std::string::npos ? std::string() : text.substr(semi + 1);
        std::string type = meta["type"], id = meta["id"];
        if (type == "read") read(meta, base64::decode(payload), id);
        else if (type == "write") startWrite(meta, id);
        else if (type == "wdata") writeData(meta, payload);
    }

    void answer(const std::string& clipboard, bool granted) {
        Pending was = std::exchange(pending, Pending::None);
        if (was == Pending::Osc52) send("52;c;" + base64::encode(granted ? clipboard : std::string()));
        else if (was == Pending::Read) {
            if (granted) sendRead(clipboard, readId);
            else send("5522;type=read:status=EPERM" + idSuffix(readId));
        }
    }

    bool announcePaste(const std::string& text) {
        if (!pasteEvents) return false;
        std::random_device random;
        char hex[17];
        snprintf(hex, sizeof hex, "%08x%08x", random(), random());
        pastePassword = hex;
        pasteText = text;
        std::string pw = ":pw=" + base64::encode(pastePassword);
        send("5522;type=read:status=OK" + pw);
        send("5522;type=read:status=DATA:mime=" + base64::encode(".") + pw + ";" + base64::encode(TEXT));
        send("5522;type=read:status=DONE" + pw);
        return true;
    }

    void reset() {
        pending = Pending::None;
        writing = false;
        written.clear();
        pastePassword.clear();
        pasteText.clear();
        pasteEvents = false;
    }

private:
    static constexpr const char* TEXT = "text/plain";
    static constexpr size_t CHUNK = 4096, WRITE_LIMIT = 64u << 20;
    enum class Pending { None, Osc52, Read } pending = Pending::None;
    std::string readId, writeId, pastePassword, pasteText;
    std::map<std::string, std::string> written;
    size_t writtenBytes = 0;
    bool writing = false;

    static std::map<std::string, std::string> keys(const std::string& meta) {
        std::map<std::string, std::string> out;
        size_t from = 0;
        while (from <= meta.size()) {
            size_t to = meta.find(':', from);
            std::string kv = meta.substr(from, to == std::string::npos ? std::string::npos : to - from);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) out[kv.substr(0, eq)] = kv.substr(eq + 1);
            if (to == std::string::npos) break;
            from = to + 1;
        }
        return out;
    }

    static std::vector<std::string> words(const std::string& s) {
        std::vector<std::string> out;
        size_t at = 0;
        while ((at = s.find_first_not_of(" \t\r\n", at)) != std::string::npos) {
            size_t end = s.find_first_of(" \t\r\n", at);
            out.push_back(s.substr(at, end - at));
            at = end;
        }
        return out;
    }

    static bool textType(const std::string& mime) { return mime == TEXT || mime.rfind("text/plain;", 0) == 0 || mime == "UTF8_STRING" || mime == "STRING" || mime == "TEXT"; }

    static std::string idSuffix(const std::string& id) {
        std::string clean;
        for (char c : id) if (std::isalnum((unsigned char)c) || std::strchr("-_+.", c)) clean += c;
        return clean.empty() ? clean : ":id=" + clean;
    }

    void send(const std::string& body) { if (reply) reply("\x1b]" + body + "\x1b\\"); }

    void sendRead(const std::string& text, const std::string& id) {
        std::string suffix = idSuffix(id), mime = ":mime=" + base64::encode(TEXT);
        send("5522;type=read:status=OK" + suffix);
        for (size_t at = 0; at < text.size(); at += CHUNK) send("5522;type=read:status=DATA" + mime + suffix + ";" + base64::encode(text.substr(at, CHUNK)));
        send("5522;type=read:status=DONE" + suffix);
    }

    void read(std::map<std::string, std::string>& meta, const std::string& mimes, const std::string& id) {
        std::string suffix = idSuffix(id);
        if (meta.count("loc")) { send("5522;type=read:status=ENOSYS" + suffix); return; }
        std::vector<std::string> wanted = words(mimes);
        if (wanted.size() == 1 && wanted[0] == ".") {
            send("5522;type=read:status=OK" + suffix);
            send("5522;type=read:status=DATA:mime=" + base64::encode(".") + suffix + ";" + base64::encode(TEXT));
            send("5522;type=read:status=DONE" + suffix);
            return;
        }
        if (std::none_of(wanted.begin(), wanted.end(), textType)) { send("5522;type=read:status=ENOSYS" + suffix); return; }
        if (!pastePassword.empty() && meta.count("name") && base64::decode(meta["pw"]) == pastePassword) {
            pastePassword.clear();
            sendRead(std::exchange(pasteText, std::string()), id);
            return;
        }
        if (pending != Pending::None) { send("5522;type=read:status=EBUSY" + suffix); return; }
        pending = Pending::Read;
        readId = id;
        if (ask) ask();
    }

    void startWrite(std::map<std::string, std::string>& meta, const std::string& id) {
        written.clear();
        writtenBytes = 0;
        writeId = id;
        writing = !meta.count("loc");
        if (!writing) send("5522;type=write:status=ENOSYS" + idSuffix(id));
    }

    void writeData(std::map<std::string, std::string>& meta, const std::string& payload) {
        if (!writing) return;
        if (meta.count("mime")) {
            writtenBytes += payload.size();
            if (writtenBytes > WRITE_LIMIT) { writing = false; send("5522;type=write:status=EFBIG" + idSuffix(writeId)); return; }
            written[base64::decode(meta["mime"])] += payload;
            return;
        }
        writing = false;
        for (auto& [mime, data] : written)
            if (textType(mime)) { if (store) store(base64::decode(data)); break; }
        written.clear();
        send("5522;type=write:status=DONE" + idSuffix(writeId));
    }
};
