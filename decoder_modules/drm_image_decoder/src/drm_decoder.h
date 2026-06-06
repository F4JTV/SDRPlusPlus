#pragma once
/*
 * drm_decoder.h — clean, Qt-free wrapper around the vendored RXAMADRM /
 * QSSTV DRM acquisition engine, for the SDR++ "drm_image_decoder" module.
 *
 * Pipeline fed to this wrapper:
 *      real audio @ 12 kHz  --(Hilbert FIR)-->  analytic (I/Q)  -->  engine
 *
 * The engine carries heavy global state (it is effectively a singleton), so
 * only ONE DRMDecoder may be active at a time. The SDR++ module declares max
 * 1 instance accordingly.
 *
 * Increment 1 exposes acquisition status: robustness-mode detection (A/B/C/D),
 * time/frequency/frame synchronization, WMER/SNR, PSD and the equalized OFDM
 * cell constellation. Channel decoding (FAC/SDC/MSC) and MOT image reassembly
 * are wired as increment 2 / 3 plug-in points.
 */

#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <complex>

namespace drm {

    // DRM input sample rate the engine works at (BASESAMPLERATE/SUBSAMPLINGFACTOR
    // in QSSTV == 48000/4). Audio MUST be supplied to feed() at this rate.
    constexpr int    ENGINE_SAMPLE_RATE = 12000;
    constexpr int    STRIPE_COMPLEX     = 1024;   // complex samples per demodulate() call

    enum class Robustness { NONE = -1, A = 0, B = 1, C = 2, D = 3 };

    struct Status {
        bool        timeSync       = false;
        bool        freqSync       = false;
        bool        frameSync      = false;
        bool        facAvailable   = false;
        Robustness  robustness     = Robustness::NONE;
        int         spectrumOcc    = -1;     // 0..5 (DRM spectrum-occupancy code)
        float       wmer_dB        = 0.0f;   // weighted MER on the FAC cells
        float       avgSNR_dB      = 0.0f;   // smoothed SNR estimate
        bool        snrAvailable   = false;

        // Champs décodés par le canal (incrément 2)
        // Indicatif émetteur, accumulé sur 4 trames FAC successives.
        char        callsign[9]    = { 0 };  // C-string, max 8 chars
        bool        callsignValid  = false;
        // MSC mode DRM (0=64-QAM SM, 1=64-QAM HMmix, 2=64-QAM HMsym, 3=16-QAM) ;
        // peut être étendu par les bits HM. -1 = inconnu.
        int         mscMode        = -1;
        // Profondeur d'entrelacement : 0 = long (2 s), 1 = short (400 ms). -1 = inconnu.
        int         interleaverDepth = -1;
        // Vrai lorsqu'au moins un train MSC a été décodé sans erreur de CRC.
        bool        mscAvailable   = false;

        // Progression de la réception (incrément 3)
        // Compteur de trames OFDM décodées depuis le démarrage de l'engine.
        // ~12 trames/s en mode A — utile pour visualiser que ça « tourne ».
        uint64_t    frameCounter   = 0;
        // Réception en cours d'un objet MOT (image).
        int         rxSegments     = 0;     // segments déjà reçus
        int         totalSegments  = 0;     // segments attendus (0 si inconnu)
        int         bodySize       = 0;     // taille totale en octets (0 si inconnue)
        int         segmentSize    = 0;     // taille d'un segment en octets
        char        objectName[80] = { 0 }; // nom du fichier en cours
        // Vrai si on est en train de recevoir un objet (au moins 1 segment).
        bool        receivingObject = false;

        // Diagnostics (pour aider à comprendre pourquoi l'image ne s'affiche pas)
        uint64_t    filesReceived       = 0; // total des fichiers passés par QFile WriteOnly
        uint64_t    imageDecodeFailures = 0; // décodages RGBA échoués
    };

    class DRMDecoder {
    public:
        DRMDecoder();
        ~DRMDecoder();

        // (Re)initialize the engine. Safe to call to clear acquisition state.
        void init();

        // Feed real audio samples at ENGINE_SAMPLE_RATE (12 kHz). Thread: DSP.
        void feed(const float* audio, int count);

        // If true, the imaginary branch of the analytic signal is negated,
        // i.e. the spectrum is mirrored. Use it if the wanted sideband ends up
        // inverted for your setup. (USB/LSB is normally selected upstream.)
        void setInvertSpectrum(bool inv) { invertSpectrum.store(inv); }
        bool getInvertSpectrum() const   { return invertSpectrum.load(); }

        // Thread-safe status snapshot for the UI.
        Status getStatus();

        // Copy the latest equalized OFDM cell constellation (decision points).
        // Returns the number of points written (<= maxPoints).
        int getConstellation(std::complex<float>* out, int maxPoints);

        // Copy the latest PSD (power spectral density), length 513, dB-ish.
        void getPSD(float* out513);

        // ===== Incrément 3 : récupération de l'image décodée =====

        // True si une nouvelle image a été reçue depuis le dernier markImageConsumed().
        bool hasNewImage();

        // Récupère les bytes bruts du dernier fichier image reçu et son nom.
        // Retourne true si une image est disponible.
        bool getLastImageBytes(std::vector<uint8_t>& outBytes, std::string& outName);

        // Marque la dernière image comme "consommée" (hasNewImage redeviendra false).
        void markImageConsumed();

        // Décode l'image courante en RGBA8 (4 octets par pixel, ordre RGBA).
        // outRGBA est redimensionné à width*height*4 octets.
        // Retourne true si décodage OK. Supporte JPEG, PNG, BMP, JPEG 2000.
        bool decodeImageRGBA(std::vector<uint8_t>& outRGBA, int& width, int& height);

        // À appeler quand un décodage d'image a échoué (incrémente le compteur
        // exposé dans Status.imageDecodeFailures).
        void noteImageDecodeFailure();

        // Format détecté du dernier fichier (chaîne courte: "jpeg", "png",
        // "bmp", "jp2", "?"). Utile pour libeller le bouton de sauvegarde.
        std::string getImageFormat();

    private:
        void buildHilbert();
        void pushComplexSample(float re, float im);

        // Hilbert FIR state
        std::vector<float> hTaps;     // odd-length Hilbert transformer taps
        std::vector<float> delayLine; // ring buffer of recent real samples
        int  hLen   = 0;
        int  hMid   = 0;
        int  dlPos  = 0;
        std::atomic<bool> invertSpectrum{ false };

        // Stripe accumulation (interleaved I/Q for the engine)
        std::vector<float> stripe;    // size 2*STRIPE_COMPLEX
        int  stripeCount = 0;

        // SNR smoothing (mirrors QSSTV drmRx::run avgSNR logic)
        float avgSNR = 0.0f;
        bool  avgSNRAvailable = false;

        std::mutex mtx;
        bool initialized = false;

        static std::mutex s_engineMtx;   // guards the global engine (singleton)
        bool ownsEngine = false;
    };

} // namespace drm
