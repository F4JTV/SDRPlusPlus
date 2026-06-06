# Compilation — drm_image_decoder (Ubuntu 24.04)

Procédure pour compiler le module en intégration dans l'arborescence SDR++.

## 1. Dépendances système

```bash
sudo apt update
sudo apt install -y build-essential cmake git \
    libfftw3-dev libopenjp2-7-dev libstb-dev \
    libglfw3-dev libvolk-dev libzstd-dev pkg-config
```

(`libglfw3-dev`, `libvolk-dev`, `libzstd-dev` sont les dépendances habituelles du
cœur SDR++ ; `libfftw3-dev` est requis par le moteur DRM ; `libopenjp2-7-dev`
décode le JPEG 2000 transmis par HamDRM ; `libstb-dev` fournit les en-têtes
single-header `stb_image.h` et `stb_image_write.h` pour JPEG/PNG/BMP.)

## 2. Récupérer SDR++

```bash
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus
```

## 3. Installer le module

Copiez le dossier `drm_image_decoder/` dans `decoder_modules/` :

```bash
# depuis le dossier contenant l'archive
tar xzf drm_image_decoder.tar.gz -C /chemin/vers/SDRPlusPlus/decoder_modules/
```

L'arborescence doit donner `SDRPlusPlus/decoder_modules/drm_image_decoder/CMakeLists.txt`.

## 4. Enregistrer le module dans le CMakeLists racine

Éditez `SDRPlusPlus/CMakeLists.txt`.

**(a)** Dans le bloc des options `OPT_BUILD_*_DECODER` (vers la ligne 52, à côté de
`OPT_BUILD_KG_SSTV_DECODER`), ajoutez :

```cmake
option(OPT_BUILD_DRM_IMAGE_DECODER "Build the HamDRM digital image (DRM SSTV) decoder (Dependencies: fftw3)" OFF)
```

**(b)** Dans le bloc des `add_subdirectory(...)` des décodeurs (vers la ligne 280,
à côté de `kg_sstv_decoder`), ajoutez :

```cmake
if (OPT_BUILD_DRM_IMAGE_DECODER)
add_subdirectory("decoder_modules/drm_image_decoder")
endif (OPT_BUILD_DRM_IMAGE_DECODER)
```

## 5. Compiler

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_DRM_IMAGE_DECODER=ON
make -j$(nproc)
```

Le module produit `build/decoder_modules/drm_image_decoder/drm_image_decoder.so`.

## 6. Installation

### Installation système (comme les autres plugins)

```bash
sudo cp decoder_modules/drm_image_decoder/drm_image_decoder.so /usr/lib/sdrpp/plugins/
```

### Ou installation locale (sans toucher au système)

Copiez le `.so` dans votre dossier de plugins SDR++ utilisateur, puis activez le
module dans **Module Manager** au premier lancement.

## 7. Mise à jour (recompilation après modification)

```bash
cd ~/SDRPlusPlus
# remplacer les sources du module
rm -rf decoder_modules/drm_image_decoder
tar xzf /chemin/vers/drm_image_decoder.tar.gz -C decoder_modules/

cd build
cmake ..
make -j$(nproc) drm_image_decoder
sudo cp decoder_modules/drm_image_decoder/drm_image_decoder.so /usr/lib/sdrpp/plugins/
sdrpp
```

## Remarques

- **FFTW double + simple précision.** Le moteur RXAMADRM utilise les deux ; sous
  Ubuntu, `libfftw3-dev` fournit `libfftw3` et `libfftw3f`. Le CMake du module lie
  les deux via `pkg-config`.
- **Instance unique.** Le moteur porte un état global ; le module est limité à une
  instance (déclaré dans `SDRPP_MOD_INFO`).
- **Windows / Android.** Le `CMakeLists.txt` prévoit les branches MSVC (vcpkg
  `FFTW3`/`FFTW3f`) et Android (`/sdr-kit/.../libfftw3*.so`), sur le modèle du
  module `dab_decoder`. La cible principale validée reste **Ubuntu 24.04**.
