# drm_image_decoder — Décodeur d'images numériques HamDRM pour SDR++

Module SDR++ de réception d'**images numériques HamDRM** (DRM bande étroite /
SSTV numérique, du type **EasyPal** / **DigTRX**, telles qu'on les rencontre sur
les bandes HF amateur en SSB).

La chaîne de réception est bâtie autour du moteur d'acquisition **RXAMADRM** de
**M. Bos (PA0MBO)**, vendorisé depuis **QSSTV** (ON4QZ), sous licence **GPL**.
Ce module en constitue la coque SDR++ : VFO, démodulation SSB, ré-échantillonnage
vers 12 kHz, et restitution de l'état d'acquisition.

> **Crédits / licence.** Le cœur DSP DRM (`src/engine/`) est dérivé de QSSTV
> (https://github.com/ON4QZ/QSSTV) — moteur RXAMADRM © M. Bos PA0MBO. Il reste
> sous **GPL**. La coque SDR++ (`src/main.cpp`, `src/drm_decoder.*`) est fournie
> par F4JTV (ADRASEC 06). En distribuant ce module, respectez la GPL.

---

## Feuille de route incrémentale

Le récepteur DRM complet de QSSTV (~10 000 lignes) est porté par paliers, selon la
même méthode que le module SSTV — chaque palier compile et se teste isolément.

| Incrément | Contenu | État |
|-----------|---------|------|
| **1 — Acquisition** | Détection du mode de robustesse (A/B/C/D), synchro temps/fréquence/trame, WMER/SNR, PSD, constellation des cellules OFDM | ✅ |
| **2 — Décodage canal** | FAC (callsign, MSC mode, occupation) + SDC + MSC + Viterbi + désentrelacement + démappage QAM + CRC | ✅ |
| **3 — Image** | Reconstruction des objets MOT, décodage **JPEG 2000 / JPEG / PNG / BMP**, affichage et sauvegarde | ✅ |

**Le décodeur HamDRM complet est fonctionnel.** Validé sur un enregistrement réel (Mode A, QAM-4, BW 2200 Hz, ADRASEC 06) : accrochage en 5 s, callsign décodé, MSC sans erreur CRC, image JP2 (953×927 pixels) reconstruite et affichée.

---

## Fonctionnalités

- VFO accordable sur le signal, démodulation sélectionnable : **USB** (défaut HamDRM), **LSB**, **NFM**
- **Inversion de spectre** (utile selon le câblage / la bande latérale)
- Largeur de bande de démodulation réglable (par mode)
- Indicateurs de **synchronisation** colorés : Temps, Fréquence, Trame, FAC
- **Mode de robustesse** détecté (A/B/C/D), **occupation spectrale**, **MSC mode**, **interleaver**
- **Callsign** de l'émetteur décodé via FAC
- Jauge **SNR / WMER** (estimés par le moteur DRM)
- **Diagramme de constellation** des cellules OFDM
- Tracé de la **densité spectrale (PSD)**
- **Image reçue** affichée en direct (JPEG 2000 / JPEG / PNG / BMP)
- **Sauvegarde** au format BMP, PNG, JPEG (ré-encodés depuis les pixels décodés) ou **Brut** (format natif reçu)
- Sauvegarde de la configuration (mode, inversion, largeur, low-pass NFM)

---

## Architecture

```
drm_image_decoder/
├── CMakeLists.txt          # Module SDR++ + moteur, lien FFTW
├── README.md
├── BUILDING.md             # Procédure de compilation Ubuntu 24
└── src/
    ├── main.cpp            # Coque SDR++ : VFO, SSB, resampler, UI
    ├── drm_decoder.h/.cpp  # Wrapper propre, sans Qt, autour du moteur
    │                       #   (Hilbert réel→I/Q, agrégation en stripes 1024,
    │                       #    snapshots d'état/constellation/PSD)
    └── engine/             # Moteur RXAMADRM vendorisé (sous-ensemble incr. 1)
        ├── qt_shim.h            # Remplace les dépendances Qt de QSSTV
        ├── sourcedecoder_stub.h # Stub MOT/image (réel en incrément 3)
        ├── demodulator.cpp      # OFDM : synchro, estimation de canal
        ├── getofdm*.cpp         # Acquisition OFDM
        ├── getmode.cpp          # Détection du mode de robustesse
        ├── newfft.cpp, psd*.cpp # FFT / PSD
        └── …                    # nrutil, filtres, etc.
```

Chaîne DSP :

```
VFO (complexe) ─▶ SSB (USB/LSB) ─▶ RationalResampler ─▶ 12 kHz réel
                                                            │
                                              Hilbert FIR (wrapper)
                                                            ▼
                                            moteur RXAMADRM (stripes 1024 I/Q)
```

Le moteur porte un **état global** (variables globales héritées de RXAMADRM) ; le
module est donc déclaré en **instance unique** (`Max instances = 1`).

---

## Dépendances

| Composant | Rôle |
|-----------|------|
| `libfftw3-dev` | FFT double + simple précision (moteur OFDM/PSD) |
| `libopenjp2-7-dev` | Décodage JPEG 2000 (format usuel HamDRM) |
| `libstb-dev` | stb_image / stb_image_write (JPEG, PNG, BMP) |
| SDR++ (core) | fourni par l'arborescence de build |

Sous Ubuntu 24.04 : `libfftw3-dev` fournit fftw3 + fftw3f ; `libopenjp2-7-dev`
fournit OpenJPEG 2.5 (paquet `libopenjp2`/`pkg-config libopenjp2`) ; `libstb-dev`
fournit les en-têtes `stb_image.h` et `stb_image_write.h` (single-header, pas
de linkage).

---

## Compilation et installation

Voir **[BUILDING.md](BUILDING.md)** pour la procédure complète sous Ubuntu 24.04
(intégration dans l'arborescence SDR++, options CMake, installation du `.so`).

---

## Utilisation

1. Lancez SDR++ ; le module **DRM Image Decoder** apparaît dans le menu de gauche.
2. Sélectionnez une source SDR et démarrez la réception.
3. Placez le VFO sur le signal HamDRM. Laissez la bande latérale sur **USB**
   (réglage habituel HamDRM) ; basculez en **LSB** ou activez **Inverser le
   spectre** si l'accrochage échoue.
4. Observez les indicateurs : lorsque **Temps**, **Fréquence** puis **Trame**
   passent au vert et que le **mode** (A/B/C/D) s'affiche, l'acquisition est
   réussie ; la constellation se resserre et le WMER monte.

> **Validation.** L'ensemble du pipeline a été vérifié de bout en bout sur un
> enregistrement HamDRM réel (Mode A, QAM-4, BW 2200 Hz, interleaving court,
> RS=none, fourni par F4JTV / ADRASEC 06) :
>
> - accrochage en **5,2 s** (Temps/Fréquence puis Trame/FAC),
> - mode de robustesse **A** détecté, occupation **4,5 kHz**, mscMode = 3,
> - **WMER ≈ 35 dB / SNR ≈ 35 dB** stable pendant toute la transmission (~155 s),
> - **callsign FAC décodé** : `TM06REF` (validé en 1,5 s après frame sync),
> - **MSC décodé sans erreur CRC** (Viterbi + désentrelacement + démappage QAM),
> - **constellation QAM-4** propre (256 points, répartition équilibrée),
> - **image JPEG 2000 reconstruite** : 27,3 ko, 953×927 pixels RGB
>   (calendrier ADRASEC 06 / avril 2026), décodage RGBA en < 1 s,
> - décodage temps réel global : **≈ 240× temps réel** (162 s d'audio en 0,7 s CPU).
