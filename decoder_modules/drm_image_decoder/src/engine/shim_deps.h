#ifndef DRM_SHIM_DEPS_H
#define DRM_SHIM_DEPS_H
/*
 * shim_deps.h — stubs for the non-DSP ancillary headers that QSSTV's
 * sourcedecoder.cpp pulls in but that we don't want to ship inside an
 * SDR++ module : FTP upload, logbook, Qt UI widgets, dispatcher signals,
 * hybrid-mode encryption keys.
 *
 * All routines here are no-ops; sourcedecoder.cpp will read isHybrid==false
 * and skip every hybrid-FTP code path.
 */

#include "qt_shim.h"

// --- ftpFunctions ----------------------------------------------------------
class ftpFunctions : public QObject {
public:
    ftpFunctions(QObject* parent = nullptr) : QObject(parent) {}
    bool setupFtp(const QString&, const QString&, int,
                  const QString&, const QString&, const QString&) { return false; }
    bool changePath(const QString&, bool /*create*/ = false)      { return false; }
    bool putFile (const QString&, const QString&)                 { return false; }
    bool getFile (const QString&, const QString&)                 { return false; }
    bool downloadFile(const QString&, const QString&)             { return false; }
    bool downloadFile(const QByteArray&, const QString&, bool, bool) { return false; }
    bool listFiles(const QString&)                                 { return false; }
    bool deleteFile(const QString&)                                { return false; }
    bool uploadData(const char*, const QString&, bool, bool)      { return false; }
    bool mremove(const QString&, bool, bool)                      { return false; }
    void changeThreadName(const QString&)                         {}
    void disconnect()                                             {}
    bool isBusy() const                                           { return false; }
    void abort()                                                  {}
    // signals: void downloadDone(bool err, QString filename);  -- ignored
};

// --- Configuration QSSTV qu'on neutralise (mode hybride toujours OFF) -----
inline bool enableHybridRx     = false;
inline bool enableHybridNotify = false;

namespace soundBase {
    enum { SNDINFROMFILE = 0, SNDINFROMSOUNDCARD = 1 };
}
inline int soundRoutingInput = 1; // != SNDINFROMFILE

// --- logbook --------------------------------------------------------------
namespace logbook {
    inline void logQSO(const QString&, const QString&)            {}
    inline void logImage(const QString&, const QString&, const QString&) {}
}

class logBook : public QObject {
public:
    void logQSO(const QString&, const QString&, const QString&) {}
};
inline logBook* logBookPtr = nullptr;

// --- dispatch / dispatcher ------------------------------------------------
class Dispatcher : public QObject {
public:
    static Dispatcher* instance() { static Dispatcher d; return &d; }
    void notifyImageReceived(const QString&, const QByteArray&) {}
    void notifyImageReceived(const QString&)                    {}
    void emitDrmImage(const QString&)                            {}
};
inline Dispatcher* dispatcher() { return Dispatcher::instance(); }

// --- drmstatusframe (UI widget) -------------------------------------------
class drmStatusFrame : public QObject {
public:
    void setStatus(const QString&) {}
    void addLine  (const QString&) {}
    void clear()                   {}
};

// --- hybridcrypt (no-op : we never decrypt) -------------------------------
class hybridCrypt {
public:
    hybridCrypt() {}
    bool enCrypt(QByteArray* /*ba*/) { return false; }
    bool deCrypt(QByteArray* /*ba*/) { return false; }
    QString host()   { return QString(""); }
    QString user()   { return QString(""); }
    QString passwd() { return QString(""); }
    QString dir()    { return QString(""); }
    int     port()   { return 0; }
};

// --- appdefs (constants reused from QSSTV) --------------------------------
#define BASESAMPLERATE     48000
#define SUBSAMPLINGFACTOR  4
#define MONOCHANNEL        1
#define STEREOCHANNEL      2
#define RXSTRIPE           1024
#define TXSTRIPE           1024
#define DOWNSAMPLESIZE     (SUBSAMPLINGFACTOR * RXSTRIPE)
#define SAMPLERATE         (BASESAMPLERATE / SUBSAMPLINGFACTOR)
typedef double DSPFLOAT;
typedef double _REAL;
typedef short  _SAMPLE;
typedef unsigned char _BYTE;
typedef bool          _BOOLEAN;
typedef unsigned char _BINARY;

// Strings referenced by sourcedecoder for hybrid mode (never reached because
// isHybrid stays false).
inline const QString hybridFtpHybridFilesDirectory = QString("");
inline const QString hybridNotifyDir               = QString("");

// Chemin de stockage des images DRM reçues : non utilisé (QFile shim ne touche
// pas le disque, les images vont dans drmcapture::files), mais doit exister
// pour que les compositions de chemin de QSSTV compilent.
inline QString rxDRMImagesPath = QString(".");

// Qt event "objects" envoyés par QSSTV au thread UI (`QApplication::postEvent`).
// Nous n'avons pas de thread UI : on les construit en no-op et personne ne les
// poste (sourcedecoder.cpp les `new` puis ne fait rien d'utile avec dans notre
// chemin, car isHybrid reste false et alreadyDisplayed est géré séparément).
class loadRXImageEvent {
public:
    loadRXImageEvent(const QString& /*filename*/) {}
};
class displayMBoxEvent {
public:
    displayMBoxEvent(const QString& /*title*/, const QString& /*msg*/) {}
};
class prepareFixEvent {
public:
    prepareFixEvent(const QByteArray& /*ba*/) {}
};
class rxDRMStatusEvent {
public:
    rxDRMStatusEvent(const QString& /*msg*/) {}
};
class displayTextEvent {
public:
    displayTextEvent(const QString& /*msg*/) {}
    displayTextEvent(const QString& /*title*/, const QString& /*msg*/) {}
};
class saveDRMImageEvent {
public:
    saveDRMImageEvent(const QString& /*filename*/, const QString& /*info*/) {}
    // QSSTV utilise waitFor(&done) pour bloquer jusqu'à ce que l'UI ait sauvé.
    // On simule un succès immédiat.
    void waitFor(bool* done) { if (done) *done = true; }
    void waitFor(int*  done) { if (done) *done = 1; }
};

// Cible des postEvent — sans Qt elle reste nullptr ; les events n'ont nulle
// part où aller mais doivent compiler.
inline QObject* dispatcherPtr = nullptr;

// Globales QSSTV utilisées en chemins logging/notification : valeurs neutres.
inline QString myCallsign     = QString("");
// (lastAvgSNR est déjà défini par drm.cpp — ne pas redéfinir ici)

// Helpers de QSSTV qui produisent des strings descriptives de mode/QAM/etc.
// pour le log ou des popups. On les neutralise.
inline QString compactModeToString(unsigned /*modeCode*/) { return QString(""); }
inline QString modeToString       (int      /*modeCode*/) { return QString(""); }

// QApplication::postEvent — no-op.
inline void postEvent(QObject* /*recipient*/, void* /*event*/) {}

// ---------------------------------------------------------------------------
// drmprogress : suivi de la progression d'un objet MOT en cours de réception.
// Alimenté par sourcedecoder.cpp à chaque segment reçu et lu par le wrapper
// drm_decoder.cpp pour l'affichage UI (progression + ETA).
// ---------------------------------------------------------------------------
#include <atomic>
#include <mutex>
#include <string>
namespace drmprogress {
    extern std::atomic<int>      currentSegments;  // segments reçus
    extern std::atomic<int>      totalSegments;    // segments attendus
    extern std::atomic<int>      bodySize;         // taille totale (octets) si connue
    extern std::atomic<int>      segmentSize;      // taille d'un segment (octets)
    extern std::atomic<uint64_t> frameCounter;     // compteur de trames OFDM
    extern std::mutex            nameMtx;
    extern std::string           objectName;       // nom du fichier en cours
}

#endif // DRM_SHIM_DEPS_H
