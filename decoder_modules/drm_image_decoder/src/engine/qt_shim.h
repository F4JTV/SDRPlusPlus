#ifndef DRM_QT_SHIM_H
#define DRM_QT_SHIM_H
/*
 * qt_shim.h — Qt compatibility layer for the vendored QSSTV / RXAMADRM engine.
 *
 * Lets the engine compile inside the SDR++ "drm_image_decoder" module WITHOUT
 * a real Qt dependency. The shim is split into three families:
 *
 *   (1) Logging / chrome  — QString, QList, addToLog, LOG* constants.
 *
 *   (2) Data containers   — QByteArray, QVector (= std::vector alias).
 *                           These are real, working containers because the
 *                           DRM source decoder actually relies on them for
 *                           MOT object assembly.
 *
 *   (3) File / app stubs  — QFile, QFileInfo, QImage, QObject, QApplication,
 *                           QThread, QDateTime. Reduced to the surface the
 *                           engine actually calls. QFile is the ONLY one
 *                           with real side-effects: every WriteOnly close()
 *                           captures the buffer into ::drmcapture so the SDR++
 *                           wrapper can surface the received image.
 */

#include <string>
#include <vector>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cctype>
#include <mutex>
#include <unistd.h>  // usleep, lseek

// ---------------------------------------------------------------------------
// Logging-level constants used by the engine. Values are irrelevant for us.
// ---------------------------------------------------------------------------
#ifndef LOGDRMDEMOD
#define LOGDRMDEMOD   0
#endif
#ifndef LOGPERFORM
#define LOGPERFORM    0
#endif
#ifndef LOGFFT
#define LOGFFT        0
#endif
#ifndef LOGDRMSYNC
#define LOGDRMSYNC    0
#endif
#ifndef LOGDRMCHANNEL
#define LOGDRMCHANNEL 0
#endif
#ifndef LOGDRMSRC
#define LOGDRMSRC     0
#endif
#ifndef LOGDRMMOT
#define LOGDRMMOT     0
#endif
#ifndef LOGALL
#define LOGALL        0
#endif

class QString; // forward (for QByteArray)

// ---------------------------------------------------------------------------
// QByteArray — real byte buffer (the engine uses it heavily for MOT assembly).
// ---------------------------------------------------------------------------
class QByteArray {
public:
    QByteArray() {}
    QByteArray(const char* d, int len) : v_(d, d + (len > 0 ? len : 0)) {}
    QByteArray(int size, char c) : v_(size > 0 ? size : 0, c) {}
    QByteArray(int size, int c)  : v_(size > 0 ? size : 0, (char)c) {}
    explicit QByteArray(const std::vector<char>& v) : v_(v) {}

    void clear()                                  { v_.clear(); }
    int  size()                            const  { return (int)v_.size(); }
    int  count()                           const  { return (int)v_.size(); }
    int  length()                          const  { return (int)v_.size(); }
    bool isEmpty()                         const  { return v_.empty(); }
    const char* data()                     const  { return v_.empty() ? "" : v_.data(); }
    char*       data()                            { return v_.empty() ? nullptr : v_.data(); }
    const char* constData()                const  { return data(); }
    char at(int i)                         const  { return v_[(size_t)i]; }
    char& operator[](int i)                       { return v_[(size_t)i]; }
    char  operator[](int i)               const   { return v_[(size_t)i]; }

    QByteArray& append(const char* s, int len)    { v_.insert(v_.end(), s, s + len); return *this; }
    QByteArray& append(const char* s)             { if (s) v_.insert(v_.end(), s, s + std::strlen(s)); return *this; }
    QByteArray& append(char c)                    { v_.push_back(c); return *this; }
    QByteArray& append(const QByteArray& o)       { v_.insert(v_.end(), o.v_.begin(), o.v_.end()); return *this; }
    QByteArray& operator+=(const QByteArray& o)   { return append(o); }
    QByteArray& operator+=(const char* o)         { return append(o); }
    QByteArray& operator+=(char c)                { return append(c); }
    QByteArray  operator+(const QByteArray& o) const { QByteArray r(*this); r.append(o); return r; }
    QByteArray  operator+(const char* o)       const { QByteArray r(*this); r.append(o); return r; }
    QByteArray  operator+(char c)              const { QByteArray r(*this); r.append(c); return r; }
    QByteArray& prepend(const QByteArray& o)      { v_.insert(v_.begin(), o.v_.begin(), o.v_.end()); return *this; }
    QByteArray& prepend(const char* s)            { if (s) v_.insert(v_.begin(), s, s + std::strlen(s)); return *this; }
    QByteArray& prepend(char c)                   { v_.insert(v_.begin(), c); return *this; }

    void chop(int n)                              { if (n > (int)v_.size()) n = v_.size(); if (n > 0) v_.resize(v_.size() - n); }
    void remove(int pos, int n) {
        if (pos < 0 || pos >= (int)v_.size()) return;
        if (n < 0) n = 0;
        if (pos + n > (int)v_.size()) n = v_.size() - pos;
        v_.erase(v_.begin() + pos, v_.begin() + pos + n);
    }
    void resize(int n)                            { v_.resize(n > 0 ? n : 0); }
    void fill(char c, int n)                      { v_.assign(n > 0 ? n : 0, c); }
    void fill(unsigned char c, int n)             { v_.assign(n > 0 ? n : 0, (char)c); }
    void fill(int c, int n)                       { v_.assign(n > 0 ? n : 0, (char)c); }
    QByteArray leftJustified(int width, char fill = ' ') const {
        QByteArray r(*this);
        if ((int)r.v_.size() < width) r.v_.insert(r.v_.end(), width - r.v_.size(), fill);
        return r;
    }

    QByteArray left(int n)  const                 { n = std::min((int)v_.size(), std::max(0, n)); return QByteArray(v_.data(), n); }
    QByteArray right(int n) const                 { n = std::min((int)v_.size(), std::max(0, n)); int off = (int)v_.size() - n; return QByteArray(v_.data() + off, n); }
    QByteArray mid(int pos, int n = -1) const {
        if (pos < 0 || pos >= (int)v_.size()) return QByteArray();
        if (n < 0 || pos + n > (int)v_.size()) n = v_.size() - pos;
        return QByteArray(v_.data() + pos, n);
    }
    bool operator==(const QByteArray& o) const    { return v_ == o.v_; }

private:
    std::vector<char> v_;
};

// ---------------------------------------------------------------------------
// QString — keeps log strings, supports .arg() chaining (no-op formatting).
// ---------------------------------------------------------------------------
class QString {
public:
    QString() {}
    QString(const char* s) : s_(s ? s : "") {}
    QString(const std::string& s) : s_(s) {}

    QString& arg(int)              { return *this; }
    QString& arg(unsigned int)     { return *this; }
    QString& arg(long)             { return *this; }
    QString& arg(unsigned long)    { return *this; }
    QString& arg(long long)        { return *this; }
    QString& arg(unsigned long long) { return *this; }
    QString& arg(float)            { return *this; }
    QString& arg(double)           { return *this; }
    QString& arg(char)             { return *this; }
    QString& arg(const char*)      { return *this; }
    QString& arg(const QString&)   { return *this; }
    // Variantes avec width, base, format... (utilisées pour formater des SNR
    // genre arg(snr, 0, 'f', 1)). Toutes no-op, on n'utilise pas la sortie.
    QString& arg(int, int /*w*/, int /*b*/ = 10, char /*pad*/ = ' ')           { return *this; }
    QString& arg(unsigned int, int /*w*/, int /*b*/ = 10, char /*pad*/ = ' ')  { return *this; }
    QString& arg(double, int /*w*/, char /*fmt*/ = 'g', int /*prec*/ = -1, char /*pad*/ = ' ') { return *this; }
    QString& arg(float, int /*w*/, char /*fmt*/ = 'g', int /*prec*/ = -1, char /*pad*/ = ' ')  { return *this; }

    const std::string& toStdString() const { return s_; }
    const char* c_str()             const { return s_.c_str(); }
    QByteArray  toLatin1()          const { return QByteArray(s_.data(), (int)s_.size()); }
    QByteArray  toUtf8()            const { return QByteArray(s_.data(), (int)s_.size()); }
    bool isEmpty()                  const { return s_.empty(); }
    void clear()                          { s_.clear(); }
    int  length()                   const { return (int)s_.size(); }
    int  size()                     const { return (int)s_.size(); }

    QString left(int n)  const            { n = std::min((int)s_.size(), std::max(0, n)); return QString(s_.substr(0, n)); }
    QString right(int n) const            { n = std::min((int)s_.size(), std::max(0, n)); return QString(s_.substr(s_.size() - n)); }
    QString mid(int pos, int n = -1) const {
        if (pos < 0 || pos >= (int)s_.size()) return QString();
        if (n < 0) return QString(s_.substr(pos));
        return QString(s_.substr(pos, n));
    }

    QString& operator=(const char* s)        { s_ = (s ? s : ""); return *this; }
    QString& operator=(const std::string& s) { s_ = s; return *this; }
    QString& operator=(const QByteArray& ba) { s_.assign(ba.data(), ba.size()); return *this; }
    QString  operator+(const QString& o) const { return QString(s_ + o.s_); }
    QString  operator+(const char* o)    const { return QString(s_ + (o ? o : "")); }
    QString& operator+=(const QString& o)  { s_ += o.s_; return *this; }
    QString& operator+=(const char* o)     { if (o) s_ += o; return *this; }
    QString& operator+=(char c)            { s_ += c; return *this; }
    QString& append(const QString& o)      { s_ += o.s_; return *this; }
    QString& append(const char* o)         { if (o) s_ += o; return *this; }
    QString& append(char c)                { s_ += c; return *this; }
    QString& prepend(const QString& o)     { s_ = o.s_ + s_; return *this; }
    QString& prepend(const char* o)        { if (o) s_ = std::string(o) + s_; return *this; }
    QString& prepend(char c)               { s_ = c + s_; return *this; }
    QString  leftJustified(int width, char fill = ' ') const {
        std::string r = s_;
        if ((int)r.size() < width) r.append(width - r.size(), fill);
        return QString(r);
    }
    QString  toLower() const {
        std::string r = s_; for (auto& c : r) c = (char)std::tolower((unsigned char)c);
        return QString(r);
    }
    QString  toUpper() const {
        std::string r = s_; for (auto& c : r) c = (char)std::toupper((unsigned char)c);
        return QString(r);
    }
    bool operator==(const QString& o) const  { return s_ == o.s_; }
    bool operator!=(const QString& o) const  { return s_ != o.s_; }

    static QString number(int v)           { char b[32]; std::snprintf(b, 32, "%d", v); return QString(b); }
    static QString number(unsigned v)      { char b[32]; std::snprintf(b, 32, "%u", v); return QString(b); }
    static QString number(long v)          { char b[32]; std::snprintf(b, 32, "%ld", v); return QString(b); }
    static QString fromLatin1(const char* s, int len = -1) {
        if (!s) return QString();
        return (len < 0) ? QString(s) : QString(std::string(s, (size_t)len));
    }
    static QString fromUtf8(const char* s, int len = -1) {
        return fromLatin1(s, len);
    }

private:
    std::string s_;
};

inline QString operator+(const char* l, const QString& r) { return QString(std::string(l ? l : "") + r.toStdString()); }

// ---------------------------------------------------------------------------
// QList<T>, QVector<T> — std::vector backed.
// ---------------------------------------------------------------------------
template <typename T>
class QList {
public:
    QList() {}
    void append(const T& v)      { v_.push_back(v); }
    void push_back(const T& v)   { v_.push_back(v); }
    void prepend(const T& v)     { v_.insert(v_.begin(), v); }
    void clear()                 { v_.clear(); }
    int  count()           const { return (int)v_.size(); }
    int  size()            const { return (int)v_.size(); }
    bool isEmpty()         const { return v_.empty(); }
    bool contains(const T& v) const {
        return std::find(v_.begin(), v_.end(), v) != v_.end();
    }
    T&       at(int i)             { return v_[(size_t)i]; }
    const T& at(int i)       const { return v_[(size_t)i]; }
    T&       operator[](int i)       { return v_[(size_t)i]; }
    const T& operator[](int i) const { return v_[(size_t)i]; }
    T&       first()                  { return v_.front(); }
    const T& first()            const { return v_.front(); }
    T&       last()                   { return v_.back(); }
    const T& last()             const { return v_.back(); }
    void removeAt(int i)             { if (i >= 0 && i < (int)v_.size()) v_.erase(v_.begin() + i); }
    void removeFirst()               { if (!v_.empty()) v_.erase(v_.begin()); }
    void removeLast()                { if (!v_.empty()) v_.pop_back(); }
    T    takeAt(int i)               { T t = v_[(size_t)i]; v_.erase(v_.begin() + i); return t; }
    T    takeFirst()                 { T t = v_.front(); v_.erase(v_.begin()); return t; }
    T    takeLast()                  { T t = v_.back(); v_.pop_back(); return t; }
    typename std::vector<T>::iterator begin() { return v_.begin(); }
    typename std::vector<T>::iterator end()   { return v_.end(); }
private:
    std::vector<T> v_;
};

template <typename T>
using QVector = QList<T>;

// ---------------------------------------------------------------------------
// QFile capture hook.
//
// The QSSTV source-decoder calls QFile::open(WriteOnly) + write() + close()
// when it finishes assembling an image. We do NOT write to disk; instead
// every closed WriteOnly QFile dumps its filename + bytes into ::drmcapture,
// which the SDR++ wrapper reads to surface the received image.
// ---------------------------------------------------------------------------
namespace drmcapture {
    struct CapturedFile {
        std::string name;      // filename the engine intended to write
        QByteArray  bytes;     // the file contents
        uint64_t    timestamp; // capture epoch seconds
    };
    extern std::mutex            mtx;
    extern std::vector<CapturedFile> files;     // history of received files
    extern uint64_t              counter;       // strictly increasing
    inline void push(const std::string& n, const QByteArray& b) {
        std::lock_guard<std::mutex> lk(mtx);
        files.push_back({n, b, (uint64_t)std::time(nullptr)});
        counter++;
    }
}

// ---------------------------------------------------------------------------
// QFile / QFileInfo / QIODevice — read paths kept as no-ops; write paths
// redirected to drmcapture above.
// ---------------------------------------------------------------------------
class QIODevice {
public:
    enum OpenModeFlag : int { ReadOnly = 1, WriteOnly = 2, ReadWrite = 3, Append = 4, Truncate = 8, Text = 16 };
};

class QFile : public QIODevice {
public:
    QFile() : open_(false), mode_(0) {}
    explicit QFile(const QString& name) : name_(name), open_(false), mode_(0) {}

    void setFileName(const QString& name)  { name_ = name; }
    QString fileName() const                { return name_; }

    int open(int mode) {
        mode_ = mode;
        buf_.clear();
        open_ = true;
        return 1; // >0 = success in the engine's checks
    }
    void close() {
        if (open_ && (mode_ & QIODevice::WriteOnly)) {
            drmcapture::push(name_.toStdString(), buf_);
        }
        open_ = false;
    }
    long long write(const QByteArray& ba) {
        if (open_ && (mode_ & QIODevice::WriteOnly)) {
            buf_.append(ba);
            return ba.size();
        }
        return -1;
    }
    long long write(const char* d, int len) {
        if (open_ && (mode_ & QIODevice::WriteOnly)) {
            buf_.append(d, len);
            return len;
        }
        return -1;
    }
    QByteArray readAll() { return QByteArray(); } // not used in our path
    bool atEnd() const   { return true; }

private:
    QString    name_;
    QByteArray buf_;
    bool       open_;
    int        mode_;
};

class QFileInfo {
public:
    QFileInfo() {}
    explicit QFileInfo(const QString& path) : path_(path) {}
    explicit QFileInfo(const QFile& f) : path_(f.fileName()) {}

    QString fileName() const {
        const std::string& p = path_.toStdString();
        size_t slash = p.find_last_of("/\\");
        return QString(slash == std::string::npos ? p : p.substr(slash + 1));
    }
    QString baseName() const {
        QString fn = fileName();
        const std::string& s = fn.toStdString();
        size_t dot = s.find_first_of('.');
        return QString(dot == std::string::npos ? s : s.substr(0, dot));
    }
    QString completeBaseName() const {
        QString fn = fileName();
        const std::string& s = fn.toStdString();
        size_t dot = s.find_last_of('.');
        return QString(dot == std::string::npos ? s : s.substr(0, dot));
    }
    QString suffix() const {
        QString fn = fileName();
        const std::string& s = fn.toStdString();
        size_t dot = s.find_last_of('.');
        return QString(dot == std::string::npos ? "" : s.substr(dot + 1));
    }
    QString absoluteFilePath() const { return path_; }
    bool exists() const              { return false; } // we never write to disk
private:
    QString path_;
};

// ---------------------------------------------------------------------------
// QImage — only loadFromData(QByteArray) is used (to detect a valid image).
// We do a magic-byte sniff for JPEG/PNG/BMP and report isNull accordingly.
// ---------------------------------------------------------------------------
class QImage {
public:
    QImage() : null_(true) {}
    bool loadFromData(const QByteArray& ba) {
        const char* d = ba.data();
        int n = ba.size();
        if (n < 12) { null_ = true; return false; }
        // JPEG: FF D8 FF .. ; PNG: 89 50 4E 47 ; BMP: 42 4D
        // JP2 (JPEG 2000): 00 00 00 0C 6A 50 20 20 0D 0A 87 0A
        bool jpeg = (unsigned char)d[0] == 0xFF && (unsigned char)d[1] == 0xD8 && (unsigned char)d[2] == 0xFF;
        bool png  = (unsigned char)d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
        bool bmp  = d[0] == 'B' && d[1] == 'M';
        bool jp2  = d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0x0C &&
                    d[4] == 'j' && d[5] == 'P';
        null_ = !(jpeg || png || bmp || jp2);
        return !null_;
    }
    bool loadFromData(const QByteArray* ba) { return ba ? loadFromData(*ba) : false; }
    bool load(const QString& /*filename*/) {
        // Nous ne lisons jamais de disque. Renvoie "image invalide" pour
        // forcer sourcedecoder à utiliser le chemin readAll() de QFile.
        null_ = true;
        return false;
    }
    bool isNull() const { return null_; }
private:
    bool null_;
};

// ---------------------------------------------------------------------------
// QObject, QApplication, qApp, QThread, QDateTime — stubs.
// ---------------------------------------------------------------------------
class QObject {
public:
    QObject(QObject* /*parent*/ = nullptr) {}
    virtual ~QObject() {}
};
class QApplication {
public:
    static void processEvents() {}
    static void postEvent(QObject* /*receiver*/, void* /*event*/) {}
};
inline QApplication* qApp_() { static QApplication a; return &a; }
#define qApp qApp_()

class QThread {
public:
    static void msleep(unsigned long /*ms*/) {}
    static void usleep(unsigned long /*us*/) {}
};

class QDateTime {
public:
    static QDateTime currentDateTime() { return QDateTime(); }
    QString toString(const char* /*fmt*/ = nullptr) const {
        time_t t = time(nullptr);
        char b[32]; strftime(b, 32, "%Y%m%d_%H%M%S", localtime(&t));
        return QString(b);
    }
};

// Qt-style signal/slot keywords — make slots:/signals: in headers compile.
#ifndef signals
#define signals public
#endif
#ifndef slots
#define slots /* nothing */
#endif
#ifndef Q_OBJECT
#define Q_OBJECT
#endif
#ifndef Q_SLOTS
#define Q_SLOTS
#endif
#ifndef Q_SIGNALS
#define Q_SIGNALS public
#endif
#define emit /* nothing */

// SIGNAL() / SLOT() — old-style Qt connect macros; reduce to a no-op string.
#ifndef SIGNAL
#define SIGNAL(x) #x
#endif
#ifndef SLOT
#define SLOT(x)   #x
#endif

// errorOut() — QSSTV's std::cerr-flavoured logger; we send to std::cerr.
#include <iostream>
inline std::ostream& errorOut() { return std::cerr; }
inline std::ostream& infoOut()  { return std::cerr; }

// connect() — no-op (returns true). Variadic to swallow any signature.
template <typename... Args>
inline bool connect(Args&&...) { return true; }
template <typename... Args>
inline bool disconnect(Args&&...) { return true; }

// quint*/qint* types
typedef unsigned char  quint8;
typedef unsigned short quint16;
typedef unsigned int   quint32;
typedef unsigned long long quint64;
typedef signed char    qint8;
typedef short          qint16;
typedef int            qint32;
typedef long long      qint64;

// ---------------------------------------------------------------------------
// addToLog — engine log entry point. Routed to an optional sink.
// ---------------------------------------------------------------------------
namespace drmlog {
    typedef void (*LogSink)(const char* text, int level);
    extern LogSink g_sink;
    inline void emit_log(const QString& msg, int level) {
        if (g_sink) g_sink(msg.c_str(), level);
    }
}
inline void addToLog(const QString& msg, int level) { drmlog::emit_log(msg, level); }
inline void addToLog(const char* msg, int level)    { if (drmlog::g_sink) drmlog::g_sink(msg, level); }

// ---------------------------------------------------------------------------
// Engine entry-point forwards (real impls in channeldecode.cpp / sourcedecoder.cpp,
// or stubs in engine_stubs.cpp depending on the increment level).
// ---------------------------------------------------------------------------
void channel_decoding(void);
void source_decoding(void);

#endif // DRM_QT_SHIM_H
